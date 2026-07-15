// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thin Linux queue and interrupt driver for the VAMS PCIe endpoint.
 * Public command submission remains deferred until request tracking exists.
 */

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "vams_abi.h"
#include "vams_regs.h"

#define VAMS_DRIVER_NAME "vams_pci"
#define VAMS_PCI_VENDOR_ID 0x1b36
#define VAMS_PCI_DEVICE_ID 0x1100

static_assert(sizeof(struct vams_submission) == VAMS_SUBMISSION_SIZE);
static_assert(sizeof(struct vams_completion) == VAMS_COMPLETION_SIZE);

enum vams_probe_step {
	VAMS_PROBE_AFTER_ENABLE = 1,
	VAMS_PROBE_AFTER_REGION,
	VAMS_PROBE_AFTER_IOMAP,
	VAMS_PROBE_AFTER_DMA_MASK,
	VAMS_PROBE_AFTER_RINGS,
	VAMS_PROBE_AFTER_VECTORS,
	VAMS_PROBE_AFTER_CQ_IRQ,
	VAMS_PROBE_AFTER_ASYNC_IRQ,
};

struct vams_device {
	struct pci_dev *pdev;
	void __iomem *bar0;
	u32 hw_if_version;
	u32 fw_version;
	u32 capabilities;
	u32 reset_generation;
	struct vams_submission *sq;
	dma_addr_t sq_dma;
	struct vams_completion *cq;
	dma_addr_t cq_dma;
	u32 sq_tail;
	u32 cq_head;
	bool queues_ready;
	/* Serializes CQ consumption between IRQ and future polling paths. */
	spinlock_t cq_lock;
	atomic64_t cq_interrupts;
	atomic64_t async_interrupts;
#ifdef CONFIG_VAMS_PCI_TESTING
	struct completion cq_test_completion;
	struct completion async_test_completion;
	struct completion nop_test_completion;
	struct vams_completion nop_test_result;
	u32 nop_test_command_id;
	bool nop_test_pending;
#endif
};

#ifdef CONFIG_VAMS_PCI_TESTING
static unsigned int probe_fail_step;
module_param(probe_fail_step, uint, 0400);
MODULE_PARM_DESC(probe_fail_step,
		 "test only: fail probe after resource acquisition step 1 through 8");

static bool probe_irq_selftest;
module_param(probe_irq_selftest, bool, 0400);
MODULE_PARM_DESC(probe_irq_selftest,
		 "test only: force and verify both MSI-X interrupt paths during probe");

static bool probe_nop_selftest;
module_param(probe_nop_selftest, bool, 0400);
MODULE_PARM_DESC(probe_nop_selftest,
		 "test only: submit and verify one NOP command during probe");

static int vams_maybe_fail_probe(struct vams_device *vdev,
				 enum vams_probe_step step)
{
	if (probe_fail_step != step)
		return 0;

	dev_info(&vdev->pdev->dev, "injecting probe failure at step %u\n",
		 probe_fail_step);
	return -EIO;
}
#else
static int vams_maybe_fail_probe(struct vams_device *vdev,
				 enum vams_probe_step step)
{
	return 0;
}
#endif

static u32 vams_readl(const struct vams_device *vdev, u32 offset)
{
	return ioread32(vdev->bar0 + offset);
}

static void vams_writel(const struct vams_device *vdev, u32 offset, u32 value)
{
	iowrite32(value, vdev->bar0 + offset);
}

static void vams_mask_interrupts(const struct vams_device *vdev)
{
	vams_writel(vdev, VAMS_REG_INTR_MASK, VAMS_INTR_ALL);
	/* Flush the posted mask write before IRQ teardown or status handling. */
	vams_readl(vdev, VAMS_REG_INTR_MASK);
}

static int vams_alloc_queues(struct vams_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;
	size_t cq_size = VAMS_QUEUE_DEPTH * sizeof(*vdev->cq);
	size_t sq_size = VAMS_QUEUE_DEPTH * sizeof(*vdev->sq);

	vdev->sq = dma_alloc_coherent(dev, sq_size, &vdev->sq_dma, GFP_KERNEL);
	if (!vdev->sq)
		return -ENOMEM;

	vdev->cq = dma_alloc_coherent(dev, cq_size, &vdev->cq_dma, GFP_KERNEL);
	if (!vdev->cq) {
		dma_free_coherent(dev, sq_size, vdev->sq, vdev->sq_dma);
		vdev->sq = NULL;
		return -ENOMEM;
	}

	if ((vdev->sq_dma & 63) || (vdev->cq_dma & 63)) {
		dev_err(dev, "coherent queue DMA addresses are not 64-byte aligned\n");
		dma_free_coherent(dev, cq_size, vdev->cq, vdev->cq_dma);
		dma_free_coherent(dev, sq_size, vdev->sq, vdev->sq_dma);
		vdev->cq = NULL;
		vdev->sq = NULL;
		return -EFAULT;
	}

	return 0;
}

static void vams_free_queues(struct vams_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;

	if (vdev->cq)
		dma_free_coherent(dev,
				  VAMS_QUEUE_DEPTH * sizeof(*vdev->cq),
				  vdev->cq, vdev->cq_dma);
	if (vdev->sq)
		dma_free_coherent(dev,
				  VAMS_QUEUE_DEPTH * sizeof(*vdev->sq),
				  vdev->sq, vdev->sq_dma);
	vdev->cq = NULL;
	vdev->sq = NULL;
}

static unsigned int vams_drain_cq(struct vams_device *vdev)
{
	unsigned int drained = 0;
	u32 tail;

	if (!vdev->queues_ready)
		return 0;

	spin_lock(&vdev->cq_lock);
	tail = vams_readl(vdev, VAMS_REG_CQ_TAIL);
	if (tail >= VAMS_QUEUE_DEPTH) {
		dev_err_ratelimited(&vdev->pdev->dev,
				    "invalid CQ tail %u\n", tail);
		spin_unlock(&vdev->cq_lock);
		return 0;
	}

	dma_rmb();
	while (vdev->cq_head != tail) {
		struct vams_completion completion;

		memcpy(&completion, &vdev->cq[vdev->cq_head], sizeof(completion));
		vdev->cq_head = (vdev->cq_head + 1) & (VAMS_QUEUE_DEPTH - 1);
		drained++;

#ifdef CONFIG_VAMS_PCI_TESTING
		if (vdev->nop_test_pending &&
		    le32_to_cpu(completion.command_id) ==
			    vdev->nop_test_command_id) {
			vdev->nop_test_result = completion;
			vdev->nop_test_pending = false;
			complete(&vdev->nop_test_completion);
			continue;
		}
#endif
		dev_warn_ratelimited(&vdev->pdev->dev,
				     "unexpected completion id %#x\n",
				     le32_to_cpu(completion.command_id));
	}

	if (drained) {
		dma_wmb();
		vams_writel(vdev, VAMS_REG_CQ_DOORBELL, vdev->cq_head);
	}
	spin_unlock(&vdev->cq_lock);
	return drained;
}

static void vams_disable_queues(struct vams_device *vdev);

static int vams_configure_queues(struct vams_device *vdev)
{
	u32 status;

	vams_writel(vdev, VAMS_REG_CQ_BASE_LO, lower_32_bits(vdev->cq_dma));
	vams_writel(vdev, VAMS_REG_CQ_BASE_HI, upper_32_bits(vdev->cq_dma));
	vams_writel(vdev, VAMS_REG_CQ_DEPTH, VAMS_QUEUE_DEPTH);
	vams_writel(vdev, VAMS_REG_SQ_BASE_LO, lower_32_bits(vdev->sq_dma));
	vams_writel(vdev, VAMS_REG_SQ_BASE_HI, upper_32_bits(vdev->sq_dma));
	vams_writel(vdev, VAMS_REG_SQ_DEPTH, VAMS_QUEUE_DEPTH);
	vams_writel(vdev, VAMS_REG_CQ_CONTROL, VAMS_QUEUE_ENABLE);
	vams_writel(vdev, VAMS_REG_SQ_CONTROL, VAMS_QUEUE_ENABLE);
	vdev->queues_ready = true;

	status = vams_readl(vdev, VAMS_REG_CQ_STATUS);
	if (!(status & VAMS_QUEUE_STATUS_ENABLED))
		goto err_reset;
	status = vams_readl(vdev, VAMS_REG_SQ_STATUS);
	if (!(status & VAMS_QUEUE_STATUS_ENABLED))
		goto err_reset;

	vdev->sq_tail = 0;
	vdev->cq_head = 0;
	vams_writel(vdev, VAMS_REG_DEVICE_CONTROL, VAMS_DEVICE_ENABLE);
	status = vams_readl(vdev, VAMS_REG_DEVICE_STATUS);
	if (!(status & VAMS_STATUS_QUEUES_READY))
		goto err_reset;

	return 0;

err_reset:
	vams_disable_queues(vdev);
	return -EIO;
}

static void vams_disable_queues(struct vams_device *vdev)
{
	if (!vdev->queues_ready)
		return;

	vams_writel(vdev, VAMS_REG_DEVICE_CONTROL, VAMS_DEVICE_QUIESCE);
	vams_writel(vdev, VAMS_REG_SQ_CONTROL, VAMS_QUEUE_RESET);
	vams_readl(vdev, VAMS_REG_DEVICE_STATUS);
	vdev->queues_ready = false;
}

static irqreturn_t vams_cq_irq(int irq, void *data)
{
	struct vams_device *vdev = data;
	u32 pending;

	pending = vams_readl(vdev, VAMS_REG_INTR_STATUS) & VAMS_INTR_CQ;
	if (!pending)
		return IRQ_NONE;

	vams_drain_cq(vdev);
	vams_writel(vdev, VAMS_REG_INTR_STATUS, pending);
	atomic64_inc(&vdev->cq_interrupts);
#ifdef CONFIG_VAMS_PCI_TESTING
	complete(&vdev->cq_test_completion);
#endif

	return IRQ_HANDLED;
}

static irqreturn_t vams_async_irq(int irq, void *data)
{
	struct vams_device *vdev = data;
	u32 pending;

	pending = vams_readl(vdev, VAMS_REG_INTR_STATUS) & VAMS_INTR_ASYNC;
	if (!pending)
		return IRQ_NONE;

	if (pending & VAMS_INTR_ERROR) {
		u32 error = vams_readl(vdev, VAMS_REG_ERROR_STATUS);
		u32 fatal = vams_readl(vdev, VAMS_REG_LAST_FATAL);

		dev_warn_ratelimited(&vdev->pdev->dev,
				     "device error status %#x, last fatal %#x\n",
				     error, fatal);
	}

	if (pending & VAMS_INTR_RESET_DONE)
		vdev->reset_generation =
			vams_readl(vdev, VAMS_REG_RESET_GENERATION);

	vams_writel(vdev, VAMS_REG_INTR_STATUS, pending);
	atomic64_inc(&vdev->async_interrupts);
#ifdef CONFIG_VAMS_PCI_TESTING
	complete(&vdev->async_test_completion);
#endif

	return IRQ_HANDLED;
}

static int vams_validate_endpoint(struct vams_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;
	u32 desc_version;
	u32 device_id;
	u32 status;
	u32 major;

	device_id = vams_readl(vdev, VAMS_REG_DEVICE_ID);
	if (device_id != VAMS_DEVICE_ID_VALUE) {
		dev_err(dev, "BAR0 identity mismatch: %#x\n", device_id);
		return -ENODEV;
	}

	vdev->hw_if_version = vams_readl(vdev, VAMS_REG_HW_IF_VERSION);
	major = vdev->hw_if_version >> 16;
	if (major != VAMS_HW_IF_MAJOR_SUPPORTED) {
		dev_err(dev, "unsupported hardware interface version %#x\n",
			vdev->hw_if_version);
		return -EPROTONOSUPPORT;
	}

	desc_version = vams_readl(vdev, VAMS_REG_DESC_VERSION);
	if (desc_version != VAMS_DESC_VERSION_SUPPORTED) {
		dev_err(dev, "unsupported descriptor version %u\n", desc_version);
		return -EPROTONOSUPPORT;
	}

	vdev->capabilities = vams_readl(vdev, VAMS_REG_CAPABILITIES);
	if (!(vdev->capabilities & VAMS_CAP_MSIX)) {
		dev_err(dev, "endpoint does not advertise required MSI-X support\n");
		return -ENODEV;
	}

	status = vams_readl(vdev, VAMS_REG_DEVICE_STATUS);
	if (!(status & VAMS_STATUS_READY) ||
	    (status & (VAMS_STATUS_RESETTING | VAMS_STATUS_FATAL))) {
		dev_err(dev, "endpoint is not ready: status %#x\n", status);
		return -EBUSY;
	}

	vdev->fw_version = vams_readl(vdev, VAMS_REG_FW_VERSION);
	vdev->reset_generation =
		vams_readl(vdev, VAMS_REG_RESET_GENERATION);

	return 0;
}

static int vams_set_dma_mask(struct vams_device *vdev)
{
	struct device *dev = &vdev->pdev->dev;
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (!ret)
		return 0;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(dev, "no usable coherent DMA mask\n");
		return ret;
	}

	dev_warn(dev, "using 32-bit DMA addressing\n");
	return 0;
}

#ifdef CONFIG_VAMS_PCI_TESTING
static int vams_irq_selftest(struct vams_device *vdev)
{
	unsigned long timeout;

	if (!probe_irq_selftest)
		return 0;

	reinit_completion(&vdev->cq_test_completion);
	vams_writel(vdev, VAMS_REG_INTR_FORCE, VAMS_INTR_CQ);
	vams_readl(vdev, VAMS_REG_INTR_STATUS);
	timeout = wait_for_completion_timeout(&vdev->cq_test_completion, HZ);
	if (!timeout) {
		dev_err(&vdev->pdev->dev, "CQ MSI-X self-test timed out\n");
		return -ETIMEDOUT;
	}

	reinit_completion(&vdev->async_test_completion);
	vams_writel(vdev, VAMS_REG_INTR_FORCE, VAMS_INTR_FW_EVENT);
	vams_readl(vdev, VAMS_REG_INTR_STATUS);
	timeout = wait_for_completion_timeout(&vdev->async_test_completion, HZ);
	if (!timeout) {
		dev_err(&vdev->pdev->dev, "async MSI-X self-test timed out\n");
		return -ETIMEDOUT;
	}

	dev_info(&vdev->pdev->dev, "MSI-X self-test passed\n");
	return 0;
}

static int vams_nop_selftest(struct vams_device *vdev)
{
	static const u32 command_id = 0x56414d53;
	static const u64 cookie = 0x1122334455667788ULL;
	struct vams_submission *submission;
	unsigned long timeout;

	if (!probe_nop_selftest)
		return 0;
	if (!vdev->queues_ready)
		return -EOPNOTSUPP;

	submission = &vdev->sq[vdev->sq_tail];
	memset(submission, 0, sizeof(*submission));
	submission->version = cpu_to_le16(VAMS_DESC_VERSION_1);
	submission->opcode = VAMS_OP_NOP;
	submission->command_id = cpu_to_le32(command_id);
	submission->user_cookie = cpu_to_le64(cookie);

	reinit_completion(&vdev->nop_test_completion);
	vdev->nop_test_command_id = command_id;
	vdev->nop_test_pending = true;
	dma_wmb();
	vdev->sq_tail = (vdev->sq_tail + 1) & (VAMS_QUEUE_DEPTH - 1);
	vams_writel(vdev, VAMS_REG_SQ_DOORBELL, vdev->sq_tail);
	timeout = wait_for_completion_timeout(&vdev->nop_test_completion, HZ);
	if (!timeout) {
		vdev->nop_test_pending = false;
		dev_err(&vdev->pdev->dev, "NOP completion timed out\n");
		return -ETIMEDOUT;
	}

	if (le32_to_cpu(vdev->nop_test_result.command_id) != command_id ||
	    le16_to_cpu(vdev->nop_test_result.status) != VAMS_STATUS_SUCCESS ||
	    le16_to_cpu(vdev->nop_test_result.error_code) != VAMS_ERR_NONE ||
	    le32_to_cpu(vdev->nop_test_result.bytes_processed) != 0 ||
	    le64_to_cpu(vdev->nop_test_result.user_cookie) != cookie) {
		dev_err(&vdev->pdev->dev, "NOP completion contents are invalid\n");
		return -EPROTO;
	}

	dev_info(&vdev->pdev->dev,
		 "NOP round trip passed: id=%#x cookie=%#llx\n",
		 command_id, cookie);
	return 0;
}
#else
static int vams_irq_selftest(struct vams_device *vdev)
{
	return 0;
}

static int vams_nop_selftest(struct vams_device *vdev)
{
	return 0;
}
#endif

static int vams_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct device *dev = &pdev->dev;
	struct vams_device *vdev;
	int ret;

	vdev = kzalloc_obj(struct vams_device);
	if (!vdev)
		return -ENOMEM;

	vdev->pdev = pdev;
	spin_lock_init(&vdev->cq_lock);
	atomic64_set(&vdev->cq_interrupts, 0);
	atomic64_set(&vdev->async_interrupts, 0);
#ifdef CONFIG_VAMS_PCI_TESTING
	init_completion(&vdev->cq_test_completion);
	init_completion(&vdev->async_test_completion);
	init_completion(&vdev->nop_test_completion);
#endif

	ret = pci_enable_device_mem(pdev);
	if (ret)
		goto err_free_device;

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_ENABLE);
	if (ret)
		goto err_disable_device;

	if (!(pci_resource_flags(pdev, 0) & IORESOURCE_MEM) ||
	    pci_resource_len(pdev, 0) < VAMS_BAR0_SIZE) {
		dev_err(dev, "BAR0 is not a %u-byte memory resource\n",
			VAMS_BAR0_SIZE);
		ret = -ENODEV;
		goto err_disable_device;
	}

	ret = pci_request_region(pdev, 0, VAMS_DRIVER_NAME);
	if (ret)
		goto err_disable_device;

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_REGION);
	if (ret)
		goto err_release_region;

	vdev->bar0 = pci_iomap(pdev, 0, VAMS_BAR0_SIZE);
	if (!vdev->bar0) {
		ret = -ENOMEM;
		goto err_release_region;
	}

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_IOMAP);
	if (ret)
		goto err_iounmap;

	vams_mask_interrupts(vdev);
	vams_writel(vdev, VAMS_REG_INTR_STATUS, VAMS_INTR_ALL);

	ret = vams_validate_endpoint(vdev);
	if (ret)
		goto err_iounmap;

	ret = vams_set_dma_mask(vdev);
	if (ret)
		goto err_iounmap;

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_DMA_MASK);
	if (ret)
		goto err_iounmap;

	if (vdev->capabilities & VAMS_CAP_DMA) {
		ret = vams_alloc_queues(vdev);
		if (ret)
			goto err_iounmap;
	}

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_RINGS);
	if (ret)
		goto err_free_queues;

	ret = pci_alloc_irq_vectors(pdev, VAMS_MSIX_VECTORS,
				    VAMS_MSIX_VECTORS, PCI_IRQ_MSIX);
	if (ret < 0)
		goto err_free_queues;
	if (ret != VAMS_MSIX_VECTORS) {
		dev_err(dev, "expected %u MSI-X vectors, received %d\n",
			VAMS_MSIX_VECTORS, ret);
		ret = -ENOSPC;
		goto err_free_vectors;
	}

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_VECTORS);
	if (ret)
		goto err_free_vectors;

	ret = request_irq(pci_irq_vector(pdev, VAMS_MSIX_CQ_VECTOR),
			  vams_cq_irq, 0, "vams-cq", vdev);
	if (ret)
		goto err_free_vectors;

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_CQ_IRQ);
	if (ret)
		goto err_free_cq_irq;

	ret = request_irq(pci_irq_vector(pdev, VAMS_MSIX_ASYNC_VECTOR),
			  vams_async_irq, 0, "vams-async", vdev);
	if (ret)
		goto err_free_cq_irq;

	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_ASYNC_IRQ);
	if (ret)
		goto err_free_async_irq;

	pci_set_drvdata(pdev, vdev);
	/* MSI-X messages require device-initiated memory writes even before DMA. */
	pci_set_master(pdev);
	if (vdev->capabilities & VAMS_CAP_DMA) {
		ret = vams_configure_queues(vdev);
		if (ret) {
			dev_err(dev, "queue configuration failed\n");
			goto err_clear_master;
		}
	}
	vams_writel(vdev, VAMS_REG_INTR_MASK, 0);
	vams_readl(vdev, VAMS_REG_INTR_MASK);

	ret = vams_irq_selftest(vdev);
	if (ret)
		goto err_clear_master;
	ret = vams_nop_selftest(vdev);
	if (ret)
		goto err_clear_master;

	dev_info(dev,
		 "ready: hw_if=%u.%u fw=%#x caps=%#x dma=%u-bit generation=%u\n",
		 vdev->hw_if_version >> 16, vdev->hw_if_version & 0xffff,
		 vdev->fw_version, (u32)(vdev->capabilities & VAMS_CAP_KNOWN),
		 dma_get_mask(dev) > DMA_BIT_MASK(32) ? 64 : 32,
		 vdev->reset_generation);
	if (!(vdev->capabilities & VAMS_CAP_DMA))
		dev_info(dev, "DMA queues unavailable; bound in discovery mode\n");
	else
		dev_info(dev, "coherent SQ/CQ ready: depth=%u\n",
			 VAMS_QUEUE_DEPTH);

	return 0;

err_clear_master:
	vams_mask_interrupts(vdev);
	vams_disable_queues(vdev);
	pci_clear_master(pdev);
	pci_set_drvdata(pdev, NULL);
err_free_async_irq:
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_ASYNC_VECTOR), vdev);
err_free_cq_irq:
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_CQ_VECTOR), vdev);
err_free_vectors:
	pci_free_irq_vectors(pdev);
err_free_queues:
	vams_free_queues(vdev);
err_iounmap:
	pci_iounmap(pdev, vdev->bar0);
err_release_region:
	pci_release_region(pdev, 0);
err_disable_device:
	pci_disable_device(pdev);
err_free_device:
	kfree(vdev);
	return ret;
}

static void vams_remove(struct pci_dev *pdev)
{
	struct vams_device *vdev = pci_get_drvdata(pdev);

	vams_mask_interrupts(vdev);
	vams_writel(vdev, VAMS_REG_INTR_STATUS, VAMS_INTR_ALL);
	vams_disable_queues(vdev);
	pci_clear_master(pdev);
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_ASYNC_VECTOR), vdev);
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_CQ_VECTOR), vdev);
	pci_free_irq_vectors(pdev);
	vams_free_queues(vdev);
	pci_iounmap(pdev, vdev->bar0);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(vdev);
}

static const struct pci_device_id vams_pci_ids[] = {
	{ PCI_DEVICE(VAMS_PCI_VENDOR_ID, VAMS_PCI_DEVICE_ID) },
	{ }
};
MODULE_DEVICE_TABLE(pci, vams_pci_ids);

static struct pci_driver vams_pci_driver = {
	.name = VAMS_DRIVER_NAME,
	.id_table = vams_pci_ids,
	.probe = vams_probe,
	.remove = vams_remove,
};
module_pci_driver(vams_pci_driver);

MODULE_AUTHOR("VAMS contributors");
MODULE_DESCRIPTION("Virtual Accelerator Management Subsystem PCIe driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1.0");

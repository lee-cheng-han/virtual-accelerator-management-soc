// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thin Linux discovery and interrupt driver for the VAMS PCIe endpoint.
 * Queue allocation and command submission are added with the queue ABI.
 */

#include <linux/completion.h>
#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>

#include "vams_regs.h"

#define VAMS_DRIVER_NAME "vams_pci"
#define VAMS_PCI_VENDOR_ID 0x1b36
#define VAMS_PCI_DEVICE_ID 0x1100

enum vams_probe_step {
	VAMS_PROBE_AFTER_ENABLE = 1,
	VAMS_PROBE_AFTER_REGION,
	VAMS_PROBE_AFTER_IOMAP,
	VAMS_PROBE_AFTER_DMA_MASK,
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
	atomic64_t cq_interrupts;
	atomic64_t async_interrupts;
#ifdef CONFIG_VAMS_PCI_TESTING
	struct completion cq_test_completion;
	struct completion async_test_completion;
#endif
};

#ifdef CONFIG_VAMS_PCI_TESTING
static unsigned int probe_fail_step;
module_param(probe_fail_step, uint, 0400);
MODULE_PARM_DESC(probe_fail_step,
		 "test only: fail probe after resource acquisition step 1 through 7");

static bool probe_irq_selftest;
module_param(probe_irq_selftest, bool, 0400);
MODULE_PARM_DESC(probe_irq_selftest,
		 "test only: force and verify both MSI-X interrupt paths during probe");

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

static irqreturn_t vams_cq_irq(int irq, void *data)
{
	struct vams_device *vdev = data;
	u32 pending;

	pending = vams_readl(vdev, VAMS_REG_INTR_STATUS) & VAMS_INTR_CQ;
	if (!pending)
		return IRQ_NONE;

	/* Queue draining will precede this W1C when the CQ is implemented. */
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
#else
static int vams_irq_selftest(struct vams_device *vdev)
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
	atomic64_set(&vdev->cq_interrupts, 0);
	atomic64_set(&vdev->async_interrupts, 0);
#ifdef CONFIG_VAMS_PCI_TESTING
	init_completion(&vdev->cq_test_completion);
	init_completion(&vdev->async_test_completion);
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

	ret = pci_alloc_irq_vectors(pdev, VAMS_MSIX_VECTORS,
				    VAMS_MSIX_VECTORS, PCI_IRQ_MSIX);
	if (ret < 0)
		goto err_iounmap;
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
	vams_writel(vdev, VAMS_REG_INTR_MASK, 0);
	vams_readl(vdev, VAMS_REG_INTR_MASK);

	ret = vams_irq_selftest(vdev);
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

	return 0;

err_clear_master:
	vams_mask_interrupts(vdev);
	pci_clear_master(pdev);
	pci_set_drvdata(pdev, NULL);
err_free_async_irq:
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_ASYNC_VECTOR), vdev);
err_free_cq_irq:
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_CQ_VECTOR), vdev);
err_free_vectors:
	pci_free_irq_vectors(pdev);
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
	pci_clear_master(pdev);
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_ASYNC_VECTOR), vdev);
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_CQ_VECTOR), vdev);
	pci_free_irq_vectors(pdev);
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

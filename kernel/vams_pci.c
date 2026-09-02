// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux queue, interrupt, registered-buffer, and asynchronous host-API driver
 * for the VAMS PCIe endpoint.
 */

#include <linux/completion.h>
#include <linux/compat.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/pci.h>
#include <linux/poll.h>
#include <linux/refcount.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>

#include "include/uapi/linux/vams.h"
#include "vams_abi.h"
#include "vams_regs.h"

#define VAMS_DRIVER_NAME "vams_pci"
#define VAMS_PCI_VENDOR_ID 0x1b36
#define VAMS_PCI_DEVICE_ID 0x1100
#define VAMS_CQ_POLL_INTERVAL_MS 10U
#define VAMS_NOP_WAIT_MS 1000U
#define VAMS_MAX_TRANSFER (16U * 1024U * 1024U)

static_assert(sizeof(struct vams_submission) == VAMS_SUBMISSION_SIZE);
static_assert(sizeof(struct vams_completion) == VAMS_COMPLETION_SIZE);
static_assert(sizeof(struct vams_ioc_info) == 32);
static_assert(sizeof(struct vams_ioc_nop) == 56);
static_assert(sizeof(struct vams_ioc_buffer_register) == 40);
static_assert(sizeof(struct vams_ioc_buffer_unregister) == 24);
static_assert(sizeof(struct vams_ioc_submit) == 64);
static_assert(sizeof(struct vams_ioc_wait) == 64);

static DEFINE_IDA(vams_instance_ida);

enum vams_probe_step {
	VAMS_PROBE_AFTER_ENABLE = 1,
	VAMS_PROBE_AFTER_REGION,
	VAMS_PROBE_AFTER_IOMAP,
	VAMS_PROBE_AFTER_DMA_MASK,
	VAMS_PROBE_AFTER_RINGS,
	VAMS_PROBE_AFTER_VECTORS,
	VAMS_PROBE_AFTER_CQ_IRQ,
	VAMS_PROBE_AFTER_ASYNC_IRQ,
	VAMS_PROBE_AFTER_CHARDEV,
};

struct vams_device;
struct vams_file;

struct vams_mapping {
	struct kref refs;
	struct vams_device *vdev;
	struct page **pages;
	struct sg_table sgt;
	dma_addr_t dma_addr;
	enum dma_data_direction direction;
	unsigned int npages;
	u64 user_address;
	u64 length;
	u32 flags;
	u32 handle;
};

struct vams_request {
	struct completion done;
	refcount_t refs;
	struct vams_completion result;
	struct vams_file *owner;
	struct vams_mapping *source;
	struct vams_mapping *destination;
	int driver_status;
	u32 command_id;
	u32 reset_generation;
};

struct vams_file {
	struct kref refs;
	struct vams_device *vdev;
	/* Serializes this file's mapping and reapable-request ownership. */
	struct mutex lock;
	struct xarray mappings;
	struct xarray requests;
	wait_queue_head_t waitq;
	atomic_t completed_requests;
	bool closing;
};

struct vams_device {
	struct pci_dev *pdev;
	void __iomem *bar0;
	u32 hw_if_version;
	u32 fw_version;
	u32 capabilities;
	u32 reset_generation;
	u32 reset_reason;
	struct vams_submission *sq;
	dma_addr_t sq_dma;
	struct vams_completion *cq;
	dma_addr_t cq_dma;
	u32 sq_tail;
	u32 cq_head;
	bool queues_ready;
	bool removing;
	struct mutex submit_lock;
	/* Serializes CQ consumption between IRQ and future polling paths. */
	spinlock_t cq_lock;
	/* Orders SQ publication against reset-generation interrupt handling. */
	spinlock_t reset_lock;
	struct xarray requests;
	atomic_t next_command_id;
	atomic_t pending_requests;
	struct delayed_work cq_poll_work;
	struct work_struct reset_work;
	struct miscdevice miscdev;
	struct kref refs;
	char *misc_name;
	int instance;
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
		 "test only: fail probe after resource acquisition step 1 through 9");

static bool probe_irq_selftest;
module_param(probe_irq_selftest, bool, 0400);
MODULE_PARM_DESC(probe_irq_selftest,
		 "test only: force and verify both MSI-X interrupt paths during probe");

static bool probe_nop_selftest;
module_param(probe_nop_selftest, bool, 0400);
MODULE_PARM_DESC(probe_nop_selftest,
		 "test only: submit and verify one NOP command during probe");

static bool probe_poll_selftest;
module_param(probe_poll_selftest, bool, 0400);
MODULE_PARM_DESC(probe_poll_selftest,
		 "test only: verify completion polling with CQ interrupts masked");

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

static void vams_device_release(struct kref *refs);

static void vams_mapping_release(struct kref *refs)
{
	struct vams_mapping *mapping =
		container_of(refs, struct vams_mapping, refs);
	struct device *dev = &mapping->vdev->pdev->dev;

	dma_unmap_sg(dev, mapping->sgt.sgl, mapping->sgt.orig_nents,
		     mapping->direction);
	sg_free_table(&mapping->sgt);
	if (mapping->flags & VAMS_BUFFER_WRITE)
		unpin_user_pages_dirty_lock(mapping->pages, mapping->npages,
					    true);
	else
		unpin_user_pages(mapping->pages, mapping->npages);
	kfree(mapping->pages);
	kfree(mapping);
}

static void vams_mapping_get(struct vams_mapping *mapping)
{
	kref_get(&mapping->refs);
}

static void vams_mapping_put(struct vams_mapping *mapping)
{
	if (mapping)
		kref_put(&mapping->refs, vams_mapping_release);
}

static void vams_file_release_refs(struct kref *refs)
{
	struct vams_file *vfile = container_of(refs, struct vams_file, refs);

	xa_destroy(&vfile->requests);
	xa_destroy(&vfile->mappings);
	kref_put(&vfile->vdev->refs, vams_device_release);
	kfree(vfile);
}

static struct vams_request *vams_request_alloc(struct vams_file *owner)
{
	struct vams_request *request;

	request = kzalloc_obj(struct vams_request);
	if (!request)
		return NULL;

	init_completion(&request->done);
	refcount_set(&request->refs, 1);
	request->owner = owner;
	if (owner)
		kref_get(&owner->refs);
	return request;
}

static void vams_request_get(struct vams_request *request)
{
	refcount_inc(&request->refs);
}

static void vams_request_put(struct vams_request *request)
{
	if (refcount_dec_and_test(&request->refs)) {
		vams_mapping_put(request->source);
		vams_mapping_put(request->destination);
		if (request->owner)
			kref_put(&request->owner->refs, vams_file_release_refs);
		kfree(request);
	}
}

static void vams_request_sync_for_cpu(struct vams_request *request)
{
	struct device *dev;

	if (!request->source && !request->destination)
		return;
	dev = &request->owner->vdev->pdev->dev;
	if (request->source)
		dma_sync_sg_for_cpu(dev, request->source->sgt.sgl,
				    request->source->sgt.orig_nents,
				    request->source->direction);
	if (request->destination && request->destination != request->source)
		dma_sync_sg_for_cpu(dev, request->destination->sgt.sgl,
				    request->destination->sgt.orig_nents,
				    request->destination->direction);
}

static bool vams_finish_request(struct vams_device *vdev,
				const struct vams_completion *completion)
{
	struct vams_request *request;
	u32 command_id = le32_to_cpu(completion->command_id);

	request = xa_erase(&vdev->requests, command_id);
	if (!request)
		return false;

	request->result = *completion;
	request->driver_status = 0;
	vams_request_sync_for_cpu(request);
	complete_all(&request->done);
	if (request->owner && !READ_ONCE(request->owner->closing)) {
		atomic_inc(&request->owner->completed_requests);
		wake_up_interruptible(&request->owner->waitq);
	}
	atomic_dec(&vdev->pending_requests);
	vams_request_put(request);
	return true;
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
		if (vams_finish_request(vdev, &completion))
			continue;
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

static void vams_observe_reset(struct vams_device *vdev)
{
	unsigned long flags;
	u32 generation = vams_readl(vdev, VAMS_REG_RESET_GENERATION);
	u32 reason;

	if (generation == READ_ONCE(vdev->reset_generation))
		return;
	reason = vams_readl(vdev, VAMS_REG_LAST_RESET_REASON);
	spin_lock_irqsave(&vdev->reset_lock, flags);
	if (generation != READ_ONCE(vdev->reset_generation)) {
		WRITE_ONCE(vdev->reset_reason, reason);
		WRITE_ONCE(vdev->reset_generation, generation);
		schedule_work(&vdev->reset_work);
	}
	spin_unlock_irqrestore(&vdev->reset_lock, flags);
}

static void vams_cq_poll_work(struct work_struct *work)
{
	struct vams_device *vdev =
		container_of(to_delayed_work(work), struct vams_device,
			     cq_poll_work);

	if (READ_ONCE(vdev->removing))
		return;

	vams_observe_reset(vdev);
	vams_drain_cq(vdev);
	if (atomic_read(&vdev->pending_requests) > 0)
		schedule_delayed_work(&vdev->cq_poll_work,
				      msecs_to_jiffies(VAMS_CQ_POLL_INTERVAL_MS));
}

static void vams_cancel_requests(struct vams_device *vdev, int status,
				 bool stale_only)
{
	struct vams_request *request;
	unsigned long command_id;

	xa_for_each(&vdev->requests, command_id, request) {
		if (stale_only && request->reset_generation ==
		    READ_ONCE(vdev->reset_generation))
			continue;
		request = xa_erase(&vdev->requests, command_id);
		if (!request)
			continue;
		request->driver_status = status;
		vams_request_sync_for_cpu(request);
		complete_all(&request->done);
		if (request->owner && !READ_ONCE(request->owner->closing)) {
			atomic_inc(&request->owner->completed_requests);
			wake_up_interruptible(&request->owner->waitq);
		}
		atomic_dec(&vdev->pending_requests);
		vams_request_put(request);
	}
}

static void vams_reset_work(struct work_struct *work)
{
	struct vams_device *vdev =
		container_of(work, struct vams_device, reset_work);

	mutex_lock(&vdev->submit_lock);
	if (!vdev->removing) {
		if (READ_ONCE(vdev->reset_reason) ==
			    VAMS_RESET_REASON_HOST_DEVICE ||
		    READ_ONCE(vdev->reset_reason) ==
			    VAMS_RESET_REASON_HOST_QUEUE)
			vdev->queues_ready = false;
		vams_cancel_requests(vdev, -ECANCELED, true);
	}
	mutex_unlock(&vdev->submit_lock);
}

static void vams_disable_queues(struct vams_device *vdev);

static int vams_configure_queues(struct vams_device *vdev)
{
	u32 status;

	vams_writel(vdev, VAMS_REG_CQ_BASE_LO, lower_32_bits(vdev->cq_dma));
	vams_writel(vdev, VAMS_REG_CQ_BASE_HI, upper_32_bits(vdev->cq_dma));
	vams_writel(vdev, VAMS_REG_CQ_DEPTH, VAMS_QUEUE_DEPTH);
	if (vdev->capabilities & VAMS_CAP_CQ_WATERMARK)
		vams_writel(vdev, VAMS_REG_CQ_WATERMARK,
			    VAMS_CQ_WATERMARK_HIGH << 16 |
			    VAMS_CQ_WATERMARK_LOW);
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
		vams_observe_reset(vdev);

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

static int vams_track_request(struct vams_device *vdev,
			      struct vams_request *request)
{
	unsigned int attempt;
	int ret;

	for (attempt = 0; attempt < VAMS_QUEUE_DEPTH * 2; attempt++) {
		u32 command_id = (u32)atomic_inc_return(&vdev->next_command_id);

		if (!command_id)
			continue;
		request->command_id = command_id;
		if (request->owner) {
			ret = xa_insert(&request->owner->requests, command_id,
					request, GFP_KERNEL);
			if (ret == -EBUSY)
				continue;
			if (ret)
				return ret;
		}
		vams_request_get(request);
		ret = xa_insert(&vdev->requests, command_id, request, GFP_KERNEL);
		if (!ret)
			return 0;
		vams_request_put(request);
		if (request->owner)
			xa_erase(&request->owner->requests, command_id);
		if (ret != -EBUSY)
			return ret;
	}

	return -ENOSPC;
}

static void vams_request_sync_for_device(struct vams_request *request)
{
	struct device *dev;

	if (!request->source && !request->destination)
		return;
	dev = &request->owner->vdev->pdev->dev;
	if (request->source)
		dma_sync_sg_for_device(dev, request->source->sgt.sgl,
				       request->source->sgt.orig_nents,
				       request->source->direction);
	if (request->destination && request->destination != request->source)
		dma_sync_sg_for_device(dev, request->destination->sgt.sgl,
				       request->destination->sgt.orig_nents,
				       request->destination->direction);
}

static int vams_submit(struct vams_device *vdev,
		       struct vams_request *request,
		       const struct vams_submission *descriptor)
{
	struct vams_submission *submission;
	unsigned long flags;
	u32 next_tail;
	u32 sq_head;
	int ret;

	mutex_lock(&vdev->submit_lock);
	if (vdev->removing) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (!vdev->queues_ready) {
		ret = -EOPNOTSUPP;
		goto out_unlock;
	}
	vams_drain_cq(vdev);

	sq_head = vams_readl(vdev, VAMS_REG_SQ_HEAD);
	if (sq_head >= VAMS_QUEUE_DEPTH) {
		ret = -EIO;
		goto out_unlock;
	}
	next_tail = (vdev->sq_tail + 1) & (VAMS_QUEUE_DEPTH - 1);
	if (next_tail == sq_head) {
		ret = -EAGAIN;
		goto out_unlock;
	}

	ret = vams_track_request(vdev, request);
	if (ret)
		goto out_unlock;

	submission = &vdev->sq[vdev->sq_tail];
	*submission = *descriptor;
	submission->command_id = cpu_to_le32(request->command_id);
	vams_request_sync_for_device(request);
	spin_lock_irqsave(&vdev->reset_lock, flags);
	request->reset_generation = READ_ONCE(vdev->reset_generation);
	atomic_inc(&vdev->pending_requests);
	dma_wmb();
	vdev->sq_tail = next_tail;
	vams_writel(vdev, VAMS_REG_SQ_DOORBELL, vdev->sq_tail);
	spin_unlock_irqrestore(&vdev->reset_lock, flags);
	if (atomic_read(&vdev->pending_requests) > 0)
		schedule_delayed_work(&vdev->cq_poll_work,
				      msecs_to_jiffies(VAMS_CQ_POLL_INTERVAL_MS));
	ret = 0;

out_unlock:
	mutex_unlock(&vdev->submit_lock);
	return ret;
}

static int vams_submit_nop(struct vams_device *vdev,
			   struct vams_request *request, u64 user_cookie,
			   u32 timeout_ms)
{
	struct vams_submission descriptor = {
		.version = cpu_to_le16(VAMS_DESC_VERSION_1),
		.opcode = VAMS_OP_NOP,
		.timeout_ms = cpu_to_le32(timeout_ms),
		.user_cookie = cpu_to_le64(user_cookie),
	};

	return vams_submit(vdev, request, &descriptor);
}

static void vams_device_release(struct kref *refs)
{
	struct vams_device *vdev =
		container_of(refs, struct vams_device, refs);

	xa_destroy(&vdev->requests);
	if (vdev->instance >= 0)
		ida_free(&vams_instance_ida, vdev->instance);
	kfree(vdev->misc_name);
	pci_dev_put(vdev->pdev);
	kfree(vdev);
}

static int vams_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct vams_device *vdev =
		container_of(miscdev, struct vams_device, miscdev);
	struct vams_file *vfile;
	int ret = 0;

	vfile = kzalloc_obj(struct vams_file);
	if (!vfile)
		return -ENOMEM;
	kref_init(&vfile->refs);
	vfile->vdev = vdev;
	mutex_init(&vfile->lock);
	xa_init_flags(&vfile->mappings, XA_FLAGS_ALLOC1);
	xa_init(&vfile->requests);
	init_waitqueue_head(&vfile->waitq);
	atomic_set(&vfile->completed_requests, 0);

	mutex_lock(&vdev->submit_lock);
	if (vdev->removing) {
		ret = -ENODEV;
	} else {
		kref_get(&vdev->refs);
		file->private_data = vfile;
	}
	mutex_unlock(&vdev->submit_lock);
	if (ret) {
		xa_destroy(&vfile->requests);
		xa_destroy(&vfile->mappings);
		kfree(vfile);
	}
	return ret;
}

static int vams_release(struct inode *inode, struct file *file)
{
	struct vams_file *vfile = file->private_data;
	struct vams_mapping *mapping;
	struct vams_request *request;
	unsigned long index;

	mutex_lock(&vfile->lock);
	vfile->closing = true;
	xa_for_each(&vfile->requests, index, request) {
		request = xa_erase(&vfile->requests, index);
		if (request)
			vams_request_put(request);
	}
	xa_for_each(&vfile->mappings, index, mapping) {
		mapping = xa_erase(&vfile->mappings, index);
		if (mapping)
			vams_mapping_put(mapping);
	}
	mutex_unlock(&vfile->lock);
	wake_up_interruptible(&vfile->waitq);
	kref_put(&vfile->refs, vams_file_release_refs);
	return 0;
}

static enum dma_data_direction vams_mapping_direction(u32 flags)
{
	if (flags == VAMS_BUFFER_READ)
		return DMA_TO_DEVICE;
	if (flags == VAMS_BUFFER_WRITE)
		return DMA_FROM_DEVICE;
	return DMA_BIDIRECTIONAL;
}

static long vams_ioctl_buffer_register(struct vams_file *vfile,
				       void __user *argp)
{
	struct vams_ioc_buffer_register input;
	struct vams_mapping *mapping;
	unsigned long first;
	unsigned long last;
	unsigned long end;
	unsigned int gup_flags = FOLL_LONGTERM;
	unsigned int npages;
	long pinned;
	u32 handle;
	int mapped;
	int ret;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (input.size != sizeof(input) ||
	    input.version != VAMS_UAPI_VERSION || input.handle ||
	    !input.flags || (input.flags & ~VAMS_BUFFER_FLAGS) ||
	    !input.length || input.length > VAMS_MAX_TRANSFER ||
	    input.reserved || input.user_address > ULONG_MAX ||
	    input.length > ULONG_MAX)
		return -EINVAL;
	first = (unsigned long)input.user_address;
	if (check_add_overflow(first, (unsigned long)input.length, &end) ||
	    !access_ok((void __user *)first, (unsigned long)input.length))
		return -EFAULT;
	last = end - 1;
	npages = (last >> PAGE_SHIFT) - (first >> PAGE_SHIFT) + 1;
	mutex_lock(&vfile->lock);
	mutex_lock(&vfile->vdev->submit_lock);
	if (vfile->closing || vfile->vdev->removing) {
		ret = -ENODEV;
		goto err_unlock_lifecycle;
	}

	mapping = kzalloc_obj(struct vams_mapping);
	if (!mapping) {
		ret = -ENOMEM;
		goto err_unlock_lifecycle;
	}
	mapping->pages = kcalloc(npages, sizeof(*mapping->pages), GFP_KERNEL);
	if (!mapping->pages) {
		ret = -ENOMEM;
		goto err_free_mapping;
	}
	mapping->vdev = vfile->vdev;
	mapping->npages = npages;
	mapping->user_address = input.user_address;
	mapping->length = input.length;
	mapping->flags = input.flags;
	mapping->direction = vams_mapping_direction(input.flags);
	kref_init(&mapping->refs);
	if (input.flags & VAMS_BUFFER_WRITE)
		gup_flags |= FOLL_WRITE;
	pinned = pin_user_pages_fast(first & PAGE_MASK, npages, gup_flags,
				     mapping->pages);
	if (pinned != npages) {
		if (pinned > 0)
			unpin_user_pages(mapping->pages, (unsigned long)pinned);
		ret = pinned < 0 ? (int)pinned : -EFAULT;
		goto err_free_pages;
	}
	ret = sg_alloc_table_from_pages(&mapping->sgt, mapping->pages, npages,
					first & ~PAGE_MASK, input.length,
					GFP_KERNEL);
	if (ret)
		goto err_unpin;
	mapped = dma_map_sg(&vfile->vdev->pdev->dev, mapping->sgt.sgl,
			    mapping->sgt.orig_nents, mapping->direction);
	if (!mapped) {
		ret = -EIO;
		goto err_free_sg;
	}
	if (mapped != 1) {
		ret = -ERANGE;
		goto err_unmap;
	}
	if (sg_dma_len(mapping->sgt.sgl) < input.length) {
		ret = -ERANGE;
		goto err_unmap;
	}
	mapping->dma_addr = sg_dma_address(mapping->sgt.sgl);

	ret = xa_alloc(&vfile->mappings, &handle, mapping,
		       XA_LIMIT(1, UINT_MAX), GFP_KERNEL);
	if (!ret) {
		mapping->handle = handle;
		input.handle = handle;
	}
	if (ret)
		goto err_unmap;
	if (copy_to_user(argp, &input, sizeof(input))) {
		xa_erase(&vfile->mappings, handle);
		vams_mapping_put(mapping);
		mutex_unlock(&vfile->vdev->submit_lock);
		mutex_unlock(&vfile->lock);
		return -EFAULT;
	}
	mutex_unlock(&vfile->vdev->submit_lock);
	mutex_unlock(&vfile->lock);
	return 0;

err_unmap:
	dma_unmap_sg(&vfile->vdev->pdev->dev, mapping->sgt.sgl,
		     mapping->sgt.orig_nents, mapping->direction);
err_free_sg:
	sg_free_table(&mapping->sgt);
err_unpin:
	unpin_user_pages(mapping->pages, npages);
err_free_pages:
	kfree(mapping->pages);
err_free_mapping:
	kfree(mapping);
err_unlock_lifecycle:
	mutex_unlock(&vfile->vdev->submit_lock);
	mutex_unlock(&vfile->lock);
	return ret;
}

static long vams_ioctl_buffer_unregister(struct vams_file *vfile,
					 void __user *argp)
{
	struct vams_ioc_buffer_unregister input;
	struct vams_mapping *mapping;
	int ret = 0;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (input.size != sizeof(input) ||
	    input.version != VAMS_UAPI_VERSION || !input.handle || input.flags ||
	    input.reserved)
		return -EINVAL;

	mutex_lock(&vfile->lock);
	mapping = xa_load(&vfile->mappings, input.handle);
	if (!mapping)
		ret = -ENOENT;
	else if (refcount_read(&mapping->refs.refcount) != 1)
		ret = -EBUSY;
	else
		xa_erase(&vfile->mappings, input.handle);
	mutex_unlock(&vfile->lock);
	if (!ret)
		vams_mapping_put(mapping);
	return ret;
}

static struct vams_mapping *
vams_mapping_lookup_locked(struct vams_file *vfile, u32 handle, u64 offset,
			   u32 length, u32 required_flags,
			   dma_addr_t *dma_address)
{
	struct vams_mapping *mapping;
	dma_addr_t dma_end;
	u64 end;

	if (!handle || check_add_overflow(offset, (u64)length, &end))
		return ERR_PTR(-EINVAL);
	mapping = xa_load(&vfile->mappings, handle);
	if (!mapping)
		return ERR_PTR(-ENOENT);
	if ((mapping->flags & required_flags) != required_flags)
		return ERR_PTR(-EACCES);
	if (end > mapping->length)
		return ERR_PTR(-ERANGE);
	if (check_add_overflow(mapping->dma_addr, (dma_addr_t)offset,
			       dma_address) || !*dma_address ||
	    check_add_overflow(*dma_address, (dma_addr_t)length, &dma_end))
		return ERR_PTR(-EOVERFLOW);
	vams_mapping_get(mapping);
	return mapping;
}

static long vams_ioctl_submit(struct vams_file *vfile, void __user *argp)
{
	struct vams_ioc_submit input;
	struct vams_submission descriptor = { 0 };
	struct vams_mapping *source = NULL;
	struct vams_mapping *destination = NULL;
	struct vams_request *request;
	dma_addr_t source_dma = 0;
	dma_addr_t destination_dma = 0;
	u32 source_access = 0;
	u32 destination_access = 0;
	u32 source_length = 0;
	int ret;

	if (copy_from_user(&input, argp, sizeof(input)))
		return -EFAULT;
	if (input.size != sizeof(input) ||
	    input.version != VAMS_UAPI_VERSION || input.command_id ||
	    input.timeout_ms > 60000U ||
	    input.length > VAMS_MAX_TRANSFER ||
	    (input.flags & ~VAMS_SUBMIT_VERIFY_CRC) ||
	    (input.opcode != VAMS_OP_CRC32 && input.flags) ||
	    ((!input.flags || input.opcode != VAMS_OP_CRC32) &&
	     input.expected_crc))
		return -EINVAL;

	switch (input.opcode) {
	case VAMS_OP_NOP:
		if (input.source_handle || input.destination_handle ||
		    input.source_offset || input.destination_offset || input.length)
			return -EINVAL;
		break;
	case VAMS_OP_MEM_COPY:
		if (!input.length || !input.source_handle ||
		    !input.destination_handle)
			return -EINVAL;
		source_access = VAMS_BUFFER_READ;
		destination_access = VAMS_BUFFER_WRITE;
		source_length = input.length;
		break;
	case VAMS_OP_MEM_FILL:
		if (!input.length || !input.source_handle ||
		    !input.destination_handle)
			return -EINVAL;
		source_access = VAMS_BUFFER_READ;
		destination_access = VAMS_BUFFER_WRITE;
		source_length = 1;
		break;
	case VAMS_OP_CRC32:
		if (!input.length || !input.source_handle ||
		    input.destination_handle || input.destination_offset)
			return -EINVAL;
		source_access = VAMS_BUFFER_READ;
		source_length = input.length;
		break;
	case VAMS_OP_VECTOR_ADD:
		if (input.length < sizeof(u32) ||
		    (input.length & (sizeof(u32) - 1)) ||
		    !input.source_handle || !input.destination_handle)
			return -EINVAL;
		source_access = VAMS_BUFFER_READ;
		destination_access = VAMS_BUFFER_READ | VAMS_BUFFER_WRITE;
		source_length = input.length;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&vfile->lock);
	if (vfile->closing) {
		ret = -ENODEV;
		goto out_unlock;
	}
	if (source_access) {
		source = vams_mapping_lookup_locked(vfile, input.source_handle,
					    input.source_offset,
					    source_length, source_access,
					    &source_dma);
		if (IS_ERR(source)) {
			ret = PTR_ERR(source);
			source = NULL;
			goto out_unlock;
		}
	}
	if (destination_access) {
		destination = vams_mapping_lookup_locked(vfile,
				input.destination_handle,
				input.destination_offset, input.length,
				destination_access, &destination_dma);
		if (IS_ERR(destination)) {
			ret = PTR_ERR(destination);
			destination = NULL;
			goto out_put_source;
		}
	}
	if (source_dma && destination_dma && input.opcode != VAMS_OP_MEM_FILL &&
	    source_dma < destination_dma + input.length &&
	    destination_dma < source_dma + input.length) {
		ret = -EINVAL;
		goto out_put_mappings;
	}

	request = vams_request_alloc(vfile);
	if (!request) {
		ret = -ENOMEM;
		goto out_put_mappings;
	}
	request->source = source;
	request->destination = destination;
	source = NULL;
	destination = NULL;
	descriptor.version = cpu_to_le16(VAMS_DESC_VERSION_1);
	descriptor.opcode = input.opcode;
	descriptor.flags = input.flags;
	descriptor.source_dma = cpu_to_le64(source_dma);
	descriptor.destination_dma = cpu_to_le64(destination_dma);
	descriptor.length = cpu_to_le32(input.length);
	descriptor.timeout_ms = cpu_to_le32(input.timeout_ms);
	descriptor.user_cookie = cpu_to_le64(input.user_cookie);
	descriptor.expected_crc = cpu_to_le32(input.expected_crc);
	ret = vams_submit(vfile->vdev, request, &descriptor);
	if (ret) {
		vams_request_put(request);
		goto out_unlock;
	}
	input.command_id = request->command_id;
	if (copy_to_user(argp, &input, sizeof(input))) {
		xa_erase(&vfile->requests, request->command_id);
		if (completion_done(&request->done))
			atomic_dec_if_positive(&vfile->completed_requests);
		vams_request_put(request);
		ret = -EFAULT;
	} else {
		ret = 0;
	}
	mutex_unlock(&vfile->lock);
	return ret;

out_put_mappings:
	vams_mapping_put(destination);
out_put_source:
	vams_mapping_put(source);
out_unlock:
	mutex_unlock(&vfile->lock);
	return ret;
}

static long vams_ioctl_wait(struct vams_file *vfile, void __user *argp)
{
	struct vams_ioc_wait output;
	struct vams_request *request;
	long waited;
	int ret = 0;

	if (copy_from_user(&output, argp, sizeof(output)))
		return -EFAULT;
	if (output.size != sizeof(output) ||
	    output.version != VAMS_UAPI_VERSION || !output.command_id ||
	    output.flags || output.timeout_ms > 60000U || output.driver_status ||
	    output.status || output.error_code || output.bytes_processed ||
	    output.result_crc || output.reserved0 || output.user_cookie ||
	    output.device_timestamp || output.reserved1)
		return -EINVAL;

	mutex_lock(&vfile->lock);
	request = xa_load(&vfile->requests, output.command_id);
	if (request)
		vams_request_get(request);
	mutex_unlock(&vfile->lock);
	if (!request)
		return -ENOENT;

	if (!output.timeout_ms) {
		if (!completion_done(&request->done)) {
			ret = -EAGAIN;
			goto out_put;
		}
	} else {
		waited = wait_for_completion_interruptible_timeout(&request->done,
				msecs_to_jiffies(output.timeout_ms));
		if (waited < 0) {
			ret = waited;
			goto out_put;
		}
		if (!waited) {
			ret = -ETIMEDOUT;
			goto out_put;
		}
	}

	output.driver_status = request->driver_status;
	output.status = le16_to_cpu(request->result.status);
	output.error_code = le16_to_cpu(request->result.error_code);
	output.bytes_processed =
		le32_to_cpu(request->result.bytes_processed);
	output.result_crc = le32_to_cpu(request->result.result_crc);
	output.user_cookie = le64_to_cpu(request->result.user_cookie);
	output.device_timestamp =
		le64_to_cpu(request->result.device_timestamp);

	mutex_lock(&vfile->lock);
	if (xa_load(&vfile->requests, output.command_id) != request) {
		ret = -EALREADY;
	} else if (copy_to_user(argp, &output, sizeof(output))) {
		ret = -EFAULT;
	} else {
		xa_erase(&vfile->requests, output.command_id);
		atomic_dec_if_positive(&vfile->completed_requests);
		vams_request_put(request);
	}
	mutex_unlock(&vfile->lock);

out_put:
	vams_request_put(request);
	return ret;
}

static long vams_ioctl_info(struct vams_device *vdev, void __user *argp)
{
	struct vams_ioc_info info;

	if (copy_from_user(&info, argp, sizeof(info)))
		return -EFAULT;
	if (info.size != sizeof(info) || info.version != VAMS_UAPI_VERSION ||
	    info.reserved)
		return -EINVAL;

	mutex_lock(&vdev->submit_lock);
	if (vdev->removing) {
		mutex_unlock(&vdev->submit_lock);
		return -ENODEV;
	}
	info.hw_if_version = vdev->hw_if_version;
	info.fw_version = vdev->fw_version;
	info.capabilities = vdev->capabilities & VAMS_CAP_KNOWN;
	info.queue_depth = vdev->queues_ready ? VAMS_QUEUE_DEPTH : 0;
	info.reset_generation = vdev->reset_generation;
	mutex_unlock(&vdev->submit_lock);

	if (copy_to_user(argp, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static long vams_ioctl_nop(struct vams_device *vdev, void __user *argp)
{
	struct vams_request *request;
	struct vams_ioc_nop nop;
	long waited;
	int ret;

	if (copy_from_user(&nop, argp, sizeof(nop)))
		return -EFAULT;
	if (nop.size != sizeof(nop) || nop.version != VAMS_UAPI_VERSION ||
	    nop.flags || nop.reserved || nop.timeout_ms > 60000U)
		return -EINVAL;

	request = vams_request_alloc(NULL);
	if (!request)
		return -ENOMEM;
	ret = vams_submit_nop(vdev, request, nop.user_cookie, nop.timeout_ms);
	if (ret)
		goto out_put;

	waited = wait_for_completion_interruptible_timeout(
		&request->done, msecs_to_jiffies(VAMS_NOP_WAIT_MS));
	if (waited < 0) {
		ret = waited;
		goto out_put;
	}
	if (!waited) {
		ret = -ETIMEDOUT;
		goto out_put;
	}
	if (request->driver_status) {
		ret = request->driver_status;
		goto out_put;
	}

	nop.command_id = request->command_id;
	nop.status = le16_to_cpu(request->result.status);
	nop.error_code = le16_to_cpu(request->result.error_code);
	nop.bytes_processed = le32_to_cpu(request->result.bytes_processed);
	nop.result_crc = le32_to_cpu(request->result.result_crc);
	nop.device_timestamp =
		le64_to_cpu(request->result.device_timestamp);
	if (copy_to_user(argp, &nop, sizeof(nop))) {
		ret = -EFAULT;
		goto out_put;
	}
	ret = 0;

out_put:
	vams_request_put(request);
	return ret;
}

static long vams_ioctl(struct file *file, unsigned int command,
		       unsigned long argument)
{
	struct vams_file *vfile = file->private_data;
	struct vams_device *vdev = vfile->vdev;
	void __user *argp = (void __user *)argument;

	switch (command) {
	case VAMS_IOCTL_GET_INFO:
		return vams_ioctl_info(vdev, argp);
	case VAMS_IOCTL_NOP:
		return vams_ioctl_nop(vdev, argp);
	case VAMS_IOCTL_BUFFER_REGISTER:
		return vams_ioctl_buffer_register(vfile, argp);
	case VAMS_IOCTL_BUFFER_UNREGISTER:
		return vams_ioctl_buffer_unregister(vfile, argp);
	case VAMS_IOCTL_SUBMIT:
		return vams_ioctl_submit(vfile, argp);
	case VAMS_IOCTL_WAIT:
		return vams_ioctl_wait(vfile, argp);
	default:
		return -ENOTTY;
	}
}

static __poll_t vams_poll(struct file *file, poll_table *wait)
{
	struct vams_file *vfile = file->private_data;
	struct vams_device *vdev = vfile->vdev;
	__poll_t mask = 0;
	u32 sq_head;
	u32 next_tail;

	poll_wait(file, &vfile->waitq, wait);
	if (atomic_read(&vfile->completed_requests) > 0)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (READ_ONCE(vfile->closing) || READ_ONCE(vdev->removing))
		return mask | EPOLLERR | EPOLLHUP;
	if (!READ_ONCE(vdev->queues_ready))
		return mask | EPOLLERR;
	sq_head = vams_readl(vdev, VAMS_REG_SQ_HEAD);
	next_tail = (READ_ONCE(vdev->sq_tail) + 1) &
		(VAMS_QUEUE_DEPTH - 1);
	if (sq_head < VAMS_QUEUE_DEPTH && next_tail != sq_head)
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static const struct file_operations vams_fops = {
	.owner = THIS_MODULE,
	.open = vams_open,
	.release = vams_release,
	.poll = vams_poll,
	.unlocked_ioctl = vams_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
};

static int vams_register_chardev(struct vams_device *vdev)
{
	int ret;

	vdev->instance = ida_alloc(&vams_instance_ida, GFP_KERNEL);
	if (vdev->instance < 0)
		return vdev->instance;
	vdev->misc_name = kasprintf(GFP_KERNEL, "vams%d", vdev->instance);
	if (!vdev->misc_name) {
		ret = -ENOMEM;
		goto err_free_instance;
	}

	vdev->miscdev.minor = MISC_DYNAMIC_MINOR;
	vdev->miscdev.name = vdev->misc_name;
	vdev->miscdev.fops = &vams_fops;
	vdev->miscdev.parent = &vdev->pdev->dev;
	ret = misc_register(&vdev->miscdev);
	if (ret)
		goto err_free_name;
	return 0;

err_free_name:
	kfree(vdev->misc_name);
	vdev->misc_name = NULL;
err_free_instance:
	ida_free(&vams_instance_ida, vdev->instance);
	vdev->instance = -1;
	return ret;
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

static int vams_poll_selftest(struct vams_device *vdev)
{
	static const u64 cookie = 0x504f4c4c5f4e4f50ULL;
	struct vams_request *request;
	unsigned long timeout;
	int ret;

	if (!probe_poll_selftest)
		return 0;
	request = vams_request_alloc(NULL);
	if (!request)
		return -ENOMEM;

	vams_writel(vdev, VAMS_REG_INTR_MASK, VAMS_INTR_CQ);
	vams_readl(vdev, VAMS_REG_INTR_MASK);
	ret = vams_submit_nop(vdev, request, cookie, 0);
	if (ret)
		goto out_unmask;
	timeout = wait_for_completion_timeout(&request->done, HZ);
	if (!timeout) {
		ret = -ETIMEDOUT;
		goto out_unmask;
	}
	ret = request->driver_status;
	if (!ret &&
	    (le16_to_cpu(request->result.status) != VAMS_STATUS_SUCCESS ||
	     le16_to_cpu(request->result.error_code) != VAMS_ERR_NONE ||
	     le64_to_cpu(request->result.user_cookie) != cookie))
		ret = -EPROTO;
	if (!ret)
		dev_info(&vdev->pdev->dev,
			 "CQ polling fallback self-test passed\n");

out_unmask:
	vams_writel(vdev, VAMS_REG_INTR_STATUS, VAMS_INTR_CQ);
	vams_writel(vdev, VAMS_REG_INTR_MASK, 0);
	vams_readl(vdev, VAMS_REG_INTR_MASK);
	vams_request_put(request);
	return ret;
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

static int vams_poll_selftest(struct vams_device *vdev)
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

	vdev->pdev = pci_dev_get(pdev);
	vdev->instance = -1;
	kref_init(&vdev->refs);
	mutex_init(&vdev->submit_lock);
	spin_lock_init(&vdev->cq_lock);
	spin_lock_init(&vdev->reset_lock);
	xa_init_flags(&vdev->requests, XA_FLAGS_LOCK_IRQ);
	atomic_set(&vdev->next_command_id, 0);
	atomic_set(&vdev->pending_requests, 0);
	INIT_DELAYED_WORK(&vdev->cq_poll_work, vams_cq_poll_work);
	INIT_WORK(&vdev->reset_work, vams_reset_work);
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
	ret = vams_poll_selftest(vdev);
	if (ret)
		goto err_clear_master;
	ret = vams_register_chardev(vdev);
	if (ret) {
		dev_err(dev, "character device registration failed\n");
		goto err_clear_master;
	}
	ret = vams_maybe_fail_probe(vdev, VAMS_PROBE_AFTER_CHARDEV);
	if (ret)
		goto err_deregister_misc;

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
	dev_info(dev, "host API ready: /dev/%s version=%u\n",
		 vdev->misc_name, VAMS_UAPI_VERSION);

	return 0;

err_deregister_misc:
	mutex_lock(&vdev->submit_lock);
	vdev->removing = true;
	mutex_unlock(&vdev->submit_lock);
	misc_deregister(&vdev->miscdev);
	cancel_delayed_work_sync(&vdev->cq_poll_work);
err_clear_master:
	vams_mask_interrupts(vdev);
	vams_disable_queues(vdev);
	pci_clear_master(pdev);
	pci_set_drvdata(pdev, NULL);
err_free_async_irq:
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_ASYNC_VECTOR), vdev);
err_free_cq_irq:
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_CQ_VECTOR), vdev);
	cancel_work_sync(&vdev->reset_work);
	vams_cancel_requests(vdev, -ENODEV, false);
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
	kref_put(&vdev->refs, vams_device_release);
	return ret;
}

static void vams_remove(struct pci_dev *pdev)
{
	struct vams_device *vdev = pci_get_drvdata(pdev);

	mutex_lock(&vdev->submit_lock);
	vdev->removing = true;
	mutex_unlock(&vdev->submit_lock);
	misc_deregister(&vdev->miscdev);
	cancel_delayed_work_sync(&vdev->cq_poll_work);
	vams_mask_interrupts(vdev);
	vams_writel(vdev, VAMS_REG_INTR_STATUS, VAMS_INTR_ALL);
	vams_disable_queues(vdev);
	pci_clear_master(pdev);
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_ASYNC_VECTOR), vdev);
	free_irq(pci_irq_vector(pdev, VAMS_MSIX_CQ_VECTOR), vdev);
	cancel_work_sync(&vdev->reset_work);
	vams_cancel_requests(vdev, -ENODEV, false);
	pci_free_irq_vectors(pdev);
	vams_free_queues(vdev);
	pci_iounmap(pdev, vdev->bar0);
	pci_release_region(pdev, 0);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kref_put(&vdev->refs, vams_device_release);
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

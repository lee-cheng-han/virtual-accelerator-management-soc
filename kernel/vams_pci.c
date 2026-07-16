// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thin Linux queue, interrupt, and host-API driver for the VAMS PCIe endpoint.
 * Payload DMA and asynchronous userspace submission remain deferred.
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
#include <linux/pci.h>
#include <linux/refcount.h>
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

static_assert(sizeof(struct vams_submission) == VAMS_SUBMISSION_SIZE);
static_assert(sizeof(struct vams_completion) == VAMS_COMPLETION_SIZE);
static_assert(sizeof(struct vams_ioc_info) == 32);
static_assert(sizeof(struct vams_ioc_nop) == 56);

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

struct vams_request {
	struct completion done;
	refcount_t refs;
	struct vams_completion result;
	int driver_status;
	u32 command_id;
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
	bool removing;
	struct mutex submit_lock;
	/* Serializes CQ consumption between IRQ and future polling paths. */
	spinlock_t cq_lock;
	struct xarray requests;
	atomic_t next_command_id;
	atomic_t pending_requests;
	struct delayed_work cq_poll_work;
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

static struct vams_request *vams_request_alloc(void)
{
	struct vams_request *request;

	request = kzalloc_obj(struct vams_request);
	if (!request)
		return NULL;

	init_completion(&request->done);
	refcount_set(&request->refs, 1);
	return request;
}

static void vams_request_get(struct vams_request *request)
{
	refcount_inc(&request->refs);
}

static void vams_request_put(struct vams_request *request)
{
	if (refcount_dec_and_test(&request->refs))
		kfree(request);
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
	complete(&request->done);
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

static void vams_cq_poll_work(struct work_struct *work)
{
	struct vams_device *vdev =
		container_of(to_delayed_work(work), struct vams_device,
			     cq_poll_work);

	if (READ_ONCE(vdev->removing))
		return;

	vams_drain_cq(vdev);
	if (atomic_read(&vdev->pending_requests) > 0)
		schedule_delayed_work(&vdev->cq_poll_work,
				      msecs_to_jiffies(VAMS_CQ_POLL_INTERVAL_MS));
}

static void vams_cancel_requests(struct vams_device *vdev, int status)
{
	struct vams_request *request;
	unsigned long command_id;

	xa_for_each(&vdev->requests, command_id, request) {
		request = xa_erase(&vdev->requests, command_id);
		if (!request)
			continue;
		request->driver_status = status;
		complete(&request->done);
		atomic_dec(&vdev->pending_requests);
		vams_request_put(request);
	}
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
		vams_request_get(request);
		ret = xa_insert(&vdev->requests, command_id, request, GFP_KERNEL);
		if (!ret)
			return 0;
		vams_request_put(request);
		if (ret != -EBUSY)
			return ret;
	}

	return -ENOSPC;
}

static int vams_submit_nop(struct vams_device *vdev,
			   struct vams_request *request, u64 user_cookie,
			   u32 timeout_ms)
{
	struct vams_submission *submission;
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
	memset(submission, 0, sizeof(*submission));
	submission->version = cpu_to_le16(VAMS_DESC_VERSION_1);
	submission->opcode = VAMS_OP_NOP;
	submission->command_id = cpu_to_le32(request->command_id);
	submission->timeout_ms = cpu_to_le32(timeout_ms);
	submission->user_cookie = cpu_to_le64(user_cookie);
	atomic_inc(&vdev->pending_requests);
	dma_wmb();
	vdev->sq_tail = next_tail;
	vams_writel(vdev, VAMS_REG_SQ_DOORBELL, vdev->sq_tail);
	if (atomic_read(&vdev->pending_requests) > 0)
		schedule_delayed_work(&vdev->cq_poll_work,
				      msecs_to_jiffies(VAMS_CQ_POLL_INTERVAL_MS));
	ret = 0;

out_unlock:
	mutex_unlock(&vdev->submit_lock);
	return ret;
}

static void vams_device_release(struct kref *refs)
{
	struct vams_device *vdev =
		container_of(refs, struct vams_device, refs);

	xa_destroy(&vdev->requests);
	if (vdev->instance >= 0)
		ida_free(&vams_instance_ida, vdev->instance);
	kfree(vdev->misc_name);
	kfree(vdev);
}

static int vams_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct vams_device *vdev =
		container_of(miscdev, struct vams_device, miscdev);
	int ret = 0;

	mutex_lock(&vdev->submit_lock);
	if (vdev->removing) {
		ret = -ENODEV;
	} else {
		kref_get(&vdev->refs);
		file->private_data = vdev;
	}
	mutex_unlock(&vdev->submit_lock);
	return ret;
}

static int vams_release(struct inode *inode, struct file *file)
{
	struct vams_device *vdev = file->private_data;

	kref_put(&vdev->refs, vams_device_release);
	return 0;
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

	request = vams_request_alloc();
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
	struct vams_device *vdev = file->private_data;
	void __user *argp = (void __user *)argument;

	switch (command) {
	case VAMS_IOCTL_GET_INFO:
		return vams_ioctl_info(vdev, argp);
	case VAMS_IOCTL_NOP:
		return vams_ioctl_nop(vdev, argp);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations vams_fops = {
	.owner = THIS_MODULE,
	.open = vams_open,
	.release = vams_release,
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
	request = vams_request_alloc();
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

	vdev->pdev = pdev;
	vdev->instance = -1;
	kref_init(&vdev->refs);
	mutex_init(&vdev->submit_lock);
	spin_lock_init(&vdev->cq_lock);
	xa_init_flags(&vdev->requests, XA_FLAGS_LOCK_IRQ);
	atomic_set(&vdev->next_command_id, 0);
	atomic_set(&vdev->pending_requests, 0);
	INIT_DELAYED_WORK(&vdev->cq_poll_work, vams_cq_poll_work);
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
	vams_cancel_requests(vdev, -ENODEV);
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
	vams_cancel_requests(vdev, -ENODEV);
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

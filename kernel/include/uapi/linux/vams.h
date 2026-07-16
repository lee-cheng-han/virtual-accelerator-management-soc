/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_VAMS_H
#define _UAPI_LINUX_VAMS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VAMS_UAPI_VERSION 1U
#define VAMS_IOCTL_MAGIC 'V'

struct vams_ioc_info {
	__u32 size;
	__u32 version;
	__u32 hw_if_version;
	__u32 fw_version;
	__u32 capabilities;
	__u32 queue_depth;
	__u32 reset_generation;
	__u32 reserved;
};

struct vams_ioc_nop {
	__u32 size;
	__u32 version;
	__u32 flags;
	__u32 timeout_ms;
	__u64 user_cookie;
	__u32 command_id;
	__u16 status;
	__u16 error_code;
	__u32 bytes_processed;
	__u32 result_crc;
	__u64 device_timestamp;
	__u64 reserved;
};

#define VAMS_IOCTL_GET_INFO \
	_IOWR(VAMS_IOCTL_MAGIC, 0x00, struct vams_ioc_info)
#define VAMS_IOCTL_NOP \
	_IOWR(VAMS_IOCTL_MAGIC, 0x01, struct vams_ioc_nop)

#endif /* _UAPI_LINUX_VAMS_H */

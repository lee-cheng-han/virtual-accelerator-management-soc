/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_VAMS_H
#define _UAPI_LINUX_VAMS_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define VAMS_UAPI_VERSION 1U
#define VAMS_IOCTL_MAGIC 'V'

#define VAMS_BUFFER_READ (1U << 0)
#define VAMS_BUFFER_WRITE (1U << 1)
#define VAMS_BUFFER_FLAGS (VAMS_BUFFER_READ | VAMS_BUFFER_WRITE)

#define VAMS_OP_NOP 0x0000U
#define VAMS_OP_MEM_COPY 0x0001U
#define VAMS_OP_MEM_FILL 0x0002U
#define VAMS_OP_CRC32 0x0003U
#define VAMS_OP_VECTOR_ADD 0x0004U

#define VAMS_SUBMIT_VERIFY_CRC (1U << 0)

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

struct vams_ioc_buffer_register {
	__u32 size;
	__u32 version;
	__u32 flags;
	__u32 handle;
	__u64 user_address;
	__u64 length;
	__u64 reserved;
};

struct vams_ioc_buffer_unregister {
	__u32 size;
	__u32 version;
	__u32 handle;
	__u32 flags;
	__u64 reserved;
};

struct vams_ioc_submit {
	__u32 size;
	__u32 version;
	__u16 opcode;
	__u16 flags;
	__u32 source_handle;
	__u32 destination_handle;
	__u32 length;
	__u32 timeout_ms;
	__u64 source_offset;
	__u64 destination_offset;
	__u64 user_cookie;
	__u32 expected_crc;
	__u32 command_id;
};

struct vams_ioc_wait {
	__u32 size;
	__u32 version;
	__u32 command_id;
	__u32 flags;
	__u32 timeout_ms;
	__s32 driver_status;
	__u16 status;
	__u16 error_code;
	__u32 bytes_processed;
	__u32 result_crc;
	__u32 reserved0;
	__u64 user_cookie;
	__u64 device_timestamp;
	__u64 reserved1;
};

#define VAMS_IOCTL_GET_INFO \
	_IOWR(VAMS_IOCTL_MAGIC, 0x00, struct vams_ioc_info)
#define VAMS_IOCTL_NOP \
	_IOWR(VAMS_IOCTL_MAGIC, 0x01, struct vams_ioc_nop)
#define VAMS_IOCTL_BUFFER_REGISTER \
	_IOWR(VAMS_IOCTL_MAGIC, 0x02, struct vams_ioc_buffer_register)
#define VAMS_IOCTL_BUFFER_UNREGISTER \
	_IOW(VAMS_IOCTL_MAGIC, 0x03, struct vams_ioc_buffer_unregister)
#define VAMS_IOCTL_SUBMIT \
	_IOWR(VAMS_IOCTL_MAGIC, 0x04, struct vams_ioc_submit)
#define VAMS_IOCTL_WAIT \
	_IOWR(VAMS_IOCTL_MAGIC, 0x05, struct vams_ioc_wait)

#endif /* _UAPI_LINUX_VAMS_H */

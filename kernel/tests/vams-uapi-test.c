/* SPDX-License-Identifier: MIT */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <pthread.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <linux/vams.h>

#define THREAD_COUNT 4U
#define COMMANDS_PER_THREAD 8U
#define COMMAND_COUNT (THREAD_COUNT * COMMANDS_PER_THREAD)
#define RESET_TEST_COMMANDS 8U
#define VAMS_BAR0_SIZE 0x1000U
#define VAMS_DEVICE_CONTROL_OFFSET 0x020U
#define VAMS_DEVICE_RESET (1U << 1)

_Static_assert(sizeof(struct vams_ioc_info) == 32,
	       "vams_ioc_info ABI size changed");
_Static_assert(sizeof(struct vams_ioc_nop) == 56,
	       "vams_ioc_nop ABI size changed");
_Static_assert(offsetof(struct vams_ioc_nop, user_cookie) == 16,
	       "vams_ioc_nop user_cookie offset changed");
_Static_assert(offsetof(struct vams_ioc_nop, device_timestamp) == 40,
	       "vams_ioc_nop device_timestamp offset changed");
_Static_assert(sizeof(struct vams_ioc_buffer_register) == 40,
	       "vams_ioc_buffer_register ABI size changed");
_Static_assert(sizeof(struct vams_ioc_buffer_unregister) == 24,
	       "vams_ioc_buffer_unregister ABI size changed");
_Static_assert(sizeof(struct vams_ioc_submit) == 64,
	       "vams_ioc_submit ABI size changed");
_Static_assert(sizeof(struct vams_ioc_wait) == 64,
	       "vams_ioc_wait ABI size changed");

static uint32_t crc32_ieee(const uint8_t *data, size_t length)
{
	uint32_t crc = UINT32_MAX;
	size_t index;

	for (index = 0; index < length; index++) {
		unsigned int bit;

		crc ^= data[index];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^
				(UINT32_C(0xedb88320) & (0U - (crc & 1U)));
	}
	return ~crc;
}

static int register_buffer(int fd, void *address, size_t length, uint32_t flags,
			   uint32_t *handle)
{
	struct vams_ioc_buffer_register input = {
		.size = sizeof(input),
		.version = VAMS_UAPI_VERSION,
		.flags = flags,
		.user_address = (uintptr_t)address,
		.length = length,
	};

	if (ioctl(fd, VAMS_IOCTL_BUFFER_REGISTER, &input) < 0)
		return errno;
	if (!input.handle)
		return EPROTO;
	*handle = input.handle;
	return 0;
}

static int unregister_buffer(int fd, uint32_t handle)
{
	struct vams_ioc_buffer_unregister input = {
		.size = sizeof(input),
		.version = VAMS_UAPI_VERSION,
		.handle = handle,
	};

	return ioctl(fd, VAMS_IOCTL_BUFFER_UNREGISTER, &input) < 0 ? errno : 0;
}

static int submit_command(int fd, uint16_t opcode, uint16_t flags,
			  uint32_t source, uint32_t destination,
			  uint32_t length, uint32_t expected_crc,
			  uint64_t cookie, uint32_t *command_id)
{
	struct vams_ioc_submit input = {
		.size = sizeof(input),
		.version = VAMS_UAPI_VERSION,
		.opcode = opcode,
		.flags = flags,
		.source_handle = source,
		.destination_handle = destination,
		.length = length,
		.timeout_ms = 1000,
		.user_cookie = cookie,
		.expected_crc = expected_crc,
	};

	if (ioctl(fd, VAMS_IOCTL_SUBMIT, &input) < 0)
		return errno;
	if (!input.command_id)
		return EPROTO;
	*command_id = input.command_id;
	return 0;
}

static int wait_command(int fd, uint32_t command_id, uint64_t cookie,
			struct vams_ioc_wait *result)
{
	struct vams_ioc_wait wait = {
		.size = sizeof(wait),
		.version = VAMS_UAPI_VERSION,
		.command_id = command_id,
		.timeout_ms = 2000,
	};

	if (ioctl(fd, VAMS_IOCTL_WAIT, &wait) < 0)
		return errno;
	if (wait.driver_status || wait.status || wait.error_code ||
	    wait.user_cookie != cookie)
		return EPROTO;
	*result = wait;
	return 0;
}

static int run_payload_api(const char *path)
{
	static const size_t page_size = 4096;
	static const uint32_t length = 256;
	struct vams_ioc_wait result;
	struct pollfd pollfd;
	uint32_t source_handle;
	uint32_t destination_handle;
	uint32_t command_id;
	uint32_t expected_crc;
	uint32_t *source_words;
	uint32_t *destination_words;
	uint8_t *source = NULL;
	uint8_t *destination = NULL;
	uint32_t nop_ids[8];
	unsigned int index;
	int other_fd;
	int error;
	int fd;

	if (posix_memalign((void **)&source, page_size, page_size) ||
	    posix_memalign((void **)&destination, page_size, page_size))
		return ENOMEM;
	for (index = 0; index < page_size; index++)
		source[index] = (uint8_t)(index * 17U + 3U);
	memset(destination, 0, page_size);

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		error = errno;
		goto out_free;
	}
	error = register_buffer(fd, source, page_size, VAMS_BUFFER_READ,
				&source_handle);
	if (error)
		goto out_close;
	error = register_buffer(fd, destination, page_size,
				VAMS_BUFFER_READ | VAMS_BUFFER_WRITE,
				&destination_handle);
	if (error)
		goto out_unregister_source;

	other_fd = open(path, O_RDWR | O_CLOEXEC);
	if (other_fd < 0) {
		error = errno;
		goto out_unregister_destination;
	}
	error = submit_command(other_fd, VAMS_OP_MEM_COPY, 0, source_handle,
			       destination_handle, length, 0, 1, &command_id);
	close(other_fd);
	if (error != ENOENT) {
		error = EACCES;
		goto out_unregister_destination;
	}

	error = submit_command(fd, VAMS_OP_MEM_COPY, 0, source_handle,
			       destination_handle, length, 0,
			       UINT64_C(0x1001), &command_id);
	if (error)
		goto out_unregister_destination;
	if (unregister_buffer(fd, source_handle) != EBUSY) {
		error = EPROTO;
		goto out_unregister_destination;
	}
	pollfd.fd = fd;
	pollfd.events = POLLIN;
	if (poll(&pollfd, 1, 2000) != 1 || !(pollfd.revents & POLLIN)) {
		error = ETIMEDOUT;
		goto out_unregister_destination;
	}
	error = wait_command(fd, command_id, UINT64_C(0x1001), &result);
	if (error || result.bytes_processed != length ||
	    memcmp(source, destination, length)) {
		error = error ? error : EIO;
		goto out_unregister_destination;
	}

	source[0] = 0xa5;
	memset(destination, 0, length);
	error = submit_command(fd, VAMS_OP_MEM_FILL, 0, source_handle,
			       destination_handle, length, 0,
			       UINT64_C(0x1002), &command_id);
	if (error)
		goto out_unregister_destination;
	error = wait_command(fd, command_id, UINT64_C(0x1002), &result);
	if (error)
		goto out_unregister_destination;
	for (index = 0; index < length; index++) {
		if (destination[index] != 0xa5) {
			error = EIO;
			goto out_unregister_destination;
		}
	}

	expected_crc = crc32_ieee(source, length);
	error = submit_command(fd, VAMS_OP_CRC32, VAMS_SUBMIT_VERIFY_CRC,
			       source_handle, 0, length, expected_crc,
			       UINT64_C(0x1003), &command_id);
	if (error)
		goto out_unregister_destination;
	error = wait_command(fd, command_id, UINT64_C(0x1003), &result);
	if (error || result.result_crc != expected_crc) {
		error = error ? error : EIO;
		goto out_unregister_destination;
	}

	source_words = (uint32_t *)source;
	destination_words = (uint32_t *)destination;
	for (index = 0; index < length / sizeof(uint32_t); index++) {
		source_words[index] = index + 1;
		destination_words[index] = index * 3;
	}
	error = submit_command(fd, VAMS_OP_VECTOR_ADD, 0, source_handle,
			       destination_handle, length, 0,
			       UINT64_C(0x1004), &command_id);
	if (error)
		goto out_unregister_destination;
	error = wait_command(fd, command_id, UINT64_C(0x1004), &result);
	if (error)
		goto out_unregister_destination;
	for (index = 0; index < length / sizeof(uint32_t); index++) {
		if (destination_words[index] != index * 4 + 1) {
			error = EIO;
			goto out_unregister_destination;
		}
	}
	for (index = 0; index < 8; index++) {
		error = submit_command(fd, VAMS_OP_NOP, 0, 0, 0, 0, 0,
				       UINT64_C(0x2000) + index, &nop_ids[index]);
		if (error)
			goto out_unregister_destination;
	}
	for (index = 0; index < 8; index++) {
		error = wait_command(fd, nop_ids[index],
				     UINT64_C(0x2000) + index, &result);
		if (error)
			goto out_unregister_destination;
	}

	error = unregister_buffer(fd, destination_handle);
	if (error)
		goto out_unregister_source;
	error = unregister_buffer(fd, source_handle);
	if (error)
		goto out_close;

	error = register_buffer(fd, source, page_size, VAMS_BUFFER_READ,
				&source_handle);
	if (error)
		goto out_close;
	error = register_buffer(fd, destination, page_size,
				VAMS_BUFFER_READ | VAMS_BUFFER_WRITE,
				&destination_handle);
	if (error)
		goto out_unregister_source;
	memset(destination, 0, length);
	error = submit_command(fd, VAMS_OP_MEM_COPY, 0, source_handle,
			       destination_handle, length, 0,
			       UINT64_C(0x3001), &command_id);
	if (error)
		goto out_unregister_destination;
	close(fd);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		error = errno;
		goto out_free;
	}
	{
		struct vams_ioc_nop nop = {
			.size = sizeof(nop),
			.version = VAMS_UAPI_VERSION,
			.user_cookie = UINT64_C(0x3002),
		};

		if (ioctl(fd, VAMS_IOCTL_NOP, &nop) < 0 ||
		    nop.status || nop.error_code ||
		    nop.user_cookie != UINT64_C(0x3002)) {
			error = errno ? errno : EPROTO;
			goto out_close;
		}
	}
	if (memcmp(source, destination, length)) {
		error = EIO;
		goto out_close;
	}
	close(fd);
	free(destination);
	free(source);
	return 0;

out_unregister_destination:
	(void)unregister_buffer(fd, destination_handle);
out_unregister_source:
	(void)unregister_buffer(fd, source_handle);
out_close:
	close(fd);
out_free:
	free(destination);
	free(source);
	return error;
}

static int run_reset_cancellation(const char *path, const char *resource_path)
{
	static const size_t page_size = 4096;
	struct vams_ioc_wait wait;
	struct vams_ioc_info info;
	uint32_t command_ids[RESET_TEST_COMMANDS];
	uint32_t source_handle = 0;
	uint32_t destination_handle = 0;
	uint8_t *source = NULL;
	uint8_t *destination = NULL;
	volatile uint32_t *bar = MAP_FAILED;
	unsigned int canceled = 0;
	unsigned int index;
	uint32_t generation;
	int resource_fd = -1;
	int error = 0;
	int fd = -1;

	if (posix_memalign((void **)&source, page_size, page_size) ||
	    posix_memalign((void **)&destination, page_size, page_size)) {
		error = ENOMEM;
		goto out;
	}
	memset(source, 0x5a, page_size);
	memset(destination, 0, page_size);
	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		error = errno;
		goto out;
	}
	error = register_buffer(fd, source, page_size, VAMS_BUFFER_READ,
				&source_handle);
	if (error)
		goto out;
	error = register_buffer(fd, destination, page_size, VAMS_BUFFER_WRITE,
				&destination_handle);
	if (error)
		goto out;

	info = (struct vams_ioc_info) {
		.size = sizeof(info),
		.version = VAMS_UAPI_VERSION,
	};
	if (ioctl(fd, VAMS_IOCTL_GET_INFO, &info) < 0) {
		error = errno;
		goto out;
	}
	generation = info.reset_generation;
	resource_fd = open(resource_path, O_RDWR | O_SYNC | O_CLOEXEC);
	if (resource_fd < 0) {
		error = errno;
		goto out;
	}
	bar = mmap(NULL, VAMS_BAR0_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
		   resource_fd, 0);
	if (bar == MAP_FAILED) {
		error = errno;
		goto out;
	}

	for (index = 0; index < RESET_TEST_COMMANDS; index++) {
		error = submit_command(fd, VAMS_OP_MEM_COPY, 0, source_handle,
				       destination_handle, page_size, 0,
				       UINT64_C(0x4000) + index,
				       &command_ids[index]);
		if (error)
			goto out;
	}
	bar[VAMS_DEVICE_CONTROL_OFFSET / sizeof(*bar)] = VAMS_DEVICE_RESET;
	__sync_synchronize();

	for (index = 0; index < RESET_TEST_COMMANDS; index++) {
		wait = (struct vams_ioc_wait) {
			.size = sizeof(wait),
			.version = VAMS_UAPI_VERSION,
			.command_id = command_ids[index],
			.timeout_ms = 2000,
		};
		if (ioctl(fd, VAMS_IOCTL_WAIT, &wait) < 0) {
			error = errno;
			goto out;
		}
		if (wait.driver_status == -ECANCELED) {
			canceled++;
		} else if (wait.driver_status || wait.status || wait.error_code ||
			   wait.user_cookie != UINT64_C(0x4000) + index) {
			error = EPROTO;
			goto out;
		}
	}
	if (!canceled) {
		error = EPROTO;
		goto out;
	}
	info = (struct vams_ioc_info) {
		.size = sizeof(info),
		.version = VAMS_UAPI_VERSION,
	};
	if (ioctl(fd, VAMS_IOCTL_GET_INFO, &info) < 0 ||
	    info.reset_generation == generation || info.queue_depth != 0) {
		error = errno ? errno : EPROTO;
		goto out;
	}
	printf("VAMS reset cancellation: submitted=%u canceled=%u generation=%u "
	       "PASS\n", RESET_TEST_COMMANDS, canceled, info.reset_generation);

out:
	if (destination_handle)
		(void)unregister_buffer(fd, destination_handle);
	if (source_handle)
		(void)unregister_buffer(fd, source_handle);
	if (bar != MAP_FAILED)
		munmap((void *)bar, VAMS_BAR0_SIZE);
	if (resource_fd >= 0)
		close(resource_fd);
	if (fd >= 0)
		close(fd);
	free(destination);
	free(source);
	return error;
}

struct worker {
	const char *path;
	unsigned int index;
	uint32_t command_ids[COMMANDS_PER_THREAD];
	int error;
};

static void *run_worker(void *opaque)
{
	struct worker *worker = opaque;
	unsigned int command;
	int fd;

	fd = open(worker->path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		worker->error = errno;
		return NULL;
	}

	for (command = 0; command < COMMANDS_PER_THREAD; command++) {
		uint64_t cookie = UINT64_C(0x56414d5300000000) |
			((uint64_t)worker->index << 8) | command;
		struct vams_ioc_nop nop = {
			.size = sizeof(nop),
			.version = VAMS_UAPI_VERSION,
			.user_cookie = cookie,
		};

		if (ioctl(fd, VAMS_IOCTL_NOP, &nop) < 0) {
			worker->error = errno;
			break;
		}
		if (!nop.command_id || nop.status || nop.error_code ||
		    nop.bytes_processed || nop.result_crc ||
		    nop.user_cookie != cookie) {
			worker->error = EPROTO;
			break;
		}
		worker->command_ids[command] = nop.command_id;
	}

	close(fd);
	return NULL;
}

static int compare_u32(const void *left, const void *right)
{
	const uint32_t a = *(const uint32_t *)left;
	const uint32_t b = *(const uint32_t *)right;

	return (a > b) - (a < b);
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/dev/vams0";
	const char *resource_path = argc > 2 ? argv[2] :
		"/sys/bus/pci/devices/0000:00:02.0/resource0";
	struct worker workers[THREAD_COUNT] = { 0 };
	pthread_t threads[THREAD_COUNT];
	uint32_t command_ids[COMMAND_COUNT];
	struct vams_ioc_info info = {
		.size = sizeof(info),
		.version = VAMS_UAPI_VERSION,
	};
	unsigned int index;
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open VAMS device");
		return 1;
	}
	if (ioctl(fd, VAMS_IOCTL_GET_INFO, &info) < 0) {
		perror("VAMS_IOCTL_GET_INFO");
		close(fd);
		return 1;
	}
	if (info.version != VAMS_UAPI_VERSION ||
	    (info.hw_if_version >> 16) != 1U ||
	    (info.capabilities & 0x23U) != 0x23U || info.queue_depth != 16U) {
		fprintf(stderr, "invalid VAMS device info\n");
		close(fd);
		return 1;
	}

	info.version++;
	errno = 0;
	if (ioctl(fd, VAMS_IOCTL_GET_INFO, &info) != -1 || errno != EINVAL) {
		fprintf(stderr, "invalid UAPI version was not rejected\n");
		close(fd);
		return 1;
	}
	close(fd);
	{
		int error = run_payload_api(path);

		if (error) {
			fprintf(stderr, "payload/asynchronous API failed: %s\n",
				strerror(error));
			return 1;
		}
	}

	for (index = 0; index < THREAD_COUNT; index++) {
		workers[index].path = path;
		workers[index].index = index;
		if (pthread_create(&threads[index], NULL, run_worker,
				   &workers[index]) != 0) {
			fprintf(stderr, "pthread_create failed\n");
			return 1;
		}
	}
	for (index = 0; index < THREAD_COUNT; index++) {
		unsigned int command;

		pthread_join(threads[index], NULL);
		if (workers[index].error) {
			fprintf(stderr, "worker %u failed: %s\n", index,
				strerror(workers[index].error));
			return 1;
		}
		for (command = 0; command < COMMANDS_PER_THREAD; command++)
			command_ids[index * COMMANDS_PER_THREAD + command] =
				workers[index].command_ids[command];
	}

	qsort(command_ids, COMMAND_COUNT, sizeof(command_ids[0]), compare_u32);
	for (index = 1; index < COMMAND_COUNT; index++) {
		if (command_ids[index] == command_ids[index - 1]) {
			fprintf(stderr, "duplicate command ID %u\n", command_ids[index]);
			return 1;
		}
	}
	{
		int error = run_reset_cancellation(path, resource_path);

		if (error) {
			fprintf(stderr, "reset cancellation failed: %s\n",
				strerror(error));
			return 1;
		}
	}

	printf("VAMS UAPI round trip: sync_commands=%u payload_ops=4 async_nops=8 "
	       "reset_requests=%u depth=%u generation=%u PASS\n",
	       COMMAND_COUNT, RESET_TEST_COMMANDS, info.queue_depth,
	       info.reset_generation);
	return 0;
}

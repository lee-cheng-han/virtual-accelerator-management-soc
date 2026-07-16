/* SPDX-License-Identifier: MIT */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/vams.h>

#define THREAD_COUNT 4U
#define COMMANDS_PER_THREAD 8U
#define COMMAND_COUNT (THREAD_COUNT * COMMANDS_PER_THREAD)

_Static_assert(sizeof(struct vams_ioc_info) == 32,
	       "vams_ioc_info ABI size changed");
_Static_assert(sizeof(struct vams_ioc_nop) == 56,
	       "vams_ioc_nop ABI size changed");
_Static_assert(offsetof(struct vams_ioc_nop, user_cookie) == 16,
	       "vams_ioc_nop user_cookie offset changed");
_Static_assert(offsetof(struct vams_ioc_nop, device_timestamp) == 40,
	       "vams_ioc_nop device_timestamp offset changed");

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

	printf("VAMS UAPI round trip: commands=%u depth=%u generation=%u PASS\n",
	       COMMAND_COUNT, info.queue_depth, info.reset_generation);
	return 0;
}

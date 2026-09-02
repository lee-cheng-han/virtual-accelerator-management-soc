/* SPDX-License-Identifier: MIT */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void finish(int code)
{
	printf("VAMS Linux PCI payload, asynchronous, polling, and cleanup "
	       "smoke test: %s\n", code ? "FAIL" : "PASS");
	fflush(stdout);
	if (ioperm(0xf4, sizeof(uint32_t), 1) == 0)
		outl((uint32_t)code, 0xf4);
	sync();
	reboot(RB_POWER_OFF);
	_exit(code);
}

static void fail(const char *message)
{
	fprintf(stderr, "FAIL: %s: %s\n", message, strerror(errno));
	finish(1);
}

static int load_module(const char *parameters)
{
	int fd = open("/vams_pci.ko", O_RDONLY | O_CLOEXEC);
	int ret;

	if (fd < 0)
		return -1;
	ret = syscall(SYS_finit_module, fd, parameters, 0);
	close(fd);
	return ret;
}

static int unload_module(void)
{
	return syscall(SYS_delete_module, "vams_pci", O_NONBLOCK);
}

static int run_uapi_test(void)
{
	pid_t child = fork();
	int status;

	if (child < 0)
		return -1;
	if (child == 0) {
		execl("/vams-uapi-test", "vams-uapi-test", "/dev/vams0", NULL);
		_exit(127);
	}
	if (waitpid(child, &status, 0) != child)
		return -1;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

int main(void)
{
	struct stat status;
	unsigned int step;

	(void)mkdir("/proc", 0755);
	(void)mkdir("/sys", 0755);
	(void)mkdir("/dev", 0755);
	if (mount("proc", "/proc", "proc", 0, NULL) && errno != EBUSY)
		fail("mount proc");
	if (mount("sysfs", "/sys", "sysfs", 0, NULL) && errno != EBUSY)
		fail("mount sysfs");
	if (mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) && errno != EBUSY)
		fail("mount devtmpfs");

	for (step = 1; step <= 9; step++) {
		char parameters[64];

		snprintf(parameters, sizeof(parameters), "probe_fail_step=%u", step);
		if (load_module(parameters))
			fail("load probe-failure module");
		if (stat("/dev/vams0", &status) == 0) {
			errno = EBUSY;
			fail("probe-failure device remained registered");
		}
		if (unload_module())
			fail("unload probe-failure module");
	}

	if (load_module("probe_irq_selftest=1 probe_nop_selftest=1 "
			"probe_poll_selftest=1"))
		fail("load tested module");
	if (stat("/dev/vams0", &status) || !S_ISCHR(status.st_mode))
		fail("character device missing");
	if (run_uapi_test()) {
		errno = EPROTO;
		fail("UAPI integration");
	}
	if (unload_module())
		fail("unload tested module");

	if (load_module(""))
		fail("load module after cleanup");
	if (stat("/dev/vams0", &status) || !S_ISCHR(status.st_mode))
		fail("re-probed character device missing");
	if (unload_module())
		fail("final module unload");
	finish(0);
	return 0;
}

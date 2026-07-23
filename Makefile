SHELL := /bin/sh

.DEFAULT_GOAL := help

SPEC_DOCS := README.md \
	docs/requirements.md docs/architecture.md docs/roadmap.md \
	docs/register-map.md docs/host-firmware-abi.md \
	docs/descriptor-format.md docs/firmware-architecture.md \
	docs/command-lifecycle.md docs/fault-recovery.md \
	docs/verification-plan.md docs/performance-plan.md docs/demo.md \
	docs/minimal-riscv-subsystem.md docs/zephyr-board-port.md \
	docs/management-peripherals.md docs/pcie-endpoint.md \
	docs/linux-pci-driver.md docs/nop-command-path.md \
	docs/mem-copy-command-path.md docs/mem-fill-command-path.md \
	docs/crc32-command-path.md

SPEC_DOCS += docs/linux-uapi.md

CROSS_COMPILE ?= riscv64-unknown-elf-
QEMU_SYSTEM_RISCV32 ?= qemu-system-riscv32
QEMU_SYSTEM_X86_64 ?= qemu-system-x86_64
KERNEL_BUILD ?= /lib/modules/$(shell uname -r)/build
VAMS_LINUX_IMAGE ?=
VAMS_PCI_MODULE ?= $(CURDIR)/kernel/vams_pci.ko
VAMS_UAPI_TEST ?= $(CURDIR)/build/kernel/vams-uapi-test
BUSYBOX ?= busybox
HOST_CC ?= gcc
HOST_CLANG ?= clang
VAMS_FIRMWARE ?= $(CURDIR)/build/firmware/baremetal/vams-riscv-fw.elf
ZEPHYR_BASE ?= $(CURDIR)/build/zephyrproject/zephyr
ZEPHYR_VENV ?= $(CURDIR)/build/zephyr-venv
ZEPHYR_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr
ZEPHYR_WATCHDOG_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr-watchdog
VAMS_ZEPHYR_FIRMWARE ?= $(ZEPHYR_BUILD_DIR)/zephyr/zephyr.elf
VAMS_WATCHDOG_FIRMWARE ?= $(ZEPHYR_WATCHDOG_BUILD_DIR)/zephyr/zephyr.elf

.PHONY: help check check-docs abi-check firmware smoke zephyr-prepare zephyr \
	zephyr-smoke zephyr-watchdog management-smoke management-mmio-smoke \
	watchdog-smoke command-portal-smoke firmware-command-smoke \
	firmware-pcie-smoke mem-copy-smoke mem-fill-smoke crc32-smoke \
	pcie-smoke nop-smoke queue-model-smoke kernel kernel-test-build \
	kernel-uapi-test kernel-smoke \
	qemu-patch-check tree clean demo

help:
	@printf '%s\n' \
	  'Virtual Accelerator Management SoC' \
	  '' \
	  '  make check       Validate specifications and source hygiene' \
	  '  make firmware    Build the RV32 bare-metal firmware' \
	  '  make smoke       Boot the firmware and verify its UART transcript' \
	  '  make zephyr-prepare' \
	  '                   Fetch pinned Zephyr build dependencies' \
	  '  make zephyr      Build the vams_riscv Zephyr application' \
	  '  make zephyr-smoke' \
	  '                   Verify Zephyr timer scheduling and task IPC' \
	  '  make management-smoke' \
	  '                   Verify mailbox handling and telemetry updates' \
	  '  make management-mmio-smoke' \
	  '                   Verify the management register contract' \
	  '  make watchdog-smoke' \
	  '                   Verify watchdog reset and firmware recovery' \
	  '  make command-portal-smoke' \
	  '                   Verify the private firmware portal state machine' \
	  '  make firmware-command-smoke' \
	  '                   Verify firmware-owned NOP validation and completion' \
	  '  make firmware-pcie-smoke' \
	  '                   Verify PCI DMA through real Zephyr command handling' \
	  '  make mem-copy-smoke' \
	  '                   Verify firmware-owned payload copy and validation' \
	  '  make mem-fill-smoke' \
	  '                   Verify firmware-owned payload fill and validation' \
	  '  make crc32-smoke  Verify firmware-owned CRC32 and result checking' \
	  '  make pcie-smoke   Verify PCIe identity, BAR0, MSI-X, and reset' \
	  '  make nop-smoke    Verify SQ/CQ DMA and NOP completion behavior' \
	  '  make queue-model-smoke' \
	  '                   Compare randomized SQ/CQ sequences with the model' \
	  '  make abi-check    Regenerate-check and compile-test the v1 ABI' \
	  '  make kernel       Build the production vams_pci kernel module' \
	  '  make kernel-uapi-test' \
	  '                   Build the static VAMS host-API integration client' \
	  '  make kernel-smoke Build and test probe, MSI-X, and cleanup in a guest' \
	  '  make qemu-patch-check QEMU_SRC=/path/to/qemu' \
	  '                   Check that the QEMU patch series applies cleanly' \
	  '  make tree        Print the repository tree' \
	  '  make demo        Explain full-demo availability' \
	  '  make clean       Remove generated output'

check: check-docs abi-check

check-docs:
	@set -eu; \
	for file in $(SPEC_DOCS); do \
		test -s "$$file" || { echo "missing or empty: $$file" >&2; exit 1; }; \
	done; \
	if LC_ALL=C grep -RIn '[[:blank:]]$$' README.md docs; then \
		echo 'trailing whitespace found' >&2; exit 1; \
	fi; \
	grep -q 'Firmware-owned MEM_COPY, MEM_FILL, and CRC32 implemented' README.md; \
	grep -q 'sizeof(struct vams_submission) == 64' docs/descriptor-format.md; \
	grep -q 'sizeof(struct vams_completion) == 32' docs/descriptor-format.md; \
	echo 'Documentation checks: PASS'

abi-check:
	./scripts/gen-vams-abi.py --check
	./tests/abi/test-vams-abi.py
	@mkdir -p build/tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude \
		tests/abi/test-vams-abi.c -o build/tests/test-vams-abi-gcc
	./build/tests/test-vams-abi-gcc
	$(HOST_CLANG) -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude \
		tests/abi/test-vams-abi.c -o build/tests/test-vams-abi-clang
	./build/tests/test-vams-abi-clang

firmware:
	$(MAKE) -C firmware/baremetal CROSS_COMPILE="$(CROSS_COMPILE)"

smoke: firmware
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_FIRMWARE="$(VAMS_FIRMWARE)" \
	./qemu/tests/smoke-vams-riscv.sh

zephyr-prepare:
	ZEPHYR_VENV="$(ZEPHYR_VENV)" ./scripts/prepare-zephyr.sh

zephyr:
	@test -x "$(ZEPHYR_VENV)/bin/python" || { \
		echo 'Zephyr environment missing; run make zephyr-prepare' >&2; \
		exit 2; \
	}
	@test -f "$(ZEPHYR_BASE)/VERSION" || { \
		echo 'Zephyr source missing; run make zephyr-prepare' >&2; \
		exit 2; \
	}
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	ZEPHYR_BASE="$(ZEPHYR_BASE)" \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE="$(CROSS_COMPILE)" \
	cmake -S firmware -B "$(ZEPHYR_BUILD_DIR)" -G Ninja \
		-DUSE_CCACHE=0 \
		-DBOARD=vams_riscv \
		-DBOARD_ROOT="$(CURDIR)/firmware" \
		-DSOC_ROOT="$(CURDIR)/firmware"
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	cmake --build "$(ZEPHYR_BUILD_DIR)"

zephyr-smoke: zephyr
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/smoke-vams-zephyr.sh

zephyr-watchdog:
	@test -x "$(ZEPHYR_VENV)/bin/python" || { \
		echo 'Zephyr environment missing; run make zephyr-prepare' >&2; \
		exit 2; \
	}
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	ZEPHYR_BASE="$(ZEPHYR_BASE)" \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE="$(CROSS_COMPILE)" \
	cmake -S firmware -B "$(ZEPHYR_WATCHDOG_BUILD_DIR)" -G Ninja \
		-DUSE_CCACHE=0 \
		-DBOARD=vams_riscv \
		-DBOARD_ROOT="$(CURDIR)/firmware" \
		-DSOC_ROOT="$(CURDIR)/firmware" \
		-DEXTRA_CONF_FILE=tests/watchdog-expiry.conf
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	cmake --build "$(ZEPHYR_WATCHDOG_BUILD_DIR)"

management-smoke: zephyr
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/smoke-vams-management.sh

management-mmio-smoke: zephyr
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/smoke-vams-management-mmio.sh

watchdog-smoke: zephyr-watchdog
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_WATCHDOG_FIRMWARE="$(VAMS_WATCHDOG_FIRMWARE)" \
	./qemu/tests/smoke-vams-watchdog.sh

command-portal-smoke: firmware
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_FIRMWARE="$(VAMS_FIRMWARE)" \
	./qemu/tests/smoke-vams-command-portal.sh

firmware-command-smoke: zephyr
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/smoke-vams-firmware-command.sh

firmware-pcie-smoke: zephyr
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/smoke-vams-firmware-pcie.py

mem-copy-smoke: firmware-pcie-smoke

mem-fill-smoke: firmware-pcie-smoke

crc32-smoke: firmware-pcie-smoke

pcie-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/smoke-vams-pcie.sh

nop-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/smoke-vams-nop.sh

queue-model-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/qtest/vams-queue-model.py

kernel:
	$(MAKE) -C kernel KERNEL_BUILD="$(KERNEL_BUILD)"

kernel-test-build:
	$(MAKE) -C kernel KERNEL_BUILD="$(KERNEL_BUILD)" VAMS_TESTING=1

kernel-uapi-test:
	@mkdir -p "$(dir $(VAMS_UAPI_TEST))"
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -static -pthread \
		-Ikernel/include/uapi kernel/tests/vams-uapi-test.c \
		-o "$(VAMS_UAPI_TEST)"

kernel-smoke: kernel-test-build kernel-uapi-test
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	VAMS_LINUX_IMAGE="$(VAMS_LINUX_IMAGE)" \
	VAMS_PCI_MODULE="$(VAMS_PCI_MODULE)" \
	VAMS_UAPI_TEST="$(VAMS_UAPI_TEST)" \
	BUSYBOX="$(BUSYBOX)" \
	./kernel/tests/smoke-vams-pci.sh

qemu-patch-check:
	@test -n "$(QEMU_SRC)" || { \
		echo 'usage: make qemu-patch-check QEMU_SRC=/path/to/qemu' >&2; \
		exit 2; \
	}
	@set -eu; \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	git clone --quiet --shared "$(QEMU_SRC)" "$$tmp/qemu"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0001-hw-riscv-add-vams-riscv-machine.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0001-hw-riscv-add-vams-riscv-machine.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0002-hw-add-vams-management-peripherals.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0002-hw-add-vams-management-peripherals.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0003-hw-misc-add-vams-pcie-endpoint.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0003-hw-misc-add-vams-pcie-endpoint.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0004-hw-misc-add-vams-nop-queue-transport.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0004-hw-misc-add-vams-nop-queue-transport.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0005-hw-misc-add-vams-firmware-command-portal.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0005-hw-misc-add-vams-firmware-command-portal.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0006-hw-misc-bridge-vams-pci-queues-to-firmware.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0006-hw-misc-bridge-vams-pci-queues-to-firmware.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0007-hw-misc-add-vams-memory-copy-engine.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0007-hw-misc-add-vams-memory-copy-engine.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0008-hw-misc-add-vams-memory-fill-engine.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0008-hw-misc-add-vams-memory-fill-engine.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0009-hw-misc-add-vams-crc32-engine.patch"; \
	echo 'QEMU patch series check: PASS'

tree:
	@find . -path './.git' -prune -o -path './build' -prune -o -print | sort

demo:
	@echo 'The full PCIe accelerator demo is not implemented; see docs/demo.md'

clean:
	$(MAKE) -C kernel KERNEL_BUILD="$(KERNEL_BUILD)" clean
	rm -rf build out test-results coverage

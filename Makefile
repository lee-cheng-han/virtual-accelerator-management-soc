SHELL := /bin/sh

.DEFAULT_GOAL := help

SPEC_DOCS := README.md \
	docs/requirements.md docs/architecture.md docs/roadmap.md \
	docs/register-map.md docs/host-firmware-abi.md \
	docs/descriptor-format.md docs/firmware-architecture.md \
	docs/command-lifecycle.md docs/fault-recovery.md \
	docs/verification-plan.md docs/performance-plan.md docs/demo.md \
	docs/minimal-riscv-subsystem.md docs/zephyr-board-port.md \
	docs/management-peripherals.md

CROSS_COMPILE ?= riscv64-unknown-elf-
QEMU_SYSTEM_RISCV32 ?= qemu-system-riscv32
VAMS_FIRMWARE ?= $(CURDIR)/build/firmware/baremetal/vams-riscv-fw.elf
ZEPHYR_BASE ?= $(CURDIR)/build/zephyrproject/zephyr
ZEPHYR_VENV ?= $(CURDIR)/build/zephyr-venv
ZEPHYR_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr
ZEPHYR_WATCHDOG_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr-watchdog
VAMS_ZEPHYR_FIRMWARE ?= $(ZEPHYR_BUILD_DIR)/zephyr/zephyr.elf
VAMS_WATCHDOG_FIRMWARE ?= $(ZEPHYR_WATCHDOG_BUILD_DIR)/zephyr/zephyr.elf

.PHONY: help check check-docs firmware smoke zephyr-prepare zephyr \
	zephyr-smoke zephyr-watchdog management-smoke management-mmio-smoke \
	watchdog-smoke qemu-patch-check tree clean demo

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
	  '  make qemu-patch-check QEMU_SRC=/path/to/qemu' \
	  '                   Check that the machine patch applies cleanly' \
	  '  make tree        Print the repository tree' \
	  '  make demo        Explain full-demo availability' \
	  '  make clean       Remove generated output'

check: check-docs

check-docs:
	@set -eu; \
	for file in $(SPEC_DOCS); do \
		test -s "$$file" || { echo "missing or empty: $$file" >&2; exit 1; }; \
	done; \
	if LC_ALL=C grep -RIn '[[:blank:]]$$' README.md docs; then \
		echo 'trailing whitespace found' >&2; exit 1; \
	fi; \
	grep -q 'RISC-V management control plane implemented' README.md; \
	grep -q 'sizeof(struct vams_submission) == 64' docs/descriptor-format.md; \
	grep -q 'sizeof(struct vams_completion) == 32' docs/descriptor-format.md; \
	echo 'Documentation checks: PASS'

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
	echo 'QEMU patch series check: PASS'

tree:
	@find . -path './.git' -prune -o -path './build' -prune -o -print | sort

demo:
	@echo 'The full PCIe accelerator demo is not implemented; see docs/demo.md'

clean:
	rm -rf build out test-results coverage

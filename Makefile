SHELL := /bin/sh

.DEFAULT_GOAL := help

SPEC_DOCS := README.md \
	docs/requirements.md docs/architecture.md docs/roadmap.md \
	docs/register-map.md docs/host-firmware-abi.md \
	docs/descriptor-format.md docs/firmware-architecture.md \
	docs/command-lifecycle.md docs/fault-recovery.md \
	docs/verification-plan.md docs/performance-plan.md docs/demo.md \
	docs/minimal-riscv-subsystem.md

CROSS_COMPILE ?= riscv64-unknown-elf-
QEMU_SYSTEM_RISCV32 ?= qemu-system-riscv32
VAMS_FIRMWARE ?= $(CURDIR)/build/firmware/baremetal/vams-riscv-fw.elf

.PHONY: help check check-docs firmware smoke qemu-patch-check tree clean demo

help:
	@printf '%s\n' \
	  'Virtual Accelerator Management SoC' \
	  '' \
	  '  make check       Validate specifications and source hygiene' \
	  '  make firmware    Build the RV32 bare-metal firmware' \
	  '  make smoke       Boot the firmware and verify its UART transcript' \
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
	grep -q 'minimal RV32 subsystem implemented' README.md; \
	grep -q 'sizeof(struct vams_submission) == 64' docs/descriptor-format.md; \
	grep -q 'sizeof(struct vams_completion) == 32' docs/descriptor-format.md; \
	echo 'Documentation checks: PASS'

firmware:
	$(MAKE) -C firmware/baremetal CROSS_COMPILE="$(CROSS_COMPILE)"

smoke: firmware
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_FIRMWARE="$(VAMS_FIRMWARE)" \
	./qemu/tests/smoke-vams-riscv.sh

qemu-patch-check:
	@test -n "$(QEMU_SRC)" || { \
		echo 'usage: make qemu-patch-check QEMU_SRC=/path/to/qemu' >&2; \
		exit 2; \
	}
	git -C "$(QEMU_SRC)" apply --check \
		"$(CURDIR)/qemu/patches/0001-hw-riscv-add-vams-riscv-machine.patch"

tree:
	@find . -path './.git' -prune -o -path './build' -prune -o -print | sort

demo:
	@echo 'The full PCIe accelerator demo is not implemented; see docs/demo.md'

clean:
	rm -rf build out test-results coverage

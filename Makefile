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
	docs/compatibility.md \
	docs/reproducible-builds.md \
	docs/mem-copy-command-path.md docs/mem-fill-command-path.md \
	docs/crc32-command-path.md docs/vector-add-command-path.md
SPEC_DOCS += docs/asynchronous-engine.md
SPEC_DOCS += docs/assurance.md
SPEC_DOCS += docs/chunked-dma.md
SPEC_DOCS += docs/firmware-scheduler.md
SPEC_DOCS += docs/evidence/stress-qualification.json

SPEC_DOCS += docs/linux-uapi.md

RISCV_GCC := $(shell command -v riscv64-unknown-elf-gcc 2>/dev/null)
CROSS_COMPILE ?= $(if $(RISCV_GCC),$(patsubst %gcc,%,$(RISCV_GCC)),riscv64-unknown-elf-)
QEMU_SYSTEM_RISCV32 ?= qemu-system-riscv32
QEMU_SYSTEM_X86_64 ?= qemu-system-x86_64
KERNEL_BUILD ?= /lib/modules/$(shell uname -r)/build
VAMS_LINUX_IMAGE ?=
VAMS_PCI_MODULE ?= $(CURDIR)/kernel/vams_pci.ko
VAMS_UAPI_TEST ?= $(CURDIR)/build/kernel/vams-uapi-test
VAMS_GUEST_INIT ?= $(CURDIR)/build/kernel/vams-guest-init
GEN_INIT_CPIO ?= gen_init_cpio
HOST_CC ?= gcc
HOST_CLANG ?= clang
VAMS_FIRMWARE ?= $(CURDIR)/build/firmware/baremetal/vams-riscv-fw.elf
ZEPHYR_BASE ?= $(CURDIR)/build/zephyrproject/zephyr
ZEPHYR_VENV ?= $(CURDIR)/build/zephyr-venv
ZEPHYR_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr
ZEPHYR_WATCHDOG_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr-watchdog
ZEPHYR_SCHEDULER_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr-scheduler
ZEPHYR_OVERLOAD_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr-overload
ZEPHYR_HEALTH_BUILD_DIR ?= $(CURDIR)/build/firmware/zephyr-health
VAMS_ZEPHYR_FIRMWARE ?= $(ZEPHYR_BUILD_DIR)/zephyr/zephyr.elf
VAMS_WATCHDOG_FIRMWARE ?= $(ZEPHYR_WATCHDOG_BUILD_DIR)/zephyr/zephyr.elf
VAMS_SCHEDULER_FIRMWARE ?= $(ZEPHYR_SCHEDULER_BUILD_DIR)/zephyr/zephyr.elf
VAMS_OVERLOAD_FIRMWARE ?= $(ZEPHYR_OVERLOAD_BUILD_DIR)/zephyr/zephyr.elf
VAMS_HEALTH_FIRMWARE ?= $(ZEPHYR_HEALTH_BUILD_DIR)/zephyr/zephyr.elf
VAMS_DESCRIPTOR_FUZZ_SEED ?= 0xd35c0123
VAMS_BAR_FUZZ_SEED ?= 0xba4f0223
VAMS_FUZZ_ITERATIONS ?= 4096
VAMS_STRESS_COMMANDS ?= 1000000
VAMS_STRESS_RESETS ?= 1000
VAMS_STRESS_PAYLOAD_SAMPLES ?= 256
VAMS_STRESS_VIRTUAL_HOURS ?= 24
QEMU_SRC ?= $(CURDIR)/build/qemu-src
QEMU_BUILD_DIR ?= $(QEMU_SRC)/build-vams
VAMS_DEMO_QEMU_RISCV32 ?= $(QEMU_BUILD_DIR)/qemu-system-riscv32
VAMS_DEMO_QEMU_X86_64 ?= $(QEMU_BUILD_DIR)/qemu-system-x86_64
VAMS_DEMO_OUTPUT ?=

.PHONY: help check check-docs release-input-check abi-check firmware-scheduler-unit firmware-overload-unit firmware-health-unit firmware-retained-unit firmware smoke qemu-prepare zephyr-prepare zephyr \
	zephyr-smoke zephyr-watchdog zephyr-scheduler-timeout zephyr-overload zephyr-health \
	management-smoke management-mmio-smoke \
	watchdog-smoke command-portal-smoke firmware-command-smoke firmware-overload-smoke firmware-health-smoke \
	firmware-pcie-smoke mem-copy-smoke mem-fill-smoke crc32-smoke \
	vector-add-smoke async-engine-smoke scheduler-recovery-smoke \
	fault-injection-smoke \
	firmware-ownership-smoke firmware-ownership-model-smoke \
	payload-throughput-smoke \
	stress-smoke firmware-resource-report stress-qualification \
	dma-engine-smoke \
	pcie-smoke nop-smoke queue-model-smoke cq-backpressure-smoke \
	kernel kernel-test-build \
	kernel-uapi-test kernel-guest-init kernel-smoke \
	descriptor-fuzz bar-fuzz fuzz-smoke source-check assurance-smoke \
	qemu-patch-check tree clean demo

help:
	@printf '%s\n' \
	  'Virtual Accelerator Management SoC' \
	  '' \
	  '  make check       Validate specifications and source hygiene' \
	  '  make firmware    Build the RV32 bare-metal firmware' \
	  '  make firmware-scheduler-unit' \
	  '                   Verify firmware EDF ordering on the host' \
	  '  make firmware-overload-unit' \
	  '                   Verify saturating counters and admission policy' \
	  '  make firmware-health-unit' \
	  '                   Verify per-task deadline and stuck-task policy' \
	  '  make firmware-retained-unit' \
	  '                   Verify retained-record version and CRC handling' \
	  '  make smoke       Boot the firmware and verify its UART transcript' \
	  '  make qemu-prepare Build the exact pinned VAMS QEMU targets' \
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
	  '  make firmware-overload-smoke' \
	  '                   Verify bounded diagnostic overload accounting' \
	  '  make firmware-health-smoke' \
	  '                   Freeze each essential task and verify recovery' \
	  '  make firmware-pcie-smoke' \
	  '                   Verify PCI DMA through real Zephyr command handling' \
	  '  make mem-copy-smoke' \
	  '                   Verify firmware-owned payload copy and validation' \
	  '  make mem-fill-smoke' \
	  '                   Verify firmware-owned payload fill and validation' \
	  '  make crc32-smoke  Verify firmware-owned CRC32 and result checking' \
	  '  make vector-add-smoke' \
	  '                   Verify firmware-owned vector arithmetic and DMA' \
	  '  make async-engine-smoke' \
	  '                   Verify deadlines and reset-safe engine cancellation' \
	  '  make scheduler-recovery-smoke' \
	  '                   Verify firmware queued timeout and clean recovery' \
	  '  make fault-injection-smoke' \
	  '                   Verify debug faults, checkpoints, and recovery' \
	  '  make firmware-ownership-smoke' \
	  '                   Verify result ownership, reset, and disconnect' \
	  '  make firmware-ownership-model-smoke' \
	  '                   Run ownership recovery without firmware build inputs' \
	  '  make payload-throughput-smoke' \
	  '                   Verify bounded DMA and report virtual throughput' \
	  '  make stress-smoke Run a short queue/reset/endurance qualification' \
	  '  make firmware-resource-report' \
	  '                   Validate firmware stack/SRAM/queue/watchdog margins' \
	  '  make stress-qualification' \
	  '                   Run the million-command hardware-free qualification' \
	  '  make dma-engine-smoke' \
	  '                   Run firmware payload and bounded-engine validation' \
	  '  make pcie-smoke   Verify PCIe identity, BAR0, MSI-X, and reset' \
	  '  make nop-smoke    Verify SQ/CQ DMA and NOP completion behavior' \
	  '  make queue-model-smoke' \
	  '                   Compare randomized SQ/CQ sequences with the model' \
	  '  make cq-backpressure-smoke' \
	  '                   Verify CQ watermark hysteresis and overload throttling' \
	  '  make descriptor-fuzz' \
	  '                   Mutate raw descriptors with a replayable seed' \
	  '  make bar-fuzz     Drive malformed BAR sequences with a replayable seed' \
	  '  make fuzz-smoke   Run descriptor and BAR fuzz regressions' \
	  '  make assurance-smoke' \
	  '                   Run ABI, source, model, and fuzz assurance checks' \
	  '  make abi-check    Regenerate-check and compile-test the v1 ABI' \
	  '  make kernel       Build the production vams_pci kernel module' \
	  '  make kernel-uapi-test' \
	  '                   Build the static VAMS host-API integration client' \
	  '  make kernel-smoke Test payload DMA, async UAPI, IRQs, and cleanup in a guest' \
	  '  make qemu-patch-check QEMU_SRC=/path/to/qemu' \
	  '                   Check that the QEMU patch series applies cleanly' \
	  '  make tree        Print the repository tree' \
	  '  make demo        Run the offline hardware-free system demonstration' \
	  '  make clean       Remove generated output'

check: check-docs release-input-check abi-check firmware-scheduler-unit \
	firmware-overload-unit firmware-health-unit firmware-retained-unit

check-docs:
	@set -eu; \
	for file in $(SPEC_DOCS); do \
		test -s "$$file" || { echo "missing or empty: $$file" >&2; exit 1; }; \
	done; \
	if LC_ALL=C grep -RIn '[[:blank:]]$$' README.md docs; then \
		echo 'trailing whitespace found' >&2; exit 1; \
	fi; \
	grep -q 'Offline hardware-free system demo implemented' README.md; \
	grep -q 'bounded 64 KiB DMA working chunk' README.md; \
	grep -q 'sizeof(struct vams_submission) == 64' docs/descriptor-format.md; \
	grep -q 'sizeof(struct vams_completion) == 32' docs/descriptor-format.md; \
	echo 'Documentation checks: PASS'

release-input-check:
	./scripts/check-release-inputs.py

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

firmware-scheduler-unit:
	@mkdir -p build/tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror \
		-Iinclude -Ifirmware/include \
		tests/firmware/test-vams-scheduler.c \
		-o build/tests/test-vams-scheduler
	./build/tests/test-vams-scheduler

firmware-overload-unit:
	@mkdir -p build/tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror \
		-Ifirmware/include tests/firmware/test-vams-overload.c \
		-o build/tests/test-vams-overload
	./build/tests/test-vams-overload

firmware-health-unit:
	@mkdir -p build/tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror \
		-Ifirmware/include tests/firmware/test-vams-health.c \
		firmware/src/health/vams_health.c \
		-o build/tests/test-vams-health
	./build/tests/test-vams-health

firmware-retained-unit:
	@mkdir -p build/tests
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror \
		-Ifirmware/include tests/firmware/test-vams-retained.c \
		firmware/src/recovery/vams_retained.c \
		-o build/tests/test-vams-retained
	./build/tests/test-vams-retained

firmware:
	$(MAKE) -C firmware/baremetal CROSS_COMPILE="$(CROSS_COMPILE)"

smoke: firmware
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_FIRMWARE="$(VAMS_FIRMWARE)" \
	./qemu/tests/smoke-vams-riscv.sh

qemu-prepare:
	QEMU_SRC="$(QEMU_SRC)" QEMU_BUILD_DIR="$(QEMU_BUILD_DIR)" \
		./scripts/prepare-qemu.sh

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

zephyr-scheduler-timeout:
	@test -x "$(ZEPHYR_VENV)/bin/python" || { \
		echo 'Zephyr environment missing; run make zephyr-prepare' >&2; \
		exit 2; \
	}
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	ZEPHYR_BASE="$(ZEPHYR_BASE)" \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE="$(CROSS_COMPILE)" \
	cmake --fresh -S firmware -B "$(ZEPHYR_SCHEDULER_BUILD_DIR)" -G Ninja \
		-DUSE_CCACHE=0 \
		-DBOARD=vams_riscv \
		-DBOARD_ROOT="$(CURDIR)/firmware" \
		-DSOC_ROOT="$(CURDIR)/firmware" \
		-DEXTRA_CONF_FILE=tests/scheduler-timeout.conf
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	cmake --build "$(ZEPHYR_SCHEDULER_BUILD_DIR)"

zephyr-overload:
	@test -x "$(ZEPHYR_VENV)/bin/python" || { \
		echo 'Zephyr environment missing; run make zephyr-prepare' >&2; \
		exit 2; \
	}
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	ZEPHYR_BASE="$(ZEPHYR_BASE)" \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE="$(CROSS_COMPILE)" \
	cmake --fresh -S firmware -B "$(ZEPHYR_OVERLOAD_BUILD_DIR)" -G Ninja \
		-DUSE_CCACHE=0 \
		-DBOARD=vams_riscv \
		-DBOARD_ROOT="$(CURDIR)/firmware" \
		-DSOC_ROOT="$(CURDIR)/firmware" \
		-DEXTRA_CONF_FILE=tests/overload.conf
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	cmake --build "$(ZEPHYR_OVERLOAD_BUILD_DIR)"

zephyr-health:
	@test -x "$(ZEPHYR_VENV)/bin/python" || { \
		echo 'Zephyr environment missing; run make zephyr-prepare' >&2; \
		exit 2; \
	}
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	ZEPHYR_BASE="$(ZEPHYR_BASE)" \
	ZEPHYR_TOOLCHAIN_VARIANT=cross-compile \
	CROSS_COMPILE="$(CROSS_COMPILE)" \
	cmake --fresh -S firmware -B "$(ZEPHYR_HEALTH_BUILD_DIR)" -G Ninja \
		-DUSE_CCACHE=0 \
		-DBOARD=vams_riscv \
		-DBOARD_ROOT="$(CURDIR)/firmware" \
		-DSOC_ROOT="$(CURDIR)/firmware" \
		-DEXTRA_CONF_FILE=tests/health.conf
	PATH="$(ZEPHYR_VENV)/bin:$$PATH" \
	cmake --build "$(ZEPHYR_HEALTH_BUILD_DIR)"

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

firmware-overload-smoke: zephyr-overload
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_OVERLOAD_FIRMWARE="$(VAMS_OVERLOAD_FIRMWARE)" \
	./qemu/tests/smoke-vams-firmware-overload.sh
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/smoke-vams-firmware-pcie.py \
		--firmware "$(VAMS_OVERLOAD_FIRMWARE)" \
		--require-admission-deferral

firmware-health-smoke: zephyr-health
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_HEALTH_FIRMWARE="$(VAMS_HEALTH_FIRMWARE)" \
	./qemu/tests/smoke-vams-firmware-health.sh

firmware-pcie-smoke: zephyr
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/smoke-vams-firmware-pcie.py

mem-copy-smoke: firmware-pcie-smoke

mem-fill-smoke: firmware-pcie-smoke

crc32-smoke: firmware-pcie-smoke

vector-add-smoke: firmware-pcie-smoke

async-engine-smoke: firmware-pcie-smoke

scheduler-recovery-smoke: zephyr-scheduler-timeout
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	VAMS_SCHEDULER_FIRMWARE="$(VAMS_SCHEDULER_FIRMWARE)" \
	./qemu/tests/smoke-vams-scheduler-recovery.py

fault-injection-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/fault/vams-fault-injection.py

firmware-ownership-smoke:
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/qtest/vams-firmware-ownership.py

firmware-ownership-model-smoke:
	VAMS_SKIP_MANAGEMENT_PORTAL=1 \
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/qtest/vams-firmware-ownership.py

payload-throughput-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/performance/vams-payload-throughput.py

stress-smoke:
	@mkdir -p build/reports
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/stress/vams-stress-qualification.py \
		--commands 10000 --resets 32 --payload-samples 16 \
		--virtual-hours 1 \
		--json-output build/reports/stress-smoke.json

firmware-resource-report: zephyr
	@mkdir -p build/reports
	QEMU_SYSTEM_RISCV32="$(QEMU_SYSTEM_RISCV32)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	./qemu/tests/performance/vams-firmware-resources.py \
		--json-output build/reports/firmware-resources.json

stress-qualification: firmware-resource-report queue-model-smoke \
	cq-backpressure-smoke \
	payload-throughput-smoke
	@mkdir -p build/reports
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/stress/vams-stress-qualification.py \
		--commands "$(VAMS_STRESS_COMMANDS)" \
		--resets "$(VAMS_STRESS_RESETS)" \
		--payload-samples "$(VAMS_STRESS_PAYLOAD_SAMPLES)" \
		--virtual-hours "$(VAMS_STRESS_VIRTUAL_HOURS)" \
		--json-output build/reports/stress-qualification.json

dma-engine-smoke: firmware-pcie-smoke payload-throughput-smoke

pcie-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/smoke-vams-pcie.sh

nop-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/smoke-vams-nop.sh

queue-model-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/qtest/vams-queue-model.py

cq-backpressure-smoke:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/qtest/vams-cq-backpressure.py

descriptor-fuzz:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/fuzz/vams-descriptor-fuzz.py \
		--seed "$(VAMS_DESCRIPTOR_FUZZ_SEED)" \
		--iterations "$(VAMS_FUZZ_ITERATIONS)"

bar-fuzz:
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	./qemu/tests/fuzz/vams-bar-fuzz.py \
		--seed "$(VAMS_BAR_FUZZ_SEED)" \
		--iterations "$(VAMS_FUZZ_ITERATIONS)"

fuzz-smoke: descriptor-fuzz bar-fuzz

source-check:
	python3 -m py_compile scripts/*.py tests/abi/*.py \
		qemu/tests/*.py qemu/tests/qtest/*.py qemu/tests/fuzz/*.py \
		qemu/tests/fault/*.py \
		qemu/tests/performance/*.py qemu/tests/stress/*.py
	@set -eu; \
	for script in scripts/*.sh qemu/tests/*.sh kernel/tests/*.sh; do \
		sh -n "$$script"; \
	done
	git diff --check

assurance-smoke: check source-check queue-model-smoke cq-backpressure-smoke \
	fuzz-smoke \
	fault-injection-smoke

kernel:
	$(MAKE) -C kernel KERNEL_BUILD="$(KERNEL_BUILD)"

kernel-test-build:
	$(MAKE) -C kernel KERNEL_BUILD="$(KERNEL_BUILD)" VAMS_TESTING=1

kernel-uapi-test:
	@mkdir -p "$(dir $(VAMS_UAPI_TEST))"
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -static -pthread \
		-Ikernel/include/uapi kernel/tests/vams-uapi-test.c \
		-o "$(VAMS_UAPI_TEST)"

kernel-guest-init:
	@mkdir -p "$(dir $(VAMS_GUEST_INIT))"
	$(HOST_CC) -std=c11 -Wall -Wextra -Wpedantic -Werror -static \
		kernel/tests/vams-guest-init.c -o "$(VAMS_GUEST_INIT)"

kernel-smoke: kernel-test-build kernel-uapi-test kernel-guest-init
	QEMU_SYSTEM_X86_64="$(QEMU_SYSTEM_X86_64)" \
	VAMS_LINUX_IMAGE="$(VAMS_LINUX_IMAGE)" \
	VAMS_PCI_MODULE="$(VAMS_PCI_MODULE)" \
	VAMS_UAPI_TEST="$(VAMS_UAPI_TEST)" \
	VAMS_GUEST_INIT="$(VAMS_GUEST_INIT)" \
	GEN_INIT_CPIO="$(GEN_INIT_CPIO)" \
	./kernel/tests/smoke-vams-pci.sh

qemu-patch-check:
	@test -n "$(QEMU_SRC)" || { \
		echo 'usage: make qemu-patch-check QEMU_SRC=/path/to/qemu' >&2; \
		exit 2; \
	}
	@set -eu; \
	source=$$(cd "$(QEMU_SRC)" && pwd); \
	tmp=$$(mktemp -d); \
	trap 'rm -rf "$$tmp"' EXIT HUP INT TERM; \
	mkdir "$$tmp/qemu"; \
	git -C "$$tmp/qemu" init --quiet; \
	git -C "$$tmp/qemu" fetch --quiet --depth 1 "$$source" HEAD; \
	git -C "$$tmp/qemu" checkout --quiet --detach FETCH_HEAD; \
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
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0009-hw-misc-add-vams-crc32-engine.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0010-hw-misc-add-vams-vector-add-engine.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0010-hw-misc-add-vams-vector-add-engine.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0011-hw-misc-add-vams-asynchronous-engine.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0011-hw-misc-add-vams-asynchronous-engine.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0012-hw-misc-assert-vams-state-invariants.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0012-hw-misc-assert-vams-state-invariants.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0013-hw-misc-bound-vams-payload-dma-working-sets.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0013-hw-misc-bound-vams-payload-dma-working-sets.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0014-hw-misc-add-vams-engine-recovery-controls.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0014-hw-misc-add-vams-engine-recovery-controls.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0015-hw-misc-add-vams-deterministic-fault-controls.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0015-hw-misc-add-vams-deterministic-fault-controls.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0016-hw-misc-make-vams-firmware-own-engine-completions.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0016-hw-misc-make-vams-firmware-own-engine-completions.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0017-hw-misc-add-vams-bounded-firmware-abort.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0017-hw-misc-add-vams-bounded-firmware-abort.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0018-hw-misc-add-vams-reset-notification-and-cq-throttling.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0018-hw-misc-add-vams-reset-notification-and-cq-throttling.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0019-hw-misc-bound-firmware-reset-acknowledgment.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0019-hw-misc-bound-firmware-reset-acknowledgment.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0020-hw-misc-serialize-management-reset-ownership.patch"; \
	git -C "$$tmp/qemu" apply \
		"$(CURDIR)/qemu/patches/0020-hw-misc-serialize-management-reset-ownership.patch"; \
	git -C "$$tmp/qemu" apply --check \
		"$(CURDIR)/qemu/patches/0021-hw-misc-retain-firmware-health-evidence.patch"; \
	echo 'QEMU patch series check: PASS'

tree:
	@find . -path './.git' -prune -o -path './build' -prune -o -print | sort

demo:
	QEMU_SYSTEM_RISCV32="$(VAMS_DEMO_QEMU_RISCV32)" \
	QEMU_SYSTEM_X86_64="$(VAMS_DEMO_QEMU_X86_64)" \
	VAMS_ZEPHYR_FIRMWARE="$(VAMS_ZEPHYR_FIRMWARE)" \
	VAMS_DEMO_OUTPUT="$(VAMS_DEMO_OUTPUT)" \
		./scripts/vams-demo.py

clean:
	$(MAKE) -C kernel KERNEL_BUILD="$(KERNEL_BUILD)" clean
	rm -rf build out test-results coverage

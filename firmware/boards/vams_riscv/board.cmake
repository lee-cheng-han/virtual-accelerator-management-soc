# SPDX-License-Identifier: MIT

set(SUPPORTED_EMU_PLATFORMS qemu)
set(QEMU_binary_suffix riscv32)
set(QEMU_FLAGS_riscv
  -machine vams_riscv
  -m 512K
)
set(QEMU_KERNEL_OPTION "-bios;$<TARGET_FILE:zephyr_final>")

board_set_debugger_ifnset(qemu)

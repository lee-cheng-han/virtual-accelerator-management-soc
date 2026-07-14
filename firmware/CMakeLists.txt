# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(vams_management_firmware)

target_include_directories(app PRIVATE include)
target_sources(app PRIVATE
  src/main.c
  drivers/vams_mailbox.c
  drivers/vams_management.c
)

# Zephyr's RISC-V linker flow uses OMAGIC for a RAM-only image. Binutils 2.39+
# diagnoses the resulting load segment; the SRAM is intentionally executable
# and writable on this management subsystem.
target_link_options(app PUBLIC -Wl,--no-warn-rwx-segments)

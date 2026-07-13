CROSS_COMPILE ?= riscv64-unknown-elf-
CC := $(CROSS_COMPILE)gcc
OBJCOPY := $(CROSS_COMPILE)objcopy
SIZE := $(CROSS_COMPILE)size

BUILD_DIR ?= ../../build/firmware/baremetal
TARGET := $(BUILD_DIR)/vams-riscv-fw

CPPFLAGS := -I../include
CFLAGS := -march=rv32imac_zicsr -mabi=ilp32 -mcmodel=medany \
	-ffreestanding -fno-common -fno-builtin -fdata-sections \
	-ffunction-sections -fno-stack-protector -Wall -Wextra -Werror \
	-Wconversion -Wshadow -Wundef -Os -g3
ASFLAGS := $(CFLAGS)
LDFLAGS := -nostdlib -nostartfiles -static -Wl,--gc-sections \
	-Wl,--build-id=none -Wl,-Map,$(TARGET).map -T linker.ld

OBJS := $(BUILD_DIR)/start.o $(BUILD_DIR)/main.o

.PHONY: all clean

all: $(TARGET).elf $(TARGET).bin
	$(SIZE) $(TARGET).elf

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/start.o: start.S | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(ASFLAGS) -c $< -o $@

$(BUILD_DIR)/main.o: main.c ../include/vams_platform.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -c $< -o $@

$(TARGET).elf: $(OBJS) linker.ld
	$(CC) $(CFLAGS) $(LDFLAGS) $(OBJS) -o $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD_DIR)

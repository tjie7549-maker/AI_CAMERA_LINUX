SDK_DIR ?= /home/summary/linux/luckfox-pico
TOOLCHAIN ?= $(SDK_DIR)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-
CC := $(TOOLCHAIN)gcc
CFLAGS := -Os -Wall -Wextra -Werror -std=c11
TARGET := rtsp-preview-bridge

all: $(TARGET)

$(TARGET): rtsp_preview_bridge.c preview_shm_protocol.h
	$(CC) $(CFLAGS) -o $@ rtsp_preview_bridge.c -lrt

clean:
	rm -f $(TARGET)

.PHONY: all clean

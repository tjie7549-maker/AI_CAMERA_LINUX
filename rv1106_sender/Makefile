# Build the RV1106 VI-to-LCD demo from this directory.
SDK_DIR ?= ../../luckfox-pico
MEDIA_PARAM ?= $(SDK_DIR)/media/Makefile.param
TOOLCHAIN_DIR ?= $(SDK_DIR)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin
RK_MEDIA_CROSS ?= $(TOOLCHAIN_DIR)/arm-rockchip830-linux-uclibcgnueabihf

# Makefile.param verifies $(RK_MEDIA_CROSS)-gcc while it is included.  Keep the
# compiler prefix explicit so `make` works without a separate PATH export.

ifeq ($(wildcard $(MEDIA_PARAM)),)
$(error Cannot find $(MEDIA_PARAM). Set SDK_DIR to your Luckfox SDK directory)
endif

include $(MEDIA_PARAM)

TARGET := simple_vi_get_frame_send_vo_rv1106
MEDIA_SOURCES := $(filter-out src/npu_detect.c,$(wildcard src/*.c))
NPU_TARGET := npu_detect
NPU_SOURCE := src/npu_detect.c
RKNN_ROOT ?= /home/summary/linux/tools/rknn_model_zoo/3rdparty/rknpu2
RKNN_INCLUDE ?= $(RKNN_ROOT)/include
RKNN_LIB_DIR ?= $(RKNN_ROOT)/Linux/armhf-uclibc
SCRIPTS := run_simple_isp_vi_to_lcd_rv1106.sh run_ai_sender.sh run_ai_headless_preview.sh run_ai_terminal.sh
OUT_DIR := out

CC := $(RK_MEDIA_CROSS)-gcc

CPPFLAGS := \
	-Iinclude \
	-I$(RK_MEDIA_OUTPUT)/include \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq/uAPI2 \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq/common \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq/xcore \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq/algos \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq/iq_parser \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq/iq_parser_v2 \
	-I$(RK_MEDIA_OUTPUT)/include/rkaiq/smartIr

CFLAGS := -g -Wall -DRKPLATFORM=ON -DARCH64=OFF $(RK_MEDIA_CROSS_CFLAGS)
LDFLAGS := \
	-Wl,-rpath-link,$(RK_MEDIA_OUTPUT)/lib \
	-L$(RK_MEDIA_OUTPUT)/lib \
	-L$(RK_MEDIA_OUTPUT)/root/usr/lib \
	-Wl,--gc-sections -Wl,--as-needed
LDLIBS := -lrockchip_mpp -lrga -ldrm -lpthread -lstdc++ -lrockit_full -lrkaiq -lrtsp -lrt

.PHONY: all npu clean help

all: $(OUT_DIR)/$(TARGET) $(SCRIPTS:%=$(OUT_DIR)/%)

$(OUT_DIR):
	@mkdir -p $@

$(OUT_DIR)/$(TARGET): $(MEDIA_SOURCES) | $(OUT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(MEDIA_SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

npu: $(OUT_DIR)/$(NPU_TARGET)

$(OUT_DIR)/$(NPU_TARGET): $(NPU_SOURCE) include/preview_shm_protocol.h | $(OUT_DIR)
	$(CC) -Iinclude -I$(RKNN_INCLUDE) -O2 -Wall $(NPU_SOURCE) -o $@ \
		-L$(RKNN_LIB_DIR) -Wl,-rpath-link,$(RKNN_LIB_DIR) -lrknnmrt -lm -lrt

$(OUT_DIR)/%: % | $(OUT_DIR)
	install -m 755 $< $@

clean:
	rm -rf $(OUT_DIR)

help:
	@printf '%s\n' 'make        Build binary and startup script into out/'
	@printf '%s\n' 'make npu    Build the standalone RV1106 RKNN detector into out/'
	@printf '%s\n' 'make clean  Remove out/'
	@printf '%s\n' 'SDK_DIR=/path/to/luckfox-pico make  Use another SDK path'

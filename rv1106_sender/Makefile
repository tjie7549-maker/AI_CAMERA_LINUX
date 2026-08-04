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
SOURCES := $(wildcard src/*.c)
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

.PHONY: all clean help

all: $(OUT_DIR)/$(TARGET) $(SCRIPTS:%=$(OUT_DIR)/%)

$(OUT_DIR):
	@mkdir -p $@

$(OUT_DIR)/$(TARGET): $(SOURCES) | $(OUT_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

$(OUT_DIR)/%: % | $(OUT_DIR)
	install -m 755 $< $@

clean:
	rm -rf $(OUT_DIR)

help:
	@printf '%s\n' 'make        Build binary and startup script into out/'
	@printf '%s\n' 'make clean  Remove out/'
	@printf '%s\n' 'SDK_DIR=/path/to/luckfox-pico make  Use another SDK path'

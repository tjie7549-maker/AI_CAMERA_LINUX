SDK_DIR ?= /home/summary/linux/luckfox-pico
TOOLCHAIN ?= $(SDK_DIR)/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/arm-rockchip830-linux-uclibcgnueabihf-
CXX := $(TOOLCHAIN)g++
CPPFLAGS += -Iinclude
CXXFLAGS += -Os -Wall -Wextra -Werror -std=c++11
LDFLAGS += -pthread

TARGET := camera-daemon
SOURCES := src/main.cpp src/camera_daemon.cpp

all: $(TARGET)

$(TARGET): $(SOURCES) include/camera_daemon.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean

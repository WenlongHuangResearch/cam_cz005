# 双目 H265 录制 (C 版) —— MSYS2 / MinGW-w64 下构建
#
# 依赖: mingw-w64-x86_64-gcc, mingw-w64-x86_64-ffmpeg, mingw-w64-x86_64-pkgconf
# 在 "MSYS2 MINGW64" 终端里 make。

CC       ?= gcc
PKG_CONFIG ?= pkg-config
PKGS      = libavformat libavcodec libavdevice libavutil libswscale
CFLAGS   += -O2 -Wall -Wextra -std=gnu11 $(shell $(PKG_CONFIG) --cflags $(PKGS))
LDLIBS   += $(shell $(PKG_CONFIG) --libs $(PKGS)) -lpthread

SRC_REC = src/capture_record.c src/ring_buffer.c src/icm42688_decode.c src/encoder.c
SRC_MUX = src/hevc_to_mp4.c

BUILD_DIR ?= build
BIN_DIR   := $(BUILD_DIR)/bin
OBJ_DIR   := $(BUILD_DIR)/obj
UNAME_S   := $(shell uname -s 2>/dev/null)
ifneq (,$(findstring MINGW,$(UNAME_S)))
EXEEXT ?= .exe
else
EXEEXT ?=
endif

BIN_REC = $(BIN_DIR)/stereo_record$(EXEEXT)
BIN_MUX = $(BIN_DIR)/hevc2mp4$(EXEEXT)

OBJ_REC = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC_REC))
OBJ_MUX = $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SRC_MUX))

all: $(BIN_REC) $(BIN_MUX)

ci: clean all

$(OBJ_DIR)/%.o: src/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_REC): $(OBJ_REC) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_MUX): $(OBJ_MUX) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_DIR) $(OBJ_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)

.PHONY: all ci clean

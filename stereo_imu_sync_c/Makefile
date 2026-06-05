# 双目 H265 录制 (C 版) —— MSYS2 / MinGW-w64 下构建
#
# 依赖: mingw-w64-x86_64-gcc, mingw-w64-x86_64-ffmpeg, mingw-w64-x86_64-pkgconf
# 在 "MSYS2 MINGW64" 终端里 make。

CC      ?= gcc
PKGS     = libavformat libavcodec libavdevice libavutil libswscale
CFLAGS  += -O2 -Wall -Wextra -std=gnu11 $(shell pkg-config --cflags $(PKGS))
LDLIBS  += $(shell pkg-config --libs $(PKGS)) -lpthread

SRC_REC = src/capture_record.c src/ring_buffer.c src/icm42688_decode.c src/encoder.c
SRC_MUX = src/hevc_to_mp4.c

BIN_REC = stereo_record
BIN_MUX = hevc2mp4

all: $(BIN_REC) $(BIN_MUX)

$(BIN_REC): $(SRC_REC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BIN_MUX): $(SRC_MUX)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(BIN_REC) $(BIN_MUX) $(BIN_REC).exe $(BIN_MUX).exe

.PHONY: all clean

COMPRESSOR=./multiband_compressor/compressor/main.c
MUXER=./multiband_compressor/freq_mux/main.c
MULTIBAND=./multiband_compressor/main.c
MULTIPOLE=./multiband_compressor/multi_pole/multi_pole.c
RC=./multiband_compressor/multi_pole/RC/rc.c
ALSA=alsa_pipe/main.c
LIMITER=./lookahead_limiter/lookaheadlim.c
DEXPANDER=./downward_expander/dxpander.c
MPX=./MPX/generator.c
PARAMS=./config/params.c
WEBUI=./webui/http.c
STREAMER=./source/streamer.c
RDS=./rds/rds.c

SOURCES=audio_processing.c $(DEXPANDER) $(LIMITER) $(MPX) $(COMPRESSOR) \
	$(MUXER) $(MULTIBAND) $(MULTIPOLE) $(RC) $(ALSA) $(PARAMS) $(WEBUI) $(STREAMER) $(RDS)

TARGET=touhouradio
PAGE=webui/page.h

# ARCH is the only architecture specific part. -march=native and -mfpmath are
# x86 only, so every arm target below replaces ARCH completely.
ARCH?=-march=native -mfpmath=both
OPT?=-O3 -fno-fast-math
FLAGS=$(OPT) $(ARCH)
LIBS=-lm -lasound -lpthread

all: build

# always recompiles, like the original Makefile did. Everything is built in one
# go, and it means editing DEFAULTS.h or switching to an arm target actually
# rebuilds instead of reporting the binary as up to date.
build: $(PAGE)
	$(CC) $(SOURCES) $(LIBS) $(FLAGS) -Wall -o $(TARGET)

# The control page is baked into the binary so the program stays a single
# file you can copy anywhere. sed turns the html into one C string literal.
$(PAGE): webui/index.html
	@printf '/* generated from webui/index.html by the Makefile, do not edit */\n' > $@
	@printf '#ifndef VOSTOK_WEBUI_PAGE\n#define VOSTOK_WEBUI_PAGE\n' >> $@
	@printf 'static const char WEBUI_INDEX_HTML[] =\n' >> $@
	@sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' webui/index.html >> $@
	@printf ';\n#endif\n' >> $@

# --------------------------------------------------------------- cross builds
#
# Raspberry Pi, built on the Pi itself. Nothing generic, just safe flags.
pi: ARCH=-mcpu=native
pi: build

# Raspberry Pi 3/4/5 running 64 bit Raspberry Pi OS or Ubuntu
pi64: ARCH=-march=armv8-a+crc -mtune=cortex-a72
pi64: build

# Raspberry Pi 2/3/4 running 32 bit Raspberry Pi OS
pi32: ARCH=-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -mtune=cortex-a7
pi32: build

# Raspberry Pi Zero / Zero W / 1
pi0: ARCH=-march=armv6zk -mfpu=vfp -mfloat-abi=hard -mtune=arm1176jzf-s
pi0: build

# Cross compile from a PC to 64 bit Pi.
# needs: aarch64-linux-gnu-gcc and an arm64 libasound2-dev in SYSROOT
CROSS64?=aarch64-linux-gnu-gcc
SYSROOT?=
cross-pi64: $(PAGE)
	$(CROSS64) $(if $(SYSROOT),--sysroot=$(SYSROOT)) $(SOURCES) $(LIBS) \
		$(OPT) -march=armv8-a+crc -mtune=cortex-a72 -Wall -o $(TARGET)

# Cross compile from a PC to 32 bit Pi.
CROSS32?=arm-linux-gnueabihf-gcc
cross-pi32: $(PAGE)
	$(CROSS32) $(if $(SYSROOT),--sysroot=$(SYSROOT)) $(SOURCES) $(LIBS) \
		$(OPT) -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -Wall -o $(TARGET)

# One binary with no shared library dependencies at all.
# Needs static versions of libasound and libc, see the README.
static: $(PAGE)
	$(CC) $(SOURCES) $(FLAGS) -Wall -static -o $(TARGET) \
		-Wl,--start-group -lasound -lm -lpthread -ldl -Wl,--end-group

# the same thing on a Pi, where -march=native does not exist
pi-static: ARCH=-mcpu=native
pi-static: static

clean:
	rm -f $(TARGET) $(PAGE)

.PHONY: all build pi pi64 pi32 pi0 cross-pi64 cross-pi32 static pi-static clean

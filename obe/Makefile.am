AUTOMAKE_OPTIONS = foreign
GIT_VERSION := $(shell git describe --abbrev=8 --dirty --always --tags)

BLACKMAGIC_SDK_PATH = $(top_srcdir)/include/decklink-sdk

# This will be ignored of libklmonitoring is not installed in the system.
#HISTOGRAMS = -DPRINT_HISTOGRAMS=1

CFLAGS += -Wall -DVERSION=\"$(VERSION)\" -DPROG="\"$(PACKAGE)\"" -D_FILE_OFFSET_BITS=64 -O3 \
	-D_BSD_SOURCE $(HISTOGRAMS) \
	-I$(BLACKMAGIC_SDK_PATH) \
	-I$(top_srcdir) \
	-DKL_USERSPACE -D__STDC_FORMAT_MACROS \
	-DGIT_VERSION=\"$(GIT_VERSION)\" \
	-DHAVE_DECKLINK

LDFLAGS += -lpthread -lz -lx264 -lyuv
LDFLAGS += -lavformat -lavcodec -lavresample -ldl -lasound -lfdk-aac -lbz2 -lz -lswscale -lavutil -lm
LDFLAGS += -lklscte35 -lklvanc
LDFLAGS += -ltwolame -lmpegts -lstdc++ -ldl -lzvbi -lreadline

#OK

if DEBUG
	CFLAGS += -g
endif

CXXFLAGS = $(CFLAGS)

bin_PROGRAMS  = obecli

x86_sdi.o:
	yasm -f elf -m amd64 -DARCH_X86_64=1 -DHAVE_CPUNOP=1 -I../common/x86/ -o x86_sdi.o ../input/sdi/x86/x86_sdi.asm

vfilter.o:
	yasm -f elf -m amd64 -DARCH_X86_64=1 -DHAVE_CPUNOP=1 -I../common/x86/ -o vfilter.o ../filters/video/x86/vfilter.asm

obecli_SOURCES  = obe.c
obecli_SOURCES += obecli.c
obecli_SOURCES += ../mux/smoothing.c
obecli_SOURCES += ../mux/ts/ts.c
obecli_SOURCES += ../output/ip/ip.c
obecli_SOURCES += ../input/sdi/ancillary.c
obecli_SOURCES += ../input/sdi/sdi.c
obecli_SOURCES += ../input/sdi/vbi.c
obecli_SOURCES += ../input/sdi/smpte337_detector.c
obecli_SOURCES += ../input/sdi/decklink/decklink.cpp
obecli_SOURCES += ../input/sdi/linsys/linsys.c
obecli_SOURCES += ../input/sdi/v4l2/v4l2.cpp
obecli_SOURCES += ../filters/audio/audio.c
obecli_SOURCES += ../filters/audio/337m/337m.c
obecli_SOURCES += ../filters/video/cc.c
obecli_SOURCES += ../filters/video/video.c
obecli_SOURCES += ../encoders/encoder_smoothing.c
obecli_SOURCES += ../encoders/audio/lavc/lavc.c
obecli_SOURCES += ../encoders/audio/mp2/twolame.c
obecli_SOURCES += ../encoders/audio/ac3bitstream/ac3bitstream.c
obecli_SOURCES += ../encoders/audio/ac3bitstream/klringbuffer.c
obecli_SOURCES += ../encoders/video/avc/x264.c
obecli_SOURCES += ../common/network/udp/udp.c
obecli_SOURCES += ../common/linsys/util.c
obecli_SOURCES += ../common/x86/x86inc.asm
obecli_SOURCES += ../common/x86/x86util.asm
obecli_SOURCES += ../common/common_lavc.c
obecli_SOURCES += x86_sdi.o
obecli_SOURCES += vfilter.o

obecli_DEPENDENCIES  = x86_sdi.o
obecli_DEPENDENCIES += vfilter.o

obecli_LDFLAGS = vfilter.o x86_sdi.o

libklvanc_noinst_includedir = $(includedir)

noinst_HEADERS  = $(top_srcdir)/mux/mux.h

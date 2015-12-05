SOURCES=\
hdhomerun_channels.c \
hdhomerun_channelscan.c \
hdhomerun_control.c \
hdhomerun_debug.c \
hdhomerun_device.c \
hdhomerun_device_selector.c \
hdhomerun_discover.c \
hdhomerun_os_posix.c \
hdhomerun_pkt.c \
hdhomerun_sock_posix.c \
hdhomerun_video.c \


CFLAGS = -g -std=c99 -D_DEFAULT_SOURCE -Wpointer-arith
LDFLAGS = -lpthread

OBJECTS=$(SOURCES:%.c=%.o)

all: libhdhomerun.a

libhdhomerun.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)

hdhomerun_config: hdhomerun_config.c $(OBJECTS)
	$(CC) $< $(OBJECTS) -o $@ $(CFLAGS) $(LDFLAGS)

clean:
	$(RM) *.o *.a hdhomerun_config




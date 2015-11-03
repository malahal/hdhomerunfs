SOURCES=\
./deps/hdhomerun/hdhomerun_channels.c \
./deps/hdhomerun/hdhomerun_channelscan.c \
./deps/hdhomerun/hdhomerun_control.c \
./deps/hdhomerun/hdhomerun_debug.c \
./deps/hdhomerun/hdhomerun_device.c \
./deps/hdhomerun/hdhomerun_device_selector.c \
./deps/hdhomerun/hdhomerun_discover.c \
./deps/hdhomerun/hdhomerun_os_posix.c \
./deps/hdhomerun/hdhomerun_pkt.c \
./deps/hdhomerun/hdhomerun_sock_posix.c \
./deps/hdhomerun/hdhomerun_video.c \


CFLAGS   = -g -std=c99 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wpointer-arith -Ideps/
LDFLAGS  = -lm -lpthread -lrt -lfuse

OBJECTS=$(SOURCES:%.c=%.o)

all: hdhomerunfs

hdhomerunfs: hdhomerunfs.c $(OBJECTS)
	$(CC) $< $(OBJECTS) -o $@ $(CFLAGS) $(LDFLAGS)

clean:
	$(RM) *.o deps/hdhomerun/*.o



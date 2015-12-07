SOURCES=\
./deps/mmapring/mmapring.c \

DEPS=\
./deps/hdhomerun/libhdhomerun.a \
./deps/inih/libinih.a \


DEPS_LDFLAGS= 
DEPS_LDFLAGS+= $(foreach dep,$(DEPS),-L$(dir $(dep)) -l$(subst lib,,$(notdir $(basename $(dep)))))

CFLAGS   = -g -std=c99 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -Wpointer-arith -Ideps/
LDFLAGS  = -lm -lpthread -lrt -lfuse $(DEPS_LDFLAGS)

OBJECTS=$(SOURCES:%.c=%.o)

all: hdhomerunfs channelscan

%.a: 
	$(MAKE) -C $(@D)

hdhomerunfs: hdhomerunfs.c $(OBJECTS) $(DEPS)
	$(CC) $< $(OBJECTS) -o $@ $(CFLAGS) $(LDFLAGS)

channelscan: channelscan.c $(OBJECTS) $(DEPS)
	$(CC) $< $(OBJECTS) -o $@ $(CFLAGS) $(LDFLAGS)

.PHONY: clean
clean:
	$(RM) hdhomerunfs channelscan *.o deps/mmapring/*.o
	$(MAKE) -C deps/hdhomerun clean
	$(MAKE) -C deps/inih clean


SOURCES=\
ini.c \

CFLAGS   = -g -std=c99 -Wpointer-arith 
LDFLAGS  = 

OBJECTS=$(SOURCES:%.c=%.o)

all: libinih.a

libinih.a: $(OBJECTS)
	$(AR) rcs $@ $(OBJECTS)

clean:
	$(RM) *.o *.a



hdhomerunfs: hdhomerunfs.c
	cc -g -D_FILE_OFFSET_BITS=64 -o hdhomerunfs hdhomerunfs.c -lfuse

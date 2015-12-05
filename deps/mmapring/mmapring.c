#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>

#include "mmapring.h"

mmapring_t* mmapring_create(const char *path, off_t size)
{
	mmapring_t *rng = calloc(1, sizeof(mmapring_t));
	if (!rng) return 0;

	rng->size = sysconf(_SC_PAGE_SIZE);
	while(rng->size < size) {
		rng->size = rng->size * 2;
	}

	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0600); 
	if (fd < 0) {
		free(rng);
		return 0;
	}
	//unlink(path);

	if (ftruncate(fd, rng->size) < 0) {
		free(rng);
		return 0;
	}

	rng->base = mmap(0, rng->size << 1, PROT_NONE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
	if (rng->base == MAP_FAILED) {
		free(rng);
		close(fd);
		return 0;
	}

	char *base = mmap(rng->base, rng->size, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_SHARED, fd, 0);
	if (base != rng->base) {
		munmap(rng->base, rng->size << 1);

		free(rng);
		close(fd);
		return 0;
	}

	base = mmap(rng->base + rng->size, rng->size, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_SHARED, fd, 0);
	if (base != (rng->base + rng->size)) {
		munmap(rng->base, rng->size << 1);

		free(rng);
		close(fd);
		return 0;
	}

	close(fd);

	return rng;
}

void mmapring_destroy(mmapring_t *rng)
{
	if (rng) {
		if (munmap(rng->base, rng->size * 2) < 0) {
			perror("munmap");
		}
		free(rng);
	}
}

off_t mmapring_append(mmapring_t *rng, const char *p, off_t len)
{
	off_t wlen = rng->size;
	if (wlen <= 0) {
		errno = ENOSPC;
		return 0;
	}
	wlen = (wlen > len ? len : wlen);

	memcpy(rng->base + rng->write_offset, p, wlen);
	rng->written += wlen;
	rng->write_offset += wlen;
	if (rng->write_offset > rng->size) {
		rng->write_offset = rng->write_offset % rng->size;
	}

	return wlen;
}

void mmapring_reset(mmapring_t *rng)
{
	rng->write_offset = 0;
	rng->written = 0;
}

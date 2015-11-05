#ifndef MMAPRING_H_
#define MMAPRING_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	char *base;
	off_t size;
	off_t write_offset;
	off_t written;
} mmapring_t;


mmapring_t *mmapring_create(const char *path, off_t size);
void mmapring_destroy(mmapring_t *rng);
off_t mmapring_append(mmapring_t *rng, const char *p, off_t len);
void mmapring_reset(mmapring_t *rng);

#ifdef __cplusplus
}
#endif

#endif //MMAPRING_H_

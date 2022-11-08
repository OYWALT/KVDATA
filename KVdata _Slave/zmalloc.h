#ifndef KVDATA_ZMALLOC_H
#define KVDATA_ZMALLOC_H



void *zmalloc(size_t size);
void *zrealloc(void *ptr, size_t size);
void zfree(void *ptr);
size_t zmalloc_size(void *ptr);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
//其中\用于宏定义和字符串换行


// 实时统计数据库内存管理模块已经申请了多少空间
static size_t used_memory = 0;

//静态初始化线程锁，由于内存分配可能发生在各个线程中，所以对这个数据的管理要做到原子性。
pthread_mutex_t used_memory_mutex = PTHREAD_MUTEX_INITIALIZER;

//一般来说，锁操作比原子操作慢。但是在不支持原子操作的系统上只能使用锁机制了。
//采用线程安全的模式更新数据库中内存管理模块申请的空间大小
#ifdef HAVE_ATOMIC
#define update_zmalloc_stat_add(n) __sync_add_and_fetch(&used_memory, (n))
#define update_zmalloc_stat_sub(n) __sync_sub_and_fetch(&used_memory, (n))
#else
#define update_zmalloc_stat_add(n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory += (n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)

#define update_zmalloc_stat_sub(n) do { \
    pthread_mutex_lock(&used_memory_mutex); \
    used_memory -= (n); \
    pthread_mutex_unlock(&used_memory_mutex); \
} while(0)
#endif

//但是作为一个基础库，它不能仅仅考虑到多线程的问题。
//比如用户系统上不支持原子操作，用户希望拥有多线程安全特性
//_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)此操作是使得内存对齐
#define update_zmalloc_stat_alloc(n) do { \
    size_t _n = (n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    update_zmalloc_stat_add(_n); \
} while(0)

#define update_zmalloc_stat_free(n) do { \
    size_t _n = (n); \
    if (_n&(sizeof(long)-1)) _n += sizeof(long)-(_n&(sizeof(long)-1)); \
    update_zmalloc_stat_sub(_n); \
} while(0)



/*
tcmalloc 和 Mac平台下的 malloc 函数族提供了计算已分配空间大小的函数
（分别是tcmallocsize和mallocsize），所以就不需要单独分配一段空间记录大小了。

而针对linux和sun平台则要记录分配空间大小。对于linux，使用sizeof(sizet)定长字段记录；
对于sun os，使用sizeof(long long)定长字段记录。

因此当宏HAVE_MALLOC_SIZE没有被定义的时候，就需要在多分配出的空间内记录下当前申请的内存空间的大小。
*/
#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
#if defined(__sun) || defined(__sparc) || defined(__sparc__)
#define PREFIX_SIZE (sizeof(long long))
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif



/*
 * 在内存分配时处理内存溢出的处理。
 */
void zmalloc_oom_handler(size_t size) {
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %zu bytes\n",
            size);
    fflush(stderr);
    //直接退出当前进程
    abort();
}

/* 
 * 单片内存分配 
 * 分配一块size大小的内存
 */
void *zmalloc(size_t size) {
    //开始时，zmalloc直接分配了一个比申请空间大的空间
    void *ptr = malloc(size+PREFIX_SIZE);
    if (!ptr) zmalloc_oom_handler(size);

//如果内存库支持，则通过zmalloc_size获取刚分配的空间大小，并累计到记录整个程序申请的堆空间大小上，然后返回申请了的地址。
//此时虽然用户申请的只是size的大小，但是实际给了size+PREFIX_SIZE的大小。
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
//如果内存库不支持，则在申请的内存前sizeof(size_t)大小的空间里保存用户需要申请的空间大小size。
//累计到记录整个程序申请堆空间大小上的也是实际申请的大小。
//最后返回的是偏移了头大小的内存地址。此时用户拿到的空间就是自己要求申请的空间大小。    
#else
    *((size_t*)ptr) = size;//保存分配的size地址的大小
    update_zmalloc_stat_alloc(size+PREFIX_SIZE);
    return (char*)ptr+PREFIX_SIZE;//移动到真正保存分配的空间处
#endif
}

/*
 * 重新分配空间
 * 在ptr指针处分配一块大小为size字节的空间
 * 分配空间比原来ptr指针处的大，保留原来的元素
 * 分配空间比原来ptr指针处的小，截断一部分多出的空间元素
 * 
 * 它需要在统计程序以申请堆空间大小的数据上减去以前该块的大小，再加上新申请的空间大小
 * （理解具体实现参考zmalloc即可）
 */
void *zrealloc(void *ptr, size_t size) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    if (ptr == NULL) return zmalloc(size);
#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (!newptr) zmalloc_oom_handler(size);

    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(zmalloc_size(newptr));
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (!newptr) zmalloc_oom_handler(size);

    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    return (char*)newptr+PREFIX_SIZE;
#endif
}

/*
 * 释放指定地址内存
 */
void zfree(void *ptr) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL) return;

//如果库支持获取区块大小，则直接释放该起始地址的空间
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
//如果库不支持获取区块大小，则需要将传入的指针前移PREFIX_SIZE，然后释放该起始地址的空间。
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    update_zmalloc_stat_free(oldsize+PREFIX_SIZE);
    free(realptr);
#endif
}

/*
 * 获取地址ptr的内存空间大小
 */
#ifndef HAVE_MALLOC_SIZE
size_t zmalloc_size(void *ptr) {
    void *realptr = (char*)ptr-PREFIX_SIZE;
    size_t size = *((size_t*)realptr);
    /*这段代码判断内存空间的大小是不是8的倍数。
     *malloc()本身能够保证所分配的内存是8字节对齐的：
     *如果你要分配的内存不是8的倍数，那么malloc就会多分配一点，来凑成8的倍数。 */
    if (size&(sizeof(long)-1)) size += sizeof(long)-(size&(sizeof(long)-1));
    return size+PREFIX_SIZE;
}
#endif
#ifndef KVDATA_SDS_H
#define KVDATA_SDS_H

/* 过期时间的输入方式 默认ms*/
#define UNIT_SECONDS 0
#define UNIT_MILLISECONDS 1

#include <stdio.h>
#include <stdarg.h>
/*类型别名，用于指向 sdshdr 的 buf 属性*/
typedef char *sds;

/* 保存字符串对象的结构*/
struct sdshdr {
    
    // buf 中已占用空间的长度
    int len;
    // buf 中剩余可用空间的长度
    int free;
    // 数据空间
    char buf[];
};

sds sdsnewlen(const void *init, size_t initlen);
sds sdsdup(const sds s);
void sdsfree(sds s);
size_t sdslen(const sds s);
sds sdsnew(const char *init);
size_t sdsavail(const sds s);
sds sdsMakeRoomFor(sds s, size_t addlen);
void sdsIncrLen(sds s, int incr);
void sdsrange(sds s, int start, int end);
sds sdscatlen(sds s, const void *t, size_t len);
size_t zmalloc_size_sds(sds s);


sds sdscatprintf(sds s, const char *fmt, ...);
sds sdscatvprintf(sds s, const char *fmt, va_list ap);
#endif
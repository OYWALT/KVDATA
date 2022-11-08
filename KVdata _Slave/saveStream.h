#ifndef KVDATA_SAVESTREAM_H
#define KVDATA_SAVESTREAM_H

#include <stdio.h>
#include <stddef.h>
#include "sds.h"
#include "object.h"
#include <sys/types.h>
typedef unsigned long int uint64_t;
/*
 * saveStream API 接口和状态
 * 主要是对某一种流（stream）的状态进行描述
 */
typedef struct saveStream {
    //数据流的读方法
    size_t (*read)(struct saveStream *, void *buf, size_t len);
    //数据流的写方法
    size_t (*write)(struct saveStream *, const void *buf, size_t len);
    //获取当前的读写偏移量
    off_t (*tell)(struct saveStream *);
    // 校验和计算函数，每次有写入/读取新数据时都要计算一次
    void (*update_cksum)(struct saveStream *, const void *buf, size_t len);/* 数据流的校验和计算方法 */

    // 当前校验和
    long long cksum;
  
    /* saveStream中I/O变量 */
    union {

        /* 缓冲区 */
        struct {
            // 缓存指针
            sds ptr;
            // 偏移量
            off_t pos;
        } buffer;

        /* 文件 */
        struct {
            // 被打开文件的指针
            FILE *fp;
            /*------------------下面两项主要用于判断是否执行fsync()----------------------*/ 
            // 最近一次 fsync() 以来，写入的字节量
            off_t buffered; 
            // 写入多少字节之后，才会自动执行一次 fsync()
            off_t autosync; 
        } file;

    } io;
}saveStream;



//函数声明
size_t saveStreamFileWrite(saveStream *r, const void *buf, size_t len);
static size_t saveStreamFileRead(saveStream *r, void *buf, size_t len);
static off_t saveStreamFileTell(saveStream *r);
void saveStreamSetAutoSync(saveStream *r, off_t bytes);
void saveStreamInitWithFile(saveStream *r, FILE *fp);

static size_t saveStreamBufferRead(saveStream *r, void *buf, size_t len);
static size_t saveStreamBufferWrite(saveStream *r, const void *buf, size_t len);
static off_t saveStreamBufferTell(saveStream *r);
void saveStreamInitWithBuffer(saveStream *r, sds s);
size_t saveStreamWrite(saveStream *r, const void *buf, size_t len);
size_t saveStreamWriteBulkLongLong(saveStream *r, long long l);
size_t saveStreamWriteBulkString(saveStream *r, const char *buf, size_t len);
size_t saveStreamWriteBulkCount(saveStream *r, char prefix, int count);
int saveStreamWriteBulkObject(saveStream *r, robj *obj);

size_t saveStreamRead(saveStream *r, void *buf, size_t len);
void saveStreamGenericUpdateChecksum(saveStream *r, const void *buf, size_t len);
uint64_t crc64(uint64_t crc, const unsigned char *s, uint64_t l);




#endif  //_saveStream_H_
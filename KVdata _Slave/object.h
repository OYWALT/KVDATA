#ifndef KVDATA_OBJECT_H
#define KVDATA_OBJECT_H
#include <stdio.h>
#include "list.h"
#define STRING 1   //字符串类型的对象编码
#define INT    2   //整数类型的对象编码

//共享参数长度的对象，长度限制
#define KVDATA_SHARED_BULKHDR_LEN 32  
//unsigned类型本身是4字节
//此处在定义变量后加上：4,说明限制该变量只占4个bit位
typedef struct KVDATAObject {

    // 编码
    int encoding;

    // 引用计数
    int refcount;

    // 指向实际值的指针
    void *ptr;

} robj;

// 通过复用来减少内存碎片，以及减少操作耗时的共享对象
struct sharedObjectsStruct {
    robj *crlf, *ok, *err, *pong, *queued, *syntaxerr, *nullbulk, *wrongtypeerr,
    *execaborterr,
    *mbulkhdr[KVDATA_SHARED_BULKHDR_LEN], /* "*<value>\r\n" */
    *bulkhdr[KVDATA_SHARED_BULKHDR_LEN];  /* "$<value>\r\n" */
};

void incrRefCount(robj *o);
void decrRefCount(robj *o);
void createSharedObjects(void);
robj *createObject(int encoding, void *ptr);
robj *dupLastObject(list *reply);
robj *createStringObject(char *ptr, size_t len);
void freeStringObject(robj *o);
void freeIntObject(robj *o);
int getLongLongFromObject(robj *o, long long *target);
#endif
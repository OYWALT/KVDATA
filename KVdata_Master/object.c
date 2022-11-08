#include "object.h"
#include <stdio.h>
#include "zmalloc.h"
#include "sds.h"
#include <errno.h>
#include "events.h"
#include <stdlib.h>
#include <ctype.h>
#include "list.h"
#include "assert.h"

struct sharedObjectsStruct shared;
/*
 * 初始化共享对象
 */
void createSharedObjects(void) {
    int j;

    // 常用回复
    shared.crlf = createObject(STRING,sdsnew("\r\n"));
    shared.ok = createObject(STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(STRING,sdsnew("-ERR\r\n"));
    shared.pong = createObject(STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(STRING,sdsnew("+QUEUED\r\n"));
    shared.nullbulk = createObject(STRING,sdsnew("$-1\r\n"));
    // 常用错误回复
    shared.syntaxerr = createObject(STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.wrongtypeerr = createObject(STRING,sdsnew(
        "-WRONGTYPE Operation against a key holding the wrong kind of value\r\n"));
    shared.execaborterr = createObject(STRING,sdsnew(
        "-EXECABORT Transaction discarded because of previous errors.\r\n"));
    // 常用长度 bulk 或者 multi bulk 回复
    for (j = 0; j < KVDATA_SHARED_BULKHDR_LEN; j++) {
        shared.mbulkhdr[j] = createObject(STRING,
            sdscatprintf(sdsnewlen("",0),"*%d\r\n",j));
        shared.bulkhdr[j] = createObject(STRING,
            sdscatprintf(sdsnewlen("",0),"$%d\r\n",j));
    }
}


/*
 * 为对象的引用计数增一
 */
void incrRefCount(robj *o) {
    o->refcount++;
}

/*
 * 为对象的引用计数减一
 * 当对象的引用计数降为 0 时，释放对象。
 */
void decrRefCount(robj *o) {
    if (o->refcount <= 0) 
    printf("decrRefCount against refcount <= 0.");

    // 释放对象
    if (o->refcount == 1) {
        switch(o->encoding) {
        case STRING: freeStringObject(o); break;
        case INT: freeIntObject(o); break;
        default:  
        printf("Unknown object type.\n"); break;
        }
        zfree(o);
    // 减少计数
    } else {
        o->refcount--;
    }
}


/*
 * 创建一个新 robj 对象
 */
robj *createObject(int encoding, void *ptr) {

    robj *o = zmalloc(sizeof(*o));
    //创建对象时，对象的引用计数+1
    //编码类型设置为简单的字符串STRING
    o->encoding = encoding;
    o->ptr = ptr;
    o->refcount = 1;

    return o;
}

/* 
 * 当可变输出缓冲区最后一个节点对象的是一个共享对象
 * 创建该对象的一个复制品，用复制品替换该对象，原对象引用计数-1
 * 防止修改共享对象，影响其它进程使用
 */
robj *dupLastObject(list *reply) {
    robj *new, *cur;
    listNode *ln;
    assert(listLength(reply) > 0);
    ln = listLast(reply);
    cur = listNodeValue(ln);
    if (cur->refcount > 1) {
        //创建一个新的对象
        new = createObject(STRING, cur->ptr);
        //原来对象引用计数-1
        decrRefCount(cur);
        listNodeValue(ln) = new;
    }
    return listNodeValue(ln);
}



/*
 * 创建一个 STRING 编码的字符串对象
 * 返回值：被创建的对象
 */
robj *createStringObject(char *ptr, size_t len) {
    //sdsnewlen(ptr,len)根据字符串指针ptr以及指定字节长度len，初始化一个sdshdr结构变量
    return createObject(STRING,sdsnewlen(ptr,len));
}

void freeStringObject(robj *o)
{
     sdsfree(o->ptr);
}

void freeIntObject(robj *o)
{
}

/*
 * 尝试从对象 o 中取出整数值，
 * 或者尝试将对象 o 所保存的值转换为整数值，
 * 并将这个整数值保存到 *target 中。
 *
 * 如果 o 为 NULL ，那么将 *target 设为 0 。
 *
 * 如果对象 o 中的值不是整数，并且不能转换为整数，那么函数返回 KVDATA_ERR 。
 *
 * 成功取出或者成功进行转换时，返回 KVDATA_OK 。
 *
 * T = O(N)
 */
int getLongLongFromObject(robj *o, long long *target) {
    long long value;
    char *eptr;

    if (o == NULL) {
        // o 为 NULL 时，将值设为 0 。
        value = 0;
    } else {

        if (o->encoding == STRING) {
            //strtoll函数
            //String是要转化的字符串。endptr Endptr保存函数结束前的那个非合法字符的地址。Radix说明nptr的进制。
            //eg:ret=strtoll("123abc", &eptr, 10);   ret=123  eptr=abc
            value = strtoll(o->ptr, &eptr, 10);
            if (isspace(((char*)o->ptr)[0]) || eptr[0] != '\0')
                return AE_ERR;//获取整数值报错
        } else if (o->encoding == INT) {
            // 对于 KVDATA_ENCODING_INT 编码的整数值
            // 直接将它的值保存到 value 中
            value = (long)o->ptr;
        } else {
            printf("Unknown string encoding.\n");
        }
    }

    // 保存值到指针
    if (target) *target = value;

    // 返回结果标识符
    return AE_OK;
}
#include "sds.h"
#include <string.h>
#include "zmalloc.h"
#include "assert.h"
#include "client.h"
#include "server.h"
#include "util.h"
extern struct sharedObjectsStruct shared;
/*
 * 根据给定的初始化字符串 init 和字符串长度 initlen
 * 创建一个新的 sds
 * 返回值
 *        创建成功返回 sdshdr 相对应的 sds
 *        创建失败返回 NULL
 *  T = O(1)
 */
sds sdsnewlen(const void *init, size_t initlen) {

    struct sdshdr *sh;
    // zmalloc 不初始化所分配的内存
    sh = zmalloc(sizeof(struct sdshdr)+initlen+1);
    // 内存分配失败，返回
    if (sh == NULL) return NULL;

    // 设置初始化长度
    sh->len = initlen;

    // 新 sds 不预留任何空间
    sh->free = 0;

    // 如果有指定初始化内容，将它们复制到 sdshdr 的 buf 中
    if (initlen && init)
        memcpy(sh->buf, init, initlen);
    // 以 \0 结尾
    sh->buf[initlen] = '\0';

    // 返回 buf 部分，而不是整个 sdshdr
    return (char*)sh->buf;
}

/*
 * 根据给定字符串 init ，创建一个包含同样字符串的 sds
 * 返回值
 *        创建成功返回 sdshdr 相对应的 sds
 *        创建失败返回 NULL
 *  T = O(N)
 */
sds sdsnew(const char *init) {
    size_t initlen = (init == NULL) ? 0 : strlen(init);
    return sdsnewlen(init, initlen);
}
/*
 * 复制给定 sds 的副本
 * 返回值
          创建成功返回输入 sds 的副本
 *        创建失败返回 NULL
 */
sds sdsdup(const sds s) {
    return sdsnewlen(s, sdslen(s));
}

/*
 * 释放给定的 sds
 *  T = O(N)
 */
void sdsfree(sds s) {
    if (s == NULL) return;
    zfree(s-sizeof(struct sdshdr));
}

/*
 * 返回 sds 实际保存的字符串的长度
 * T = O(1)
 */
size_t sdslen(const sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->len;
}

/*
 * 返回 sds 对应结构剩余可用空闲空间
 * T = O(1)
 */
size_t sdsavail(const sds s) {
    struct sdshdr *sh = (void*)(s-(sizeof(struct sdshdr)));
    return sh->free;
}

/*
 * 对 sds 中 buf 的长度进行扩展，确保在函数执行之后，
 * buf 至少会有 addlen + 1 长度的空余空间（额外的 1 字节是为 \0 准备的）
 * 返回值
 *        扩展成功返回扩展后的 sds
 *        扩展失败返回 NULL
 *  T = O(N)
 */
sds sdsMakeRoomFor(sds s, size_t addlen) {

    struct sdshdr *sh, *newsh;

    // 获取 s 目前的空余空间长度
    size_t free = sdsavail(s);

    size_t len, newlen;

    // s 目前的空余空间已经足够，无须再进行扩展，直接返回
    if (free >= addlen) return s;

    // 获取 s 目前已占用空间的长度
    len = sdslen(s);
    sh = (void*) (s-(sizeof(struct sdshdr)));

    // s 最少需要的长度*2,一次多分配一些空间，避免频繁分配内存
    newlen = 2*(len+addlen);

    //ptr -- 指针指向一个要重新分配内存的内存块，该内存块之前是通过调用 malloc、calloc 或 realloc 进行分配内存的。如果为空指针，则会分配一个新的内存块，且函数返回一个指向它的指针。
    //size -- 内存块的新的大小，以字节为单位。如果大小为 0，且 ptr 指向一个已存在的内存块，则 ptr 所指向的内存块会被释放，并返回一个空指针。
    if((newsh = zrealloc(sh, sizeof(struct sdshdr)+newlen+1)) == NULL)
        return NULL;

    // 更新 sds 的空余长度
    newsh->free = newlen - len;

    // 返回 sds
    return newsh->buf;
}

/*
 * 根据sds字符串长度增加incr
 * 更新free和len的长度
 * 复杂度
 *  T = O(1)
 */
void sdsIncrLen(sds s, int incr) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));

    // 确保 sds 空间足够
    assert(sh->free >= incr);

    // 更新属性
    sh->len += incr;
    sh->free -= incr;

    // 放置新的结尾符号
    s[sh->len] = '\0';
}

/*
 * 按索引对截取 sds 字符串的其中一段
 * start 和 end 都是闭区间（包含在内）
 *
 * 索引从 0 开始，最大为 sdslen(s) - 1
 * 索引可以是负数， sdslen(s) - 1 == -1
 *
 * 复杂度
 *  T = O(N)
 */
void sdsrange(sds s, int start, int end) {
    struct sdshdr *sh = (void*) (s-(sizeof(struct sdshdr)));
    size_t newlen, len = sdslen(s);
    
    //如果给定的字符串长度为0，直接退出
    if (len == 0) return;

    //对start进行格式合理化
    if (start < 0) {
        start = len+start;
        if (start < 0) start = 0;
    }
    //对end进行格式合理化
    if (end < 0) {
        end = len+end;
        if (end < 0) end = 0;
    }
    //获取需要截取字符串的长度
    newlen = (start > end) ? 0 : (end-start)+1;
    
    if (newlen != 0) {
        if (start >= (signed)len) {
            newlen = 0;
        } else if (end >= (signed)len) {
            end = len-1;
            newlen = (start > end) ? 0 : (end-start)+1;
        }
    } else {
        start = 0;
    }

    // 如果有需要，对字符串进行移动
    // T = O(N)
    if (start && newlen) memmove(sh->buf, sh->buf+start, newlen);
    // 添加终结符
    sh->buf[newlen] = '\0';
    // 更新属性
    sh->free = sh->free+(sh->len-newlen);
    sh->len = newlen;
}


/*
 * 将长度为 len 的字符串 t 追加到 sds 的字符串末尾
 * 返回值
 *       追加成功返回新 sds ，失败返回 NULL
 *  T = O(N)
 */
sds sdscatlen(sds s, const void *t, size_t len) {
    
    struct sdshdr *sh;
    
    // 原有字符串长度
    size_t curlen = sdslen(s);
    // 扩展 sds 空间
    s = sdsMakeRoomFor(s,len);
    // 内存不足？直接返回
    if (s == NULL) return NULL;

    // 复制 t 中的内容到字符串后部
    sh = (void*) (s-(sizeof(struct sdshdr)));
    memcpy(s+curlen, t, len);

    // 更新属性
    sh->len = curlen+len;
    sh->free = sh->free-len;

    // 添加新结尾符号
    s[curlen+len] = '\0';

    // 返回新 sds
    return s;
}

/* 
 * 为了计算客户端的输出缓冲区大小，我们需要获取已分配对象的大小，
 * 但是我们不能直接在sds字符串上使用zmalloc_size()，
 * 因为它们使用的是一种技巧(头在返回的指针之前)，所以我们使用这个助手函数
 */
size_t zmalloc_size_sds(sds s) {
    return zmalloc_size(s-sizeof(struct sdshdr));
}

/*------------------------------------command---------------------------------------------------------------*/

/* SET key value [NX] [XX] [EX <seconds>] [PX <milliseconds>] */
void setGenericCommand(KVClient *c, robj *key, robj *val, robj *expire, int unit) {
    long long milliseconds = 0; /*初始化以避免任何危害警告*/
    // 取出过期时间（查看是否设置了过期时间）
    if (expire) {
        // 取出 expire 参数的值
        // 将过期时间expire转换成整数以后存入milliseconds
        if (getLongLongFromObject(expire, &milliseconds) != AE_OK)
            return;

        // expire 参数的值不正确时报错
        if (milliseconds <= 0) {
            //执行遇到错误，返回客户端一个错误；
            printf("invalid expire time\n");
            return;
        }
        // 不论输入的过期时间是秒还是毫秒,实际都以毫秒的形式保存过期时间
        // 如果输入的过期时间为秒UNIT_SECONDS，那么将它转换为毫秒
        if (unit == UNIT_SECONDS) milliseconds *= 1000;
    }
    // 将键值关联到数据库（添加不存在的键，或者更新已存在的键），并且将键的过期时间移除
    setKey(c->db,key,val);
    // 将数据库设为脏
    //服务器每次修改一个键之后，都会对脏键计数器+1，这个计数会触发服务器的持久化以及复制操作
    server.dirty++;

    // 为键设置过期时间
    if (expire) setExpire(c->db,key,mstime()+milliseconds);


    // 设置成功，向客户端发送回复
    addReply(c,shared.ok);
    printf("The key is succesfully seted\n");
}

/* SET key value*/
//void setGenericCommand(c, key, val, [ss/ms], [expire])
void setCommand(KVClient *c) {
    int j;
    robj *expire = NULL;
    int unit = UNIT_MILLISECONDS;
    // 设置选项参数
    for (j = 3; j < c->argc; j++) {
        char *a = c->argv[j]->ptr;
        robj *next = (j == c->argc-1) ? NULL : c->argv[j+1];

        if ((a[0] == 's' || a[0] == 'S') &&
            (a[1] == 's' || a[1] == 'S') && a[2] == '\0') {
            unit = UNIT_SECONDS;
            expire = next;
            j++;
        } else if ((a[0] == 'm' || a[0] == 'M') &&
                   (a[1] == 's' || a[1] == 'S') && a[2] == '\0') {
            unit = UNIT_MILLISECONDS;
            expire = next;
            j++;
        } else {
            printf( "-ERR syntax error\n");
            return;
        }
    }
    setGenericCommand(c, c->argv[1], c->argv[2], expire, unit);
}

/*
 * 将键 key 的过期时间设为 when
 */
void setExpire(KVdataDb *db, robj *key, long long when) 
{
    char buf[20];
    dictEntry *kde, *de;

    // 取出键对应的字典节点
    kde = dictFind(db->DB,key->ptr);

    //确定该节点不为空
    assert(kde != NULL);

    // 根据键取出键的过期时间
    // 如果在过期字典存在该键，则取出该键
    // 如果过期字典中不存在键，则在过期字典中添加一个该键
    if((de = dictFind(db->expires,key->ptr)) == NULL)
       de = dictAddRaw(db->expires,key->ptr);

    // 设置键的过期时间
    // 这里是直接使用整数值来保存过期时间，不是用 INT 编码的 String 对象
    de->expire = when;
    int len = ll2string(buf,sizeof(buf)-1,when);
    de->val = createStringObject(buf,len);
    printf("The key %s expire set successfully\n",(char*)key->ptr);
}

//尝试从数据库中取出键 c->argv[1] 对应的值对象
//如果键不存在时，向客户端发送回复信息，并返回KVDATA_OK
//如果存在则检查键的类型，不为字符串型则报错返回KVDATA_ERR，类型正确返回KVDATA_OK
int getGenericCommand(KVClient *c) {
    robj *o;

    // 尝试从数据库中取出键 c->argv[1] 对应的值对象
    // 如果键不存在时，向客户端发送回复信息，并返回 NULL
    if ((o = lookupKey(c->db,c->argv[1])) == NULL)
    {
        addReply(c,shared.nullbulk);
        return AE_OK;
    }

    printf("getKey  succeseful.\n");

    // 值对象存在，检查它的类型
    if ((o->encoding != STRING) && (o->encoding != INT)) {
        // 类型错误
        addReply(c,shared.wrongtypeerr);
        return AE_ERR;
    } else {
        // 类型正确，向客户端返回对象的值
        addReplyBulk(c,o);
        return AE_OK;
    }
}

//GET  key
//获取该key对应的value内容，如果已过期则不显示
void getCommand(KVClient *c) {
    getGenericCommand(c);
}

/*--------------------------------------------------*/
/*
 * s = sdscatprintf(sdsempty(), "... your format ...", args);
 * 打印任意数量个字符串，并将这些字符串追加到给定 sds 的末尾
 */
sds sdscatprintf(sds s, const char *fmt, ...) {
    va_list ap;
    char *t;
    //使参数列表指针ap指向函数参数列表中的第一个可选参数
    va_start(ap, fmt);

    t = sdscatvprintf(s,fmt,ap);

    //清空参数列表ap
    va_end(ap);
    return t;
}
/* 
 * 打印函数，被 sdscatprintf 所调用
 * 整体思想，先通过C内置函数vsnprintf(buf, buflen, fmt, cpy);将可变参数列表转换成字符串存入buf
 * 再将buf加在sds类型的s后面
 * T = O(N^2)
 */
sds sdscatvprintf(sds s, const char *fmt, va_list ap) {
    va_list cpy;
    char staticbuf[1024], *buf = staticbuf, *t;

    //先将buf的长度定义成格式化字符串（fmt是格式化字符串如："%s,%d,%d,%c"）的2倍
    size_t buflen = strlen(fmt)*2;

    //如果要打印的字符串类型长度大于定义好的staticbuf长度，则重新为buf分配内存
    if (buflen > sizeof(staticbuf)) {
        buf = zmalloc(buflen);
        if (buf == NULL) return NULL;
    } else {

    //如果buf的长度不大于staticbuf，那么buf还是指向staticbuf
    //buflen的值由原来的strlen(fmt)*2改为staticbuf的长度也就是1024字节。
        buflen = sizeof(staticbuf);
    }

    //进入循环，直到buf可以容纳ap参数为止
    while(1) {
        buf[buflen-2] = '\0';

        //将ap复制到cpy，不直接操作可变参数ap
        va_copy(cpy,ap);

        //将cpy内容以fmt所指格式输出到buf数组中，长度为buflen
        vsnprintf(buf, buflen, fmt, cpy);

        //结束可变参数ap的操作
        va_end(cpy);

        //因为上面已经将buf[buflen-2]赋值空字符，如果此时不是空字符，说明cpy在buf中溢出了
        if (buf[buflen-2] != '\0') {
            //【有意思！！！！！】
            // 如果buf == staticbuf，说明buf占用的是局部变量的内存，在栈上，不是malloc出来的内存，因此不需要释放，函数结束后自动释放
            //如果buf ！= staticbuf，说明buf占用的是malloc出来的内存，不在栈上，因此需要手动释放
            if (buf != staticbuf) zfree(buf);
            buflen *= 2;
            buf = zmalloc(buflen);
            if (buf == NULL) return NULL;
            continue;
        }
        break;
    }
    //最后，将获得的字符串连接到SDS字符串并返回它
    t = sdscatlen(s, (char*)buf, strlen(buf));
    if (buf != staticbuf) zfree(buf);//效果同上
    return t;
}
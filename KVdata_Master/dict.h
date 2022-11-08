#ifndef KVDATA_DICT_H
#define KVDATA_DICT_H
#include <stdint.h>
#include "sds.h"
// 强制 rehash 的比率
#define dict_force_resize_ratio  5
// 字典哈希表的初始大小
#define DICT_HT_INITIAL_SIZE   40
// 字典的操作状态，操作成功
#define DICT_OK 0
// 字典的操作状态，操作失败（或出错）
#define DICT_ERR 1

/* 哈希表节点 */
typedef struct dictEntry { 
    // 键
    void *key;
    // 键值对的值
    void *val;
    // 键的过期时间
    uint64_t expire;
    // 指向下个哈希表节点，形成链表，解决键冲突
    struct dictEntry *next;

} dictEntry;

/*
 * 哈希表
 * 每个字典都使用两个哈希表，从而实现渐进式 rehash 。
 */
typedef struct dictht {
    
    // 哈希表数组
    // table数组中的每个元素都是一个指向dictEntry结构的指针
    // 每个dictEntry结构保存着一个键值对。
    dictEntry **table;

    // 哈希表大小（table数组的大小）
    unsigned long size;
    
    // 哈希表大小掩码，用于计算索引值
    // 总是等于 size - 1
    unsigned long sizemask;

    // 该哈希表已有节点的数量
    unsigned long used;

} dictht;

/*
 * 字典类型特定函数
 */
typedef struct dictType {

    // 计算哈希值的函数
    unsigned int (*hashFunction)(const void *key);
    // 销毁值的函数
    void (*valDestructor)(void *privdata, void *obj);

} dictType;


/* 字典 */
typedef struct dict {
    // 哈希表
    dictht ht[2];

    // rehash 索引
    // 当 rehash 不在进行时，值为 -1
    int rehashidx;

    // 类型特定函数，每个字典结构保存了一簇用于操作特定类型键值对的函数
    // 监视键字典，过期键字典，数据库字典
    dictType *type;

} dict;

/*
 * 字典迭代器
 */
typedef struct dictIterator {  

    // 被迭代的字典
    dict *d;
    //正在被迭代的哈希表号码，值可以是 0 或 1 。
    int table;
    //迭代器当前所指向的哈希表索引位置。
    int index;
    //当前迭代到的节点的指针
    dictEntry *entry;
    //当前迭代节点的下一个节点
    dictEntry *nextEntry;
    
} dictIterator;


// 查看字典是否正在 rehash
#define dictIsRehashing(ht) ((ht)->rehashidx != -1)
// 返回获取给定节点的值
#define dictGetVal(he) ((he)->val)
// 返回字典的已有节点数量
#define dictSize(d) ((d)->ht[0].used+(d)->ht[1].used)
// 计算给定键的哈希值
#define dictHashKey(d, key) (d)->type->hashFunction(key)


dict *dictCreate(dictType *type);
int dictExpand(dict *d, unsigned long size);
int dictExpandIfNeeded(dict *d);
int dictRehash(dict *d, int n);
dictEntry *dictFind(dict *d, void *key);
struct KVDataCommand *lookupCommand(sds name);
int dictDelete(dict *d, const void *key);
int dictAdd(dict *d, void *key, void *val);
dictEntry *dictAddRaw(dict *d, void *key);
int dictKeyIndex(dict *d, const void *key);
int dictReplace(dict *d, void *key, void *val);

int dictSdsKeyCompare(const void *key1, const void *key2);

dictIterator *dictGetIterator(dict *d);
dictEntry *dictNext(dictIterator *iter);
void dictReleaseIterator(dictIterator *iter);
//需要反复研究！！！！！！！！！！！！！！！！！！

unsigned int dictSdsHash(const void *key);
unsigned int dictObjHash(const void *key);
unsigned int dictSdsCaseHash(const  void *key);
void dictListDestructor(void *privdata, void *val);
void dictKVDATAObjectDestructor(void *privdata, void *val);
unsigned int dictIntHashFunction(unsigned int key);
unsigned int dictGenHashFunction(const void *key, int len);
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len);
#endif
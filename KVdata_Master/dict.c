#include "dict.h"
#include "server.h"
#include "zmalloc.h"
#include <string.h>
#include <ctype.h>
/*
 * 创建一个新的字典
 */
dict *dictCreate(dictType *type)
{
    dict *d = zmalloc(sizeof(*d));
    // 初始化两个哈希表的各项属性值
    // 但暂时还不分配内存给哈希表数组
    for(int i=0;i<=1;i++)
    {
        //重置指定哈希表内的各项属性
        d->ht[i].table = NULL;   //哈希表数组
        d->ht[i].size = 0;       //哈希表大小（table数组的大小）
        d->ht[i].sizemask = 0;   //哈希表大小掩码
        d->ht[i].used = 0;       //该哈希表已有节点的数量
    }
    // 设置类型特定函数
    d->type = type;

    // 设置哈希表 rehash 状态
    d->rehashidx = -1;

    return d;
}

/*
 * 扩充一个新容量的哈希表，并根据字典的情况，选择以下其中一个动作来进行：
 *
 * 1) 如果字典的 0 号哈希表为空，那么将新哈希表设置为 0 号哈希表
 * 2) 如果字典的 0 号哈希表非空，那么将新哈希表设置为 1 号哈希表，
 *    并打开字典的 rehash 标识，使得程序可以开始对字典进行 rehash
 *
 * size 参数不够大，或者 rehash 已经在进行时，返回 DICT_ERR 。
 * 成功创建 0 号哈希表，或者 1 号哈希表时，返回 DICT_OK 。
 *
 * T = O(N)
 */
int dictExpand(dict *d, unsigned long size)
{
    // 新哈希表
    dictht n;

    // 不能在字典正在 rehash 时进行
    // size 的值也不能小于 0 号哈希表的当前已使用节点
    if (dictIsRehashing(d) || d->ht[0].used > size)
        return DICT_ERR;

    // 为哈希表分配空间，并将所有指针指向 NULL
    n.size = size;
    n.sizemask = size-1;
    // T = O(N)
    n.table = zmalloc(size*sizeof(dictEntry*));
    n.used = 0;

    // 如果 0 号哈希表为空，那么这是一次初始化：
    // 程序将新哈希表赋给 0 号哈希表的指针，然后字典就可以开始处理键值对了。
    if (d->ht[0].table == NULL) {
        d->ht[0] = n;
        return DICT_OK;
    }

    // 如果 0 号哈希表非空，那么这是一次 rehash ，对哈希表进行扩展，扩展到1号哈希表：
    // 程序将新哈希表设置为 1 号哈希表，
    // 并将字典的 rehash 标识打开，让程序可以开始对字典进行 rehash
    d->ht[1] = n;
    d->rehashidx = 0;
    return DICT_OK;
}

/*
 * 根据需要，初始化字典（的哈希表），
 * 或者对字典（的现有哈希表）进行扩展
 * T = O(N)
 */
int dictExpandIfNeeded(dict *d)
{
    // 渐进式 rehash 已经在进行了，直接返回，对于正在进行rehash的，则不能进行扩展或初始化
    if (dictIsRehashing(d)) return DICT_OK;

    // 如果字典（的 0 号哈希表）为空，那么创建并返回初始化大小的 0 号哈希表
    if (d->ht[0].size == 0) return dictExpand(d, DICT_HT_INITIAL_SIZE);

    // 以下两个条件之一为真时，对字典进行扩展
    // 1）字典已使用节点数和字典大小之间的比率接近 1：1
    // 2）已使用节点数和字典大小之间的比率超过 dict_force_resize_ratio
    if (d->ht[0].used >= d->ht[0].size || (d->ht[0].used*10/d->ht[0].size >= dict_force_resize_ratio))
    {
        // 新哈希表的大小至少是目前已使用节点数的两倍
        return dictExpand(d, d->ht[0].used*2);
    }

    return DICT_OK;
}

/*
 * 执行 N 步渐进式 rehash 。
 *
 * 返回 1 表示仍有键需要从 0 号哈希表移动到 1 号哈希表，
 * 返回 0 则表示所有键都已经迁移完毕。
 *
 * 注意，每步 rehash 都是以一个哈希表索引（桶）作为单位的，
 * 一个桶里可能会有多个节点，
 * 被 rehash 的桶里的所有节点都会被移动到新哈希表。
 *
 * T = O(N)
 */
int dictRehash(dict *d, int n) {
    // 只可以在 rehash 进行中时执行
    if (!dictIsRehashing(d)) return 0;

    // 进行 N 步迁移
    // T = O(N)
    while(n--) {
        dictEntry *de;

        // 如果 0 号哈希表为空，那么表示 rehash 执行完毕
        // T = O(1)
        if (d->ht[0].used == 0) {
            // 释放 0 号哈希表
            zfree(d->ht[0].table);
            // 将原来的 1 号哈希表设置为新的 0 号哈希表
            d->ht[0] = d->ht[1];   
            //重置指定哈希表内的各项属
            d->ht[1].table = NULL;   //哈希表数组
            d->ht[1].size = 0;       //哈希表大小（table数组的大小）
            d->ht[1].sizemask = 0;   //哈希表大小掩码
            d->ht[1].used = 0;       //该哈希表已有节点的数量
            // 关闭 rehash 标识
            d->rehashidx = -1;
            // 返回 0 ，向调用者表示 rehash 已经完成
            return 0;
        }

        // 略过数组中为空的索引，找到下一个非空索引
        while(d->ht[0].table[d->rehashidx] == NULL) 
        d->rehashidx++;

        // 指向该索引的链表表头节点
        de = d->ht[0].table[d->rehashidx];

        // 将一个桶中的所有节点迁移到新哈希表
        // T = O(1)
        while(de) {
            unsigned int h;
            // 计算新哈希表的哈希值，以及节点插入的索引位置
            h = dictHashKey(d, de->key) & d->ht[1].sizemask;

            // 插入节点到新哈希表
            //【键冲突时采用前插法，将本次的键next指针指向哈希表原来键的节点，再将本次节点替代原键节点位置】
            de->next = d->ht[1].table[h];
            d->ht[1].table[h] = de;

            // 更新计数器
            d->ht[0].used--;
            d->ht[1].used++;

            // 继续处理下个节点
            de = de->next;
        }
        // 将刚迁移完的哈希表索引的指针设为空
        d->ht[0].table[d->rehashidx] = NULL;
        // 更新 rehash 索引
        d->rehashidx++;
    }
    return 1;
}

/*
 * 返回字典中包含键 key 的节点
 * 找到返回节点，找不到返回 NULL
 *
 * T = O(1)
 */
dictEntry *dictFind(dict *d, void *key)
{
    dictEntry *he;
    unsigned int h, idx, table;
    // 字典（的哈希表）为空
    if (d->ht[0].size == 0) return NULL;
    // 如果正在进行rehash的话，进行单步 rehash
    if (dictIsRehashing(d)) dictRehash(d, 1);
    // 计算键的哈希值
    h = dictHashKey(d, key);
    // 在字典的哈希表中查找这个键
    for (table = 0; table <= 1; table++) {
        // 计算索引值
        idx = h & d->ht[table].sizemask;
        // 遍历给定索引上的链表的所有节点，查找 key
        he = d->ht[table].table[idx];    
        while(he) {
            //比较键是否相等
            if (dictSdsKeyCompare(key, he->key))//*(char*)key == *(char*)he->key
            {
                return he;
            }    
                
            he = he->next;
        }
        // 如果程序遍历完 0 号哈希表，仍然没找到指定的键的节点
        // 那么程序会检查字典是否在进行 rehash ，
        // 然后才决定是直接返回 NULL ，还是继续查找 1 号哈希表
        if (!dictIsRehashing(d)) return NULL;
    }
    // 进行到这里时，说明两个哈希表都没找到
    return NULL;
}

/*
 * 根据给定命令名字（SDS），查找命令
 */
struct KVDataCommand *lookupCommand(sds name) {
    dictEntry *dicNode;
    dicNode = dictFind(server.commands,name);
    return dicNode ? dictGetVal(dicNode) : NULL;
}


/*
 * 从字典中删除包含给定键的节点
 * 并且释放被删除的节点
 * 找到并成功删除返回 DICT_OK ，没找到则返回 DICT_ERR
 * T = O(1)
 */
int dictDelete(dict *d, const void *key)
{
    unsigned int h, idx;
    dictEntry *he, *prevHe;
    int table;
    
    // 字典（的哈希表）为空
    if (d->ht[0].size == 0) return DICT_ERR;
    //如果该字典正在Rehash，则使用单步Rehash
    if (dictIsRehashing(d)) dictRehash(d, 1);

    // 计算哈希值
    h = dictHashKey(d, key);
    // 遍历哈希表
    // T = O(1)
    for (table = 0; table <= 1; table++) {
        // 计算索引值 
        idx = h & d->ht[table].sizemask;
        // 指向该索引上桶的链表
        he = d->ht[table].table[idx];
        prevHe = NULL;
        // 遍历桶链表上的所有节点
        // T = O(1)
        while(he) {
            // 查找到目标节点
            if (dictSdsKeyCompare(key, he->key)) {
                // 从链表中删除，考虑了key是链表的头节点与不是头节点两种情况
                //总之就是将目标节点从原来的节点中摘除出来
                if (prevHe)
                    prevHe->next = he->next;
                else
                    d->ht[table].table[idx] = he->next;
                // 1.释放给定字典的键，和对应的值
                sdsfree(he->key);
                decrRefCount(he->val);
                // 2.释放节点本身
                zfree(he);
                // 更新已使用节点数量
                d->ht[table].used--;
                // 返回已找到信号
                return DICT_OK;
            }
            prevHe = he;
            he = he->next;
        }
        // 如果执行到这里，说明在 0 号哈希表中找不到给定键
        // 那么根据字典是否正在进行 rehash ，决定要不要查找 1 号哈希表
        if (!dictIsRehashing(d)) break;
    }

    // 没找到
    return DICT_ERR;
}


/*
 * 尝试将给定键值对添加到字典中
 * 只有给定键 key 不存在于字典时，添加操作才会成功
 * 添加成功返回 DICT_OK ，失败返回 DICT_ERR
 * 最坏 T = O(N) ，平均 O(1) 
 */
int dictAdd(dict *d, void *key, void *val)
{
    // 尝试添加键到字典，并返回包含了这个键的新哈希节点
    // T = O(N)
    dictEntry *entry = dictAddRaw(d,key);
    // 键已存在，添加失败
    if (!entry) return DICT_ERR;
    
    // 键不存在，对返回的节点设置节点的值
    entry->val = val;
    // 添加成功
    return DICT_OK;
}

/*
 * 尝试将键插入到字典中
 *
 * 如果键已经在字典存在，那么返回 NULL
 *
 * 如果键不存在，那么程序创建新的哈希节点，
 * 将节点和键关联，并插入到字典，然后返回节点本身。
 *
 * T = O(N)
 */
dictEntry *dictAddRaw(dict *d, void *key)
{
    int index;
    dictEntry *entry;
    dictht *ht;

    //如果该字典正在Rehash，则使用单步Rehash
    if (dictIsRehashing(d)) dictRehash(d, 1);

    // 计算键在哈希表中的索引值
    // dictKeyIndex函数返回可以插入键值的索引
    // 如果值为 -1 ，那么表示键已经存在
    if ((index =  dictKeyIndex(d, key)) == -1)
        return NULL;

    // 如果字典正在 rehash ，那么将新键添加到 1 号哈希表
    // 否则，将新键添加到 0 号哈希表
    ht = dictIsRehashing(d) ? &d->ht[1] : &d->ht[0];

    // 为新节点分配空间
    entry = zmalloc(sizeof(*entry));
    // 将新节点插入到桶链表表头
    entry->next = ht->table[index];
    ht->table[index] = entry;
    // 更新哈希表已使用节点数量
    ht->used++;
    // 设置新节点的键
    entry->key = key;

    return entry;
}

/*
 * 返回可以将 key 插入到哈希表的索引位置
 * 如果 key 已经存在于哈希表，那么返回 -1
 *
 * 注意，如果字典正在进行 rehash ，那么总是返回 1 号哈希表的索引。
 * 因为在字典进行 rehash 时，新节点总是插入到 1 号哈希表。
 *
 * T = O(N)
 */
int dictKeyIndex(dict *d, const void *key)
{
    unsigned int h, idx, table;
    dictEntry *he;
    //根据需要对字典进行初始化或扩展
    if (dictExpandIfNeeded(d) == DICT_ERR)
        return -1;

    // 计算 key 的哈希值
    h = dictHashKey(d, key);
    // T = O(1)
    for (table = 0; table <= 1; table++) {
        // 计算索引值
        idx = h & d->ht[table].sizemask;

        // 查找 key 是否存在
        he = d->ht[table].table[idx];
        while(he) {
            if (key == he->key)
                return -1;
            he = he->next;
        }
        // 如果运行到这里时，说明 0 号哈希表中所有节点都不包含 key
        // 如果这时 rehash 正在进行，那么继续对 1 号哈希表进行 rehash
        if (!dictIsRehashing(d)) break;
    }
    
    // 返回索引值
    return idx;
}

/*
 * 将给定的键值对添加到字典中，如果键已经存在，那么删除旧有的键值对。
 * 如果键值对为全新添加，那么返回 1 。
 * 如果键值对是通过对原有的键值对更新得来的，那么返回 0 。
 *
 * T = O(N)
 */
int dictReplace(dict *d, void *key, void *val)
{
    dictEntry *entry;
    void *auxentry;
    // 尝试直接将键值对添加到字典
    // 如果键 key 不存在的话，添加会成功
    // T = O(N)
    if (dictAdd(d, key, val) == DICT_OK)
        return 1;

    // 运行到这里，说明键 key 已经存在，那么找出包含这个 key 的节点
    entry = dictFind(d, key);
    // 先保存原有的值的指针
    auxentry = entry->val;
    // 然后设置新的值
    entry->val = val;
    // 然后释放旧值
    decrRefCount(auxentry);
    return 0;
}
/*
 * 比较字典两个键的是否相同
 */
int dictSdsKeyCompare(const void *key1, const void *key2)
{
    int l1,l2;
    //分别获取两个键的长度
    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    //比较两个键是否相同
    return memcmp(key1, key2, l1) == 0;
}


/*
 * 创建并返回给定字典的不安全迭代器
 * T = O(1)
 */
dictIterator *dictGetIterator(dict *d)
{
    //为迭代器分配空间
    dictIterator *iter = zmalloc(sizeof(*iter));
    //初始化迭代器的各项参数
    iter->d = d;
    iter->table = 0;
    iter->index = -1;
    iter->entry = NULL;
    iter->nextEntry = NULL;
    return iter;
}

/*
 * 返回迭代器当前指向节点的下一个节点
 * 字典迭代完毕时，返回 NULL
 * T = O(1)
 */
dictEntry *dictNext(dictIterator *iter)
{
    while (1) {

        // 进入这个循环有两种可能：
        // 1) 这是迭代器第一次运行
        // 2) 当前索引链表中的节点已经迭代完（NULL 为链表的表尾）
        if (iter->entry == NULL) {
            // 指向被迭代的哈希表
            dictht *ht = &iter->d->ht[iter->table];
            // 更新迭代器迭代的哈希表索引
            iter->index++;
            // 如果迭代器的当前索引大于当前被迭代的哈希表的大小
            // 那么说明这个哈希表已经迭代完毕
            if (iter->index >= (signed) ht->size) {
                // 如果正在 rehash 的话，那么说明 1 号哈希表也正在使用中
                // 那么继续对 1 号哈希表进行迭代
                if (dictIsRehashing(iter->d) && iter->table == 0) {
                    iter->table++;
                    iter->index = 0;
                    ht = &iter->d->ht[1];
                // 如果没有 rehash ，那么说明迭代已经完成
                } else {
                    break;
                }
            }
            // 如果进行到这里，说明这个哈希表并未迭代完
            // 更新节点指针，指向下个桶链表的表头节点
            iter->entry = ht->table[iter->index];
        } else {
            // 执行到这里，说明程序正在迭代某个链表
            // 将节点指针指向链表的下个节点
            iter->entry = iter->nextEntry;
        }
        // 如果当前节点不为空，那么也记录该节点的下个节点
        // 因为安全迭代器有可能会将迭代器返回的当前节点删除
        if (iter->entry) {
            iter->nextEntry = iter->entry->next;
            return iter->entry;
        }
    }
    // 迭代完毕
    return NULL;
}

/*
 * 释放给定字典迭代器
 * T = O(1)
 */
void dictReleaseIterator(dictIterator *iter)
{
    zfree(iter);
}









//需要反复研究！！！！！！！！！！！！！！！！！！

unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

unsigned int dictSdsCaseHash(const  void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, strlen((char*)key));
}

void dictListDestructor(void *privdata, void *val)
{
    KVDATA_NOTUSED(privdata);
    listRelease((list*)val);
}

void dictKVDATAObjectDestructor(void *privdata, void *val)
{
    KVDATA_NOTUSED(privdata);

    if (val == NULL) return; /* Values of swapped out keys as set to NULL */
    decrRefCount(val);
}
/* -------------------------- hash functions -------------------------------- */
static uint32_t dict_hash_function_seed = 5381;
/* 
 * 托马斯的32位混合哈希值计算方法
 * 
 * 返回计算键key对应的哈希值
 */
unsigned int dictIntHashFunction(unsigned int key)
{
    key += ~(key << 15);
    key ^=  (key >> 10);
    key +=  (key << 3);
    key ^=  (key >> 6);
    key += ~(key << 11);
    key ^=  (key >> 16);
    return key;
}

/* MurmurHash2, by Austin Appleby
 * Note - This code makes a few assumptions about how your machine behaves -
 * 1. We can read a 4-byte value from any address without crashing
 * 2. sizeof(int) == 4
 *
 * And it has a few limitations -
 *
 * 1. It will not work incrementally.
 * 2. It will not produce the same results on little-endian and big-endian
 *    machines.
 */
unsigned int dictGenHashFunction(const void *key, int len) {
    /* 'm' and 'r' are mixing constants generated offline.
     They're not really 'magic', they just happen to work well.  */
    uint32_t seed = dict_hash_function_seed;
    const uint32_t m = 0x5bd1e995;
    const int r = 24;

    /* Initialize the hash to a 'random' value */
    uint32_t h = seed ^ len;

    /* Mix 4 bytes at a time into the hash */
    const unsigned char *data = (const unsigned char *)key;

    while(len >= 4) {
        uint32_t k = *(uint32_t*)data;

        k *= m;
        k ^= k >> r;
        k *= m;

        h *= m;
        h ^= k;

        data += 4;
        len -= 4;
    }

    /* Handle the last few bytes of the input array  */
    switch(len) {
    case 3: h ^= data[2] << 16;
    case 2: h ^= data[1] << 8;
    case 1: h ^= data[0]; h *= m;
    };

    /* Do a few final mixes of the hash to ensure the last few
     * bytes are well-incorporated. */
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;

    return (unsigned int)h;
}

/* 字符串对应的哈希算法*/
unsigned int dictGenCaseHashFunction(const unsigned char *buf, int len) {
    unsigned int hash = (unsigned int)dict_hash_function_seed;//5381
    while (len--)
        hash = ((hash << 5) + hash) + (tolower(*buf++)); /* hash * 33 + c */
    return hash;
}







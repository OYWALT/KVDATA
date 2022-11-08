#include "db.h"
#include "object.h"
#include "list.h"
#include "client.h"
#include "events.h"
#include "assert.h"
#include "server.h"
#include "zmalloc.h"
/*
 * 将客户端的目标数据库切换为 id 所指定的数据库
 */
int selectDb(KVClient *c, int id) {

    // 确保 id 在正确范围内
    if (id < 0 || id >= server.dbnum)
        return AE_ERR;

    // 切换数据库（更新指针）
    c->db = &server.db[id];

    return AE_OK;
}

// 返回毫秒格式的 UNIX 时间
long long mstime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust/1000;
}

/*
 * 清空服务器的所有数据。
 */
long long emptyDb() {
    int j;
    long long removed = 0;

    // 清空所有数据库
    for (j = 0; j < server.dbnum; j++) {

        // 记录被删除键的数量
        removed += dictSize(server.db[j].DB);

        // 删除所有键值对
        dictEmpty(server.db[j].DB);
        // 删除所有键的过期时间
        dictEmpty(server.db[j].expires);
    }
    // 返回键的数量
    return removed;
}

/*
 * 清空字典上的所有哈希表节点，并重置字典属性
 * T = O(N)
 */
void dictEmpty(dict *d) {

    // 删除两个哈希表上的所有节点
    dictClear(d,&d->ht[0]);
    dictClear(d,&d->ht[1]);
    // 重置属性 
    d->rehashidx = -1;
}

/*
 * 删除哈希表上的所有节点，并重置哈希表的各项属性
 */
int dictClear(dict *d, dictht *ht) {
    // 遍历整个哈希表
    for (unsigned long i = 0; i < ht->size && ht->used > 0; i++) {
        dictEntry *he;

        // 跳过空索引
        if ((he = ht->table[i]) == NULL) 
        continue;

        // 遍历整个链表
        while(he) {
            // 删除键
            sdsfree(he->key);
            // 删除值
            decrRefCount(he->val);
            // 释放节点
            zfree(he);
            // 更新已使用节点计数
            ht->used--;
            // 处理下个节点
            he = he->next;
        }
    }
    // 释放哈希表结构
    zfree(ht->table);
    //重置指定哈希表内的各项属性
    ht->table = NULL;   //哈希表数组
    ht->size = 0;       //哈希表大小（table数组的大小）
    ht->sizemask = 0;   //哈希表大小掩码
    ht->used = 0;       //该哈希表已有节点的数量

    return DICT_OK; 
}
/*
 * 返回给定 key 的过期时间。
 * 如果键没有设置过期时间，那么返回 0。
 */
long long getExpire(KVdataDb *db, robj *key) {
    dictEntry *de;
    
    // 获取键的过期时间
    // 如果过期时间不存在，那么直接返回
    if (dictSize(db->expires) == 0 || (de = dictFind(db->expires,key->ptr)) == NULL) 
        return -1;
    //键key是在过期字典中查找到的的，意味着在主字典应该也能查找到
    assert(dictFind(db->DB,key->ptr) != NULL);
    // 返回过期时间
    return de->expire;
}

/*
 * 为执行写入操作而取出键 key 在数据库 db 中的值。
 * 找到时返回值对象，没找到返回 NULL 。
 */
robj *lookupKey(KVdataDb *db, robj *key) {
    // 检查 key 是否过期，过期时释放该键
    expireIfNeeded(db,key);

    // 查找并返回 key 的值对象
    // 查找键空间
    dictEntry *de = dictFind(db->DB,key->ptr);
    // 节点存在
    if (de) {    
        // 取出值
        robj *val = dictGetVal(de);
        return val;
    } else {
        // 节点不存在
        return NULL;
    }
}


/*
 * 检查 key 是否已经过期，如果是的话，将它从数据库中删除。
 * 返回 0 表示键没有过期时间，或者键未过期。
 * 返回 1 表示键已经因为过期而被删除了。
 */
void expireIfNeeded(KVdataDb *db, robj *key) {
    // 取出键的过期时间
    uint64_t when = getExpire(db,key);
    uint64_t now = mstime();
    printf("when to expire:%ld  ,   now:%ld\n",when,now);
    // 该键没有过期时间
    if (when <= 0) return;

    // 运行到这里，表示键带有过期时间，并且服务器为主节点
    // 如果未过期，返回 0
    if (now <= when) return;

    // 将过期键从数据库中删除
    dbDelete(db,key);
}

/*
 * 从数据库中删除给定的键，键的值，以及键的过期时间。
 * 删除成功返回 1 ，因为键不存在而导致删除失败时，返回 0 。
 */
int dbDelete(KVdataDb *db, robj *key) {

    // 删除键的过期时间
    if (dictSize(db->expires) > 0) 
    dictDelete(db->expires,key->ptr);

    // 删除键值对
    if (dictDelete(db->DB,key->ptr) == DICT_OK) {
        return 1;
    } else {
        // 键不存在
        return 0;
    }
}

/*
 * 尝试将键值对 key 和 val 添加到数据库中。
 * 调用者负责对 key 和 val 的引用计数进行增加。
 * 程序在键已经存在时会停止。
 */
void dbAdd(KVdataDb *db, robj *key, robj *val) {
    // 复制键名
    sds copy = sdsdup(key->ptr);
    // 尝试添加键值对
    int retval = dictAdd(db->DB, copy, val);
    // 确保键已经添加成功
    assert(retval == DICT_OK);

 }

/*
 * 为已存在的键关联一个新值。
 * 调用者负责对新值 val 的引用计数进行增加。
 * 这个函数不会修改键的过期时间。
 * 如果键不存在，那么函数停止。
 */
void dbOverwrite(KVdataDb *db, robj *key, robj *val) {
    dictEntry *de = dictFind(db->DB,key->ptr);
    // 节点必须存在，否则中止
    assert(de != NULL);
    // 覆写旧值
    dictReplace(db->DB, key->ptr, val);
}

/*
 * 移除键 key 的过期时间
 */
int removeExpire(KVdataDb *db, robj *key) {
    // 确保键带有过期时间
    assert(dictFind(db->DB,key->ptr) != NULL);

    // 删除过期时间，从数据库的过期字典中将键移除
    return dictDelete(db->expires,key->ptr) == DICT_OK;
}


/*
 * 高层次的 SET 操作函数。
 *
 * 这个函数可以在不管键 key 是否存在的情况下，将它和 val 关联起来。
 *
 * 1) 值对象的引用计数会被增加
 * 2) 监视键 key 的客户端会收到键已经被修改的通知
 * 3) 键的过期时间会被移除（键变为持久的）
 */
void setKey(KVdataDb *db, robj *key, robj *val) {
    //提示：指针变量必须初始化，没有初始化的指针属于野指针，不能用
    dictEntry * dicNode;
    list * watched;//监视键key的客户端链表
    listNode *cur;//链表节点
    KVClient *client;//监视键key的客户端
    // 添加或覆写数据库中的键值对
    if (lookupKey(db,key) == NULL) {
        dbAdd(db,key,val);
    } else {
        //为数据库中已存在的键值进行更新
        dbOverwrite(db,key,val);
    }
    //为对象引用计数+1
    incrRefCount(val);
    // 移除键的过期时间
    removeExpire(db,key);
    // 检查当前键是否在数据库被监视的键中，
    // 如果是，则将监视该键的客户端加上KVDATA_DIRTY_CAS标识
    if((dicNode=dictFind(db->watched_keys, key))!=NULL)
    {
        //根据监视的键，获取该键对应的监视客户端链表
         watched = dictGetVal(dicNode);
         cur = watched->head;
         //遍历该链表，为每个监视该键的客户端增加事务不安全标志
         while(cur != NULL)
         {
            client=listNodeValue(cur);
            if(!(client->flags & KVDATA_DIRTY_CAS))
            {
                client->flags |= KVDATA_DIRTY_CAS;
                printf("The key monitored by the client[%d] has been modified, add KVDATA_DIRTY_CAS flag\n",client->fd);
            }
            cur = cur->next;
         }
         
    }
}

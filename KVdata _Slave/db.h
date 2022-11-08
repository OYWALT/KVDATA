#ifndef KVDATA_DB_H
#define KVDATA_DB_H
#include "dict.h"
#include "object.h"
/* 
 * 数据库结构
 * 有多个数据库，由从0(默认数据库)到配置的最大数据库的整数标识。
 * 数据库编号是结构中的“id”字段。
 */
typedef struct KVdataDb {
    
    // 数据库号码
    int id;       

    // 数据库键空间，保存着数据库中的所有键值对（数据库键空间就是一个字典结构）
    dict *DB;               

    // 键的过期时间，字典的键为键，字典的值为过期事件 UNIX 时间戳
    dict *expires;           

    // 正在被 WATCH 命令监视的键. 字典的键为键，字典的值为监视该键的客户端链表
    dict *watched_keys;    

}KVdataDb;


long long mstime(void);
robj *lookupKey(KVdataDb *db, robj *key);
long long getExpire(KVdataDb *db, robj *key);
void expireIfNeeded(KVdataDb *db, robj *key);
int dbDelete(KVdataDb *db, robj *key);
void dbAdd(KVdataDb *db, robj *key, robj *val);
void dbOverwrite(KVdataDb *db, robj *key, robj *val);
int removeExpire(KVdataDb *db, robj *key);
void setKey(KVdataDb *db, robj *key, robj *val);


long long emptyDb();
void dictEmpty(dict *d);
int dictClear(dict *d, dictht *ht);
#endif
#ifndef KVDATA_MULTI_H
#define KVDATA_MULTI_H
#include "object.h"
#include "db.h"


/* 事务命令 */
typedef struct multiCmd {
    // 参数
    robj **argv;
    // 参数数量
    int argc;
    // 命令指针
    struct KVDataCommand *cmd;

} multiCmd;

/* 事务状态 */
typedef struct multiState {
    // 事务队列，FIFO 顺序，先进先出
    multiCmd *commands;  
    // 已入队命令计数
    int count;         

} multiState;

/* 
 * 在监视一个键时，
 * 我们既需要保存被监视的键，
 * 还需要保存该键所在的数据库。
 */
typedef struct watchedKey {

    // 被监视的键
    robj *key;

    // 键所在的数据库
    KVdataDb *db;

} watchedKey;



#endif
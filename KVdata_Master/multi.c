#include "multi.h"
#include "client.h"
#include <string.h>
#include "zmalloc.h"
#include "server.h"
#include "assert.h"
extern struct sharedObjectsStruct shared;
/* 
 * 初始化客户端的事务状态
 */
void initClientMultiState(KVClient *c) {

    // 命令队列
    c->mstate.commands = NULL;

    // 命令计数
    c->mstate.count = 0;
}

void execCommand(KVClient *c)
{
    // 客户端没有可执行的事务
    if (!(c->flags & KVDATA_MULTI)) {
        printf("EXEC without MULTI\n");
        return;
    }
    /* 检查是否需要阻止事务执行，因为：
     * 1) 有被监视的键已经被修改了
     * 2) 命令在入队时发生错误,命令格式出问题
     */
    if (c->flags & (KVDATA_DIRTY_CAS|KVDATA_DIRTY_EXEC)) {
        addReply(c, c->flags & KVDATA_DIRTY_EXEC ? shared.execaborterr :shared.nullbulk);
        // 取消事务
        discardTransaction(c);
        return;
    }
    // 已经可以保证安全性了，取消客户端对所有键的监视
    unwatchAllKeysCommand(c); 
    addReplyLongLongWithPrefix(c,c->mstate.count,'*');
    // 执行事务中的命令
    for (int j = 0; j < c->mstate.count; j++) {

        // 因为命令必须在客户端的上下文中执行
        // 所以要将事务队列中的命令、命令参数等设置给客户端
        c->argc = c->mstate.commands[j].argc;
        c->argv = c->mstate.commands[j].argv;
        c->cmd = c->mstate.commands[j].cmd;
        // 执行命令
        call(c,0);
    }
    // 清理事务状态
    discardTransaction(c);
}

void discardCommand(KVClient *c)
{

    // 不能在客户端未进行事务状态之前使用
    if (!(c->flags & KVDATA_MULTI)) {
        printf("DISCARD without MULTI\n");
        return;
    }
    discardTransaction(c);

    addReply(c,shared.ok);    
}

/*
 * 开启一个事务
 */
void multiCommand(KVClient *c) {

    // 不能在事务中嵌套事务
    if (c->flags & KVDATA_MULTI) {
        printf("MULTI calls can not be nested\n");
        return;
    }
    // 打开事务 FLAG
    c->flags |= KVDATA_MULTI;
    addReply(c,shared.ok);
}

/* 
 * 让客户端 c 监视给定的键 key
 */
void watchForKey(KVClient *c, robj *key) {

    list *clients = NULL;
    listNode *ln = c->watched_keys->head;
    watchedKey *wk;

    // 检查 key 是否已经保存在当前客户端 watched_keys 链表中，
    // 如果是的话，直接返回
    while((ln != NULL)) {
        wk = listNodeValue(ln);
        if (wk->db == c->db && dictSdsKeyCompare(key->ptr,wk->key->ptr))
            return;
    }
    // 检查 key 是否存在于数据库的 watched_keys 字典中
    dictEntry *clientNode = dictFind(c->db->watched_keys,key);
    clients = clientNode->val;

    // 如果不存在的话，创建一个客户端列表
    // 将键：被监视的键   值：客户端列表  组成的键值对加入到客户端当前使用数据库被监视键字典中
    // 方便其它客户端检查是否被修改的被监视
    if (!clients) { 
        // 值为链表
        clients = listCreate();
        // 关联键值对到字典
        dictAdd(c->db->watched_keys,key,clients);
        incrRefCount(key);
    }
    // 将客户端添加到链表的末尾
    listAddNodeTail(clients,c);

    //将被监视的键加入到当前客户端监视列表中，方便当前客户端进行事务操作检查安全性
    wk = zmalloc(sizeof(*wk));
    wk->key = key;
    wk->db = c->db;
    incrRefCount(key);
    listAddNodeTail(c->watched_keys,wk);
}


void watchCommand(KVClient *c)
{
    // 不能在事务开始后执行
    if (c->flags & KVDATA_MULTI) {
        printf("WATCH inside MULTI is not allowed\n");
        return;
    }
    // 监视输入的任意个键
    for (int j = 1; j < c->argc; j++)
        watchForKey(c,c->argv[j]);
    addReply(c,shared.ok);
}

/*
 * 取消当前事务
 */
void discardTransaction(KVClient *c) {

    // 重置事务状态
    freeClientMultiState(c);
    initClientMultiState(c);
    // 屏蔽事务状态
    c->flags &= ~(KVDATA_MULTI|KVDATA_DIRTY_CAS|KVDATA_DIRTY_EXEC);
    // 取消对所有键的监视
    unwatchAllKeysCommand(c);
}

/* 
 * 释放所有事务状态相关的资源
 */
void freeClientMultiState(KVClient *c) {
    int j;

    // 遍历事务队列
    for (j = 0; j < c->mstate.count; j++) {
        int i;
        multiCmd *mc = c->mstate.commands+j;

        // 释放所有命令参数
        for (i = 0; i < mc->argc; i++)
            decrRefCount(mc->argv[i]);

        // 释放参数数组本身
        zfree(mc->argv);
    }
    // 释放事务队列
    zfree(c->mstate.commands);
}

void queueMultiCommand(KVClient *c)
{
    multiCmd *mc;
    int j;
    
    // 为新数组元素分配空间
    c->mstate.commands = zrealloc(c->mstate.commands,
            sizeof(multiCmd)*(c->mstate.count+1));

    // 指向新元素
    mc = c->mstate.commands+c->mstate.count;

    // 设置事务的命令、命令参数数量，以及命令的参数
    mc->cmd = c->cmd;
    mc->argc = c->argc;
    mc->argv = zmalloc(sizeof(robj*)*c->argc);
    
    //将客户端的参数表赋值给事务对应命令的参数表
    memcpy(mc->argv,c->argv,sizeof(robj*)*c->argc);
    
    //各个参数对象引用计数+1
    for (j = 0; j < c->argc; j++)
        incrRefCount(mc->argv[j]);

    // 事务命令数量计数器+1
    c->mstate.count++;
}

/*
 * 取消客户端对所有键的监视。
 *
 * 清除客户端事务状态的任务由调用者执行。
 */
void unwatchAllKeysCommand(KVClient *c) {
    // 没有键被监视，直接返回
    if (listLength(c->watched_keys) == 0) return;
    //获取被监视键链表的头节点
    listNode *ln = c->watched_keys->head;
    while(ln != NULL) {
        list *clients;
        watchedKey *wk;
        // 从数据库的 watched_keys 字典的 key 键中
        // 删除链表里包含的客户端节点
        wk = listNodeValue(ln);
        // 检查 key 是否存在于数据库的 watched_keys 字典中,并取出客户端链表
        dictEntry *clientNode = dictFind(c->db->watched_keys,wk->key);
        clients = clientNode->val;
        assert(clients != NULL);

        // 删除链表中的客户端节点
        listDelNode(clients,listSearchKey(clients,c));
        // 如果链表已经被清空，那么删除这个键
        if (listLength(clients) == 0)
            dictDelete(wk->db->watched_keys, wk->key);
        // 从客户端监视链表中移除 key 节点
        listDelNode(c->watched_keys,ln);
        decrRefCount(wk->key);
        zfree(wk);
        //继续处理监视链表中的下一个键
        ln = ln->next;
    }
}

/*
 * 将事务状态设为 DIRTY_EXEC ，让之后的 EXEC 命令失败。
 * 每次在入队命令出错时调用，标志表示事务的安全性已经被破坏。
 * 打开，EXEC命令必然会执行失败。
 */
void flagTransaction(KVClient *c) {
    // 如果客户端正在执行事务
    if (c->flags & KVDATA_MULTI)
        // 表示事务在命令入队时出现了错，标志表示事务的安全性已经被破坏。打开，EXEC命令必然会执行失败。
        c->flags |= KVDATA_DIRTY_EXEC;
}

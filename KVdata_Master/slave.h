#ifndef KVDATA_SLAVE_H
#define KVDATA_SLAVE_H
#include "events.h"

/* 复制的状态（服务器是从服务器时使用）*/
#define KVDATA_REPL_NONE 0          //不是任何服务器的从节点
#define KVDATA_REPL_CONNECT 1       //发起主从复制状态
#define KVDATA_REPL_CONNECTING 2    //向主节点建链
#define KVDATA_REPL_RECEIVE_PONG 3  //等待主服务器的PONG回复
#define KVDATA_REPL_TRANSFER 4      //开始接收主服务器的RDB文件
#define KVDATA_REPL_CONNECTED 5     //接收主服务器RDB文件结束，与主服务器保持连通状态
#define KVDATA_REPL_SEND_PSYNC 6    //向主服务器发起部分同步

#define KVDATA_REPL_WAIT_BGSAVE_START 6 //开始生成RDB文件
#define KVDATA_REPL_WAIT_BGSAVE_END 7   //等待RDB文件生成结束

#define KVDATA_REPL_ONLINE 8   //RDB文件接收完毕，现在就是正常执行主服务器发来的命令
#define KVDATA_REPL_SEND_BULK 9 //向从节点发送RDB文件

/* 从节点向主节点发起部分重同步，主节点的回复信息*/
#define PSYNC_CONTINUE 0    //执行部分重同步
#define PSYNC_FULLRESYNC 1  //执行全量重同步
#define PSYNC_ERR 1  //执行全量重同步

void slaveofMyself(void);
void freeReplicationBacklog(void);
void replicationDiscardCachedMaster(void);
int cancelReplicationHandshake(void);
void replicationSetMaster(char *ip, int port);
void disconnectSlaves(void);
int connectWithMaster(void);
void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask);
void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask);
int slaveTryPartialResynchronization(int fd);
void replicationResurrectCachedMaster(int newfd);
char *sendSynchronousCommand(int fd, char* cmd);


void createReplicationBacklog(void);
void replicationCron(void);
void updateSlavesWaitingBgsave(int bgsaveerr);
void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask);
#endif
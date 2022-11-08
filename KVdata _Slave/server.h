#ifndef KVDATA_SERVER_H
#define KVDATA_SERVER_H

#include "events.h"
#include "list.h"
#include "client.h"

#define KVDATA_MAX_WRITE_PER_EVENT (1024*64)  //单次可回复客户端的最大长度
#define DB_NUM 1  //数据库数量
/* 无用参数避免警告 */
#define KVDATA_NOTUSED(V) ((void) V)

typedef struct KVServer{

// 本服务器的 RUN ID
char serverid[KVDATA_RUN_ID_SIZE+1];  
//服务器监听文件描述符
int listenfd;
// TCP 监听端口
int port;
//事件处理器状态
aeEventLoop *eventsLoop;
//我们使用全局状态下的unix时间的缓存值，需要准确性。访问全局变量要比调用时间(NULL)快得多
time_t unixtime;      
// 一个链表，保存了所有客户端状态结构
list *clients;          
// 服务器时间事件每秒调用的次数
int hz;   
// 是否开启 SO_KEEPALIVE 选项
int tcpkeepalive; 
// 命令表,字典的键为命令的名字，字典的值为{命令名字，函数指针，参数数量}的结构
dict *commands;     
//服务器当前数据库的数量
int dbnum;
//数据库数组
KVdataDb *db;

/*--------------------------------数据库持久化相关--------------------------------*/
//脏键，自从上次 SAVE 执行以来，数据库被修改的次数，用于数据库持久化
long long dirty; 
// 最近一次完成 SAVE 的时间
time_t lastsave;
//RDB文件名
char *rdb_filename;
// 这个值为真时，表示服务器正在进行载入
int loading;            
// 开始进行载入的时间
time_t loading_start_time;
//是否使用RDB校验和标志
int rdb_checksum;
/*--------------------------------主从复制相关--------------------------------*/
// 主服务器的ip地址
char *masterhost;    
// 主服务器的端口
int masterport;     
// 主服务器所对应的客户端
KVClient *master;    
// 被备份的主服务器，与主服务器重连时使用
KVClient *cached_master; 
// 保存了所有从服务器的链表（列表节点为 struct KVClient）
list *slaves;    
// 全局复制偏移量（一个累计值）
long long master_reploff; 
// 部分同步的复制积压 backlog 本身
char *repl_backlog; 
// 复制的状态（服务器是从服务器时使用）
int repl_state;    
// 从节点与主服务器建立连接的套接字
int repl_transfer_s;   
// 保存 RDB 文件的临时文件的描述符
int repl_transfer_fd;   
// 保存 RDB 文件的临时文件名字
char *repl_transfer_tmpfile; 
// 最近一次主从节点交互的时间
time_t repl_transfer_lastio;
// 已读 RDB 文件内容的字节数
off_t repl_transfer_read;
// 最近一次同步到磁盘的偏移量
off_t repl_transfer_last_fsync_off;
// RDB 文件的大小
off_t repl_transfer_size; 
// 初始化偏移量即在进行全量复制时，此时从节点的与主节点最新同步位置，
// 就是当前主服务器的更新迭代repl_master_initial_offset位置
long long repl_master_initial_offset; 
// 本服务器（从服务器）当前主服务器的 RUN ID
// 在本服务器与主服务器断链时，需要将该RUN ID保存到备份主服务器中，方便下次连接识别
char repl_master_runid[KVDATA_RUN_ID_SIZE+1]; 
// 复制挤压缓冲区backlog 的长度
long long repl_backlog_size;  
// backlog 中数据的长度
long long repl_backlog_histlen; 
// backlog 的当前索引
long long repl_backlog_idx;    
// backlog 中可以被还原的第一个字节的偏移量
long long repl_backlog_off;   
// 主从复制时当前正在使用的数据库
int slaveseldb;
// 负责执行 BGSAVE 的子进程的 ID，没在执行 BGSAVE 时，设为 -1
int rdb_child_pid;      
} KVServer;

extern KVServer server;//全局服务器变量

typedef void KVDataCommandProc(KVClient *c);
struct KVDataCommand {
    // 命令名字
    sds name;
    // 命令执行函数
    KVDataCommandProc *proc;
    // 参数个数
    int counts;
    // 命令的长度
    int len;
};
//服务器处理函数
void initServer(KVServer *server);
void updateCachedTime(void);
int serverCron(struct aeEventLoop *eventLoop, void *clientData);
void setProtocolError(KVClient *c, int pos);
void processInputBuffer(KVClient *c);
int processMultibulkBuffer(KVClient *c);
int processCommand(KVClient *c);
void call(KVClient *c, int flags);

//回复客户端处理函数
int prepareClientToWrite(KVClient *c);
void addReply(KVClient *c, robj *obj);
void addReplySds(KVClient *c, sds s);
int addReplyToBuffer(KVClient *c, char *s, size_t len);
void addReplyObjectToList(KVClient *c, robj *o);
void addReplyBulkLen(KVClient *c, robj *obj);
void addReplyLongLongWithPrefix(KVClient *c, long long ll, char prefix);
void addReplyBulk(KVClient *c, robj *obj);
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask);

//SET/GET命令
void setGenericCommand(KVClient *c, robj *key, robj *val, robj *expire, int unit);
void setCommand(KVClient *c);
void setExpire(KVdataDb *db, robj *key, long long when);
int getGenericCommand(KVClient *c);
void getCommand(KVClient *c);

//事务处理函数
void initClientMultiState(KVClient *c);
void execCommand(KVClient *c);
void discardCommand(KVClient *c);
void multiCommand(KVClient *c);
void watchForKey(KVClient *c, robj *key);
void watchCommand(KVClient *c);
void discardTransaction(KVClient *c);
void freeClientMultiState(KVClient *c);
void queueMultiCommand(KVClient *c);
void unwatchAllKeysCommand(KVClient *c);
void flagTransaction(KVClient *c);

//持久化处理函数
void saveCommand(KVClient *c);
void loadCommand(KVClient *c);

//主从复制处理函数
void slaveofCommand(KVClient *c);
void replicationCacheMaster(KVClient *c);
void pingCommand(KVClient *c);
void replconfCommand(KVClient *c);
void copyClientOutputBuffer(KVClient *dst, KVClient *src);
void syncCommand(KVClient *c);
int masterTryPartialResynchronization(KVClient *c);
long long addReplyReplicationBacklog(KVClient *c, long long offset);
void replCommand(KVClient *c);
#endif
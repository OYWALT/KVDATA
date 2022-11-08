#ifndef KVDATA_CLIENT_H
#define KVDATA_CLIENT_H
#include <sys/time.h>
#include "object.h"
#include "sds.h"
#include "db.h"
#include "list.h"
#include "multi.h"
#define KVDATA_REPLY_CHUNK_BYTES (16*1024)//回复缓冲块的大小限制
#define KVDATA_RUN_ID_SIZE 40  //服务器的运行id字符串长度
/* 客户端状态标志 */
#define KVDATA_SLAVE (1<<0)   /* 客户端是从服务器状态 */
#define KVDATA_MASTER (1<<1)  /* 客户端是主服务器状态 */
// #define KVDATA_MONITOR (1<<2) /* This client is a slave monitor, see MONITOR */
#define KVDATA_MULTI (1<<3)   /* 客户端是开启事务状态 */
// #define KVDATA_BLOCKED (1<<4) /* The client is waiting in a blocking operation */
#define KVDATA_DIRTY_CAS (1<<5) /* 客户端监视的键被修改了. */
#define KVDATA_CLOSE_AFTER_REPLY (1<<6) /* 客户端请求命令协议格式出错，异步关闭客户端的标志*/
// #define KVDATA_UNBLOCKED (1<<7) /* This client was unblocked and is stored in*/
// #define KVDATA_LUA_CLIENT (1<<8) /* This is a non connected client used by Lua */
// #define KVDATA_ASKING (1<<9)     /* Client issued the ASKING command */
// #define KVDATA_CLOSE_ASAP (1<<10)/* Close this client ASAP */
// #define KVDATA_UNIX_SOCKET (1<<11) /* Client connected via Unix domain socket */
#define KVDATA_DIRTY_EXEC (1<<12)  /* 表示事务在命令入队时出现了错，标志表示事务的安全性已经被破坏 */
#define KVDATA_MASTER_FORCE_REPLY (1<<13)  /* 从服务器需要向主服务器发送REPLICATION ACK命令 */
// #define KVDATA_FORCE_AOF (1<<14)   /* Force AOF propagation of current cmd. */
// #define KVDATA_FORCE_REPL (1<<15)  /* Force replication of current cmd. */
// #define KVDATA_PRE_PSYNC (1<<16)   /* Instance don't understand PSYNC. */
// #define KVDATA_READONLY (1<<17)    /* Cluster client is in read-only state. */

/*
 * 因为多路 I/O 复用的缘故，需要为每个客户端维持一个状态。
 *
 * 多个客户端状态被服务器用链表连接起来。
 */
typedef struct KVClient {
    
    // 客户端的名字
    robj *name;       
    
    // 套接字描述符
    int fd;

    //客户端的端口号
    int port;

    //客户端ip地址
    char*ip;

    // 当前正在使用的数据库
    KVdataDb *db;

    // 命令参数数量
    int argc;

    // 命令参数对象数组
    robj **argv;

    // 当前命令的参数个数————仅在解析命令请求时使用
    int multibulklen;   

    // 命令内容的长度【各参数的长度】————仅在解析命令请求时使用
    long bulklen;  

    //当前命令名字的长度，如SET：3
    long execlen;

    // 回复链表，链表实现可变大小缓冲区，用于保存那些长度比较大的回复
    list *reply;

    // 回复链表中对象的总大小
    unsigned long reply_bytes; 
    
    // 回复缓冲区，固定大小输出缓冲区默认16KB
    char buf[KVDATA_REPLY_CHUNK_BYTES];

    // 回复偏移量，记录了 buf 数组目前已使用的字节数量
    int bufpos;

    // 已发送字节，处理短写用
    int sentlen; 

    // 查询缓冲区，缓冲区用于保存客户端发送的命令请求（协议格式）
    sds querybuf;

    // 记录被客户端执行的命令 （当前执行的命令和最近一次执行的命令）
    struct KVDataCommand *cmd, *lastcmd;

    // 客户端状态标志，记录了客户端的角色（role）， 以及客户端目前所处的状态，可由多个二进制表示。
    int flags;             /* KVDATA_SLAVE | KVDATA_MONITOR | KVDATA_MULTI ... */

    // 客户端与服务器最近一次进行互动的时间
    time_t lastinteraction;

    // 创建客户端的时间
    time_t ctime;         

    // 空转时间，最后一次与服务器互动以来
    time_t idle_time;
    /*------------------------------------------------事务------------------------------------------------*/
    // 事务状态
    multiState mstate;  
    // 被监视的键
    list *watched_keys;     


    /*------------------------------------------------主从复制------------------------------------------------*/
    // 从节点记录的复制偏移量，每次收到主节点发来的命令时，就会将命令长度增加到该复制偏移量上
    long long reploff;    
    // 用于保存主服务器传来的 RDB 文件的文件描述符
    int repldbfd;   
    // 表示需要发送给从节点客户端的RDB文件的长度信息；
    sds replpreamble;  
    // 从节点记录的主节点运行ID,主要是PSYNC使用，
    //判断复制的服务器是否是原来的主服务器
    char replrunid[KVDATA_RUN_ID_SIZE+1];
    // 从服务器作为客户端最近一次发送 REPLCONF ACK 时的偏移量
    // 记录在主服务器对应从节点的客户端中
    long long repl_ack_off; 
    // 当该客户端为从服务器的复制状态
    int replstate;    

    // 主节点向该客户端对应从节点发送的 RDB 文件的偏移量
    off_t repldboff;     

    // 主节点向该客户端对应从节点发送的 RDB 文件的大小
    off_t repldbsize;    

}KVClient;

int selectDb(KVClient *c, int id);

KVClient *createClient(int fd);
void freeClient(KVClient *c);
void freeClientArgv(KVClient *c);
void resetClient(KVClient *c);
#endif
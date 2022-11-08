#include "slave.h"
#include "client.h"
#include "server.h"
#include "list.h"
#include <errno.h>
#include <fcntl.h>
#include "rdb.h"
#include <string.h>
#include "assert.h"
#include <unistd.h>
#include "eventEpoll.h"
#include "zmalloc.h"
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/socket.h>
extern KVServer server;//全局服务器变量
extern struct sharedObjectsStruct shared;

/*---------------------------------------------------从服务器---------------------------------------------------*/
/*
 * SLAVEOF ip port
 * 其中ip是点分十进制的地址   port是十进制的端口号
 */
void slaveofCommand(KVClient *c) {

    // SLAVEOF NO ONE 让从服务器转为主服务器
    // strcasecmp判断字符串是否相等的函数,忽略大小写
    if (!strcasecmp(c->argv[1]->ptr,"no") &&
        !strcasecmp(c->argv[2]->ptr,"one")) {
        if (server.masterhost) {
            // 让服务器取消复制，成为主服务器
            slaveofMyself();
            printf("MASTER MODE enabled (user request).\n");
        }
    } else {
        long long port;
        // 获取端口参数
        if ((getLongLongFromObject(c->argv[2], &port) != AE_OK))
            return;
        // 检查输入的 host 和 port 是否服务器目前的主服务器
        // 如果是的话，向客户端返回 +OK ，不做其他动作
        if (server.masterhost && !strcasecmp(server.masterhost,c->argv[1]->ptr)
            && server.masterport == port) {
            printf("SLAVE OF would result into synchronization with the master we are already connected with. No operation performed.\n");
            addReplySds(c,sdsnew("+OK Already connected to specified master\r\n"));
            return;
        }
        // 没有前任主服务器，或者客户端指定了新的主服务器
        // 开始执行复制操作
        replicationSetMaster(c->argv[1]->ptr, port);
        printf("SLAVE OF %s:%d enabled (user request)\n", server.masterhost, server.masterport);
    }
    addReply(c,shared.ok);
    printf("slaveof  succeseful.\n");
}

/*
 * 取消复制，将服务器设置为主服务器
 */ 
void slaveofMyself(void) {

    if (server.masterhost == NULL) return;
    //释放其复制的原主节点端口号
    sdsfree(server.masterhost);
    //避免野指针
    server.masterhost = NULL;

    if (server.master) {
        if (listLength(server.slaves) == 0) {
            // 如果这个从节点被转换为主节点并且没有从节点，那么它将从主节点继承复制偏移量。
            server.master_reploff = server.master->reploff;
            //释放复制积压缓冲区
            freeReplicationBacklog();
        }
        //释放其原主节点对应的客户端
        freeClient(server.master);
    }
    //释放其备份主节点
    replicationDiscardCachedMaster();
    cancelReplicationHandshake();
    server.repl_state = KVDATA_REPL_NONE;
}

/*
 * 释放复制积压缓冲区 backlog
 */ 
void freeReplicationBacklog(void) {
    //确认主服务器中从节点链表节点个数为0才能释放
    assert(listLength(server.slaves) == 0);
    //释放复制积压缓冲区
    zfree(server.repl_backlog);
    server.repl_backlog = NULL;
}

/*
 * 清空原 master 的备份，在主服务器发生替换时执行
 */
void replicationDiscardCachedMaster(void) {

    if (server.cached_master == NULL) return;
    printf("Discarding previously cached master state.\n");
    freeClient(server.cached_master);
    server.cached_master = NULL;
}

/* 
 * 如果有正在进行的主从复制行为，那么取消它。
 *
 * 如果复制在握手阶段被取消，那么返回 1 ，
 * 并且 server.repl_state 被设置为 KVDATA_REPL_CONNECT 。
 *
 * 如果从服务器正在下载RDB文件，停止下载，返回1 
 * 并且 server.repl_state 被设置为 KVDATA_REPL_CONNECT 。
 * 
 * 否则返回 0 ，并且不执行任何操作。
 */
int cancelReplicationHandshake(void) {

    //删除对主服务器的监听读事件
    aeDeleteFileEvent(server.eventsLoop,server.repl_transfer_s,AE_READABLE);
    //关闭与主服务器的连接
    close(server.repl_transfer_s);
    //与主服务连接的套接字repl_transfer_s恢复默认设置
    server.repl_transfer_s = -1;
    /*-----------------在接收主服务器发送来的RDB文件时取消主从复制------------------*/
    if (server.repl_state == KVDATA_REPL_TRANSFER) {
        //关闭RDB临时文件描述符
        close(server.repl_transfer_fd);
        //关闭从服务器用于保存RDB的临时文件
        unlink(server.repl_transfer_tmpfile);
        //释放保存 RDB 文件的临时文件名字
        zfree(server.repl_transfer_tmpfile);

    } else {
        return 0;
    }
    //将从服务器的复制状态变为SLAVEOF后的第一个待连接状态
    server.repl_state = KVDATA_REPL_CONNECT;
    printf("Back to KVDATA_REPL_CONNECT 1.\n");
    return 1;
}

/*
 * 将服务器设为指定地址的从服务器
 * 将服务器的复制状态server.repl_state设置为KVDATA_REPL_CONNECT
 */ 
void replicationSetMaster(char *ip, int port) {
    // 清除原有的主服务器地址（如果有的话）
    sdsfree(server.masterhost);
    // 设置主服务器IP
    server.masterhost = sdsnew(ip);
    // 设置主服务器端口
    server.masterport = port;
    // 清除原来可能有的主服务器信息... ...
    // 如果之前有其他地址，那么释放它
    if (server.master) freeClient(server.master);
    // 断开所有从服务器的连接，强制所有从服务器执行重同步
    disconnectSlaves(); 
    // 清空原 master 的备份
    replicationDiscardCachedMaster();  
    // 释放 backlog (如果存在的话)
    freeReplicationBacklog();  
    
    // 取消之前的主从复制行为（如果有的话）
    cancelReplicationHandshake();
    // 进入连接状态（重点）
    server.repl_state = KVDATA_REPL_CONNECT;
    printf("slave repl_state is KVDATA_REPL_CONNECT now.\n");
    server.master_reploff = 0;
}

/*
 * 断开所有从服务器的连接，强制所有从服务器执行重同步
 */ 
void disconnectSlaves(void) {
    while (listLength(server.slaves)) {
        listNode *ln = listFirst(server.slaves);
        freeClient((KVClient*)ln->value);
    }
}

/*
 * 当从节点收到客户端用户发来的”SLAVEOF” 命令时，或者在读取配置文件，发现了”slaveof”配置选项，
 * 就会将server.repl_state置为KVDATA_REPL_CONNECT状态。该状态表示下一步需要向主节点发起TCP建链。
 *
 * 在定时执行的函数serverCron中，会调用replicationCron函数检查主从复制的状态。
 * 一旦发现当前的server.repl_state为KVDATA_REPL_CONNECT，则会调用函数connectWithMaster，向主节点发起TCP建链请求
 */
int connectWithMaster(void) {
    int fd;

    // 向主服务器发起connect连接请求，获得已连接文件描述符fd
    fd = anetTcpNonBlockConnect(NULL,server.masterhost,server.masterport);
    if (fd == -1) {
        printf("Unable to connect to MASTER: %s\n",  strerror(errno));
        return AE_ERR;
    }
    // 监听主服务器 fd 的读和写事件，并绑定文件事件处理器syncWithMaster，该函数用于处理主从节点间的握手过程
    if (aeCreateFileEvent(server.eventsLoop,fd,AE_READABLE|AE_WRITABLE,syncWithMaster,NULL) == AE_ERR)
    {
        close(fd);
        printf("Can't create readable event for SYNC\n");
        return AE_ERR;
    }
    // 更新最近一次主从服务器交互时间(从节点向主节点请求连接成功)
    server.repl_transfer_lastio = server.unixtime;
    //初始化与主服务器建立连接的套接字
    server.repl_transfer_s = fd;

    // 将状态改为KVDATA_REPL_CONNECTING，表示从节点正在向主节点建立连接
    server.repl_state = KVDATA_REPL_CONNECTING;
    printf("slave repl_state is KVDATA_REPL_CONNECTING now.\n");

    return AE_OK;
}

/*
 * 当主从节点间的TCP建链成功之后，
 * 就会触发socket描述符server.repl_transfer_s上的可写事件，从而调用函数syncWithMaster。
 * 该函数处理从节点与主节点间的握手过程。
 * 也就是从节点在向主节点发送TCP建链请求，到接收RDB数据之前的过程。
 * */
void syncWithMaster(aeEventLoop *el, int fd, void *privdata, int mask) {
    //从服务器用于接收主服务器传来RDB文件的临时文件名
    char tmpfile[256];
    //向主服务器发送部分请求后，收到的回复
    int psync_result;
    //无效参数，避免警告
    KVDATA_NOTUSED(el);
    KVDATA_NOTUSED(privdata);
    KVDATA_NOTUSED(mask);

    // 如果从节点处于 SLAVEOF NO ONE 模式，说明从节点收到了客户端执行的"slave  no  one"命令，因此直接关闭socket描述符，然后返回
    if (server.repl_state == KVDATA_REPL_NONE) {
        close(fd);
        return;
    }

    // 如果从服务器的复制状态为 CONNECTING ，那么在进行初次同步之前，
    // 向主服务器发送一个非阻塞的 PING
    // 因为接下来的 RDB 文件发送非常耗时，所以我们想确认主服务器真的能访问
    if (server.repl_state == KVDATA_REPL_CONNECTING) {
        printf("Non blocking connect for SYNC fired the event.\n");
        // 手动发送同步 PING ，暂时取消监听写事件
        // 因为后面我们希望保证此时主服务器对应的客户端回复缓冲区为空
        aeDeleteFileEvent(server.eventsLoop,fd,AE_WRITABLE);

        // 更新复制状态KVDATA_REPL_RECEIVE_PONG，等待主服务PONG回复
        server.repl_state = KVDATA_REPL_RECEIVE_PONG;

        char* ping = "*1\r\n$4\r\nping\r\n";
        //发送 PING
        write(fd,ping,strlen(ping));

        // 返回，等待 PONG 到达
        return;
    }

    // 接收 PONG 命令
    if (server.repl_state == KVDATA_REPL_RECEIVE_PONG) {
        char buf[1024];

        // 手动接收 PONG ，暂时取消监听读事件
        // 因为后面我们希望保证此时主服务器对应的客户端查询缓冲区为空
        aeDeleteFileEvent(server.eventsLoop,fd,AE_READABLE);

        // 接收 PONG
        if (read(fd,buf,sizeof(buf)) == -1)
        {
            printf("I/O error reading PING reply from master: %s", strerror(errno));
            goto error;
        }

        // 接收到的数据只有两种可能：
        // 第一种是 +PONG ，第二种是因为未验证而出现的 -NOAUTH 错误
        if (strcmp(buf,"+PONG") != 0)
        {
            // 未接收到PONG
            printf("Error reply to PING from master: %s\n",buf);
            goto error;
        } else {
            // 接收到 +PONG
            // 更新复制状态KVDATA_REPL_SEND_PSYNC，准备向主服务器发送命令： "PSYNC <master_run_id> <repl_offset>"
            server.repl_state = KVDATA_REPL_SEND_PSYNC;
        }
    }
    //向主服务器发送命令： "PSYNC <master_run_id> <repl_offset>"
    if (server.repl_state == KVDATA_REPL_SEND_PSYNC){
        // 调用slaveTryPartialResynchronization读取主节点对于"PSYNC"命令的回复
        psync_result = slaveTryPartialResynchronization(fd);
    }
    // 可以执行部分重同步
    if (psync_result == PSYNC_CONTINUE) {
        printf("MASTER <-> SLAVE sync: Master accepted a Partial Resynchronization.\n");
        // 返回
        return;
    }
    /*--------------------------如果执行到这里，表示接下来要进行完全重同步过程-------------------------- */
    // 打开一个临时文件，用于写入和保存接下来从主服务器传来的 RDB 文件数据
    snprintf(tmpfile,256, "temp-%d.%ld.rdb",(int)server.unixtime,(long int)getpid());
    int dfd = open(tmpfile,O_CREAT|O_WRONLY|O_EXCL,0644); 
    if (dfd == -1) {
        printf("Opening the temp file needed for MASTER <-> SLAVE synchronization: %s",strerror(errno));
        goto error;
    }
    // 设置一个读事件处理器，来读取主服务器的 RDB 文件
    if (aeCreateFileEvent(server.eventsLoop,fd, AE_READABLE,readSyncBulkPayload,NULL) == AE_ERR)
    {
        printf("Can't create readable event for PSYNC: %s (fd=%d)\n", strerror(errno),fd);
        goto error;
    }
    // 设置复制状态为KVDATA_REPL_TRANSFER，表示开始接收主节点的RDB数据
    server.repl_state = KVDATA_REPL_TRANSFER;

    // 更新统计信息
    //初始化RDB文件大小为-1，因为在读取RDB文件时会获取真正的文件大小
    server.repl_transfer_size = -1;
    //初始化已读 RDB 文件内容的字节数
    server.repl_transfer_read = 0;
    //初始化从服务器最近一次执行 fsync 时的偏移量
    //为的是从服务器在每读取一定长度的RDB文件内容后，将数据刷新到磁盘中，
    //避免一次性刷新太多数据到磁盘，撑爆I/O
    server.repl_transfer_last_fsync_off = 0;
    //初始化保存 RDB 文件的临时文件的描述符
    server.repl_transfer_fd = dfd;
    //初始化保存 RDB 文件的临时文件名字
    server.repl_transfer_tmpfile = tmpfile;
    //初始化最近一次读入 RDB 内容的时间
    server.repl_transfer_lastio = server.unixtime;
    return;

    //错误处理
    error:
    //关闭与主服务器建立连接的文件描述符
    close(fd);
    //从节点与主服务器建立连接的套接字置为-1
    server.repl_transfer_s = -1;
    //从服务器去复制状态设置为slaveof命令后的待连接状态
    server.repl_state = KVDATA_REPL_CONNECT;
    return;
}

/* 在syncWithMaster函数中，握手过程结束后，需要进行完全重同步时，
 * 从节点注册与主服务器连接的socket描述符server.repl_transfer_s上的可读事件，
 * 事件回调函数为readSyncBulkPayload。从节点调用该函数接收主节点发来的RDB数据
 *
 * （1）读取RDB文件中的内容到保存 RDB 文件的临时文件的描述符中
 * （2）当RDB中所有内容读取完毕后，清空从服务器的数据库，执行RDBLoad
 * */
#define REPL_MAX_WRITTEN_BEFORE_FSYNC (1024*1024*8) /* 8 MB */
void readSyncBulkPayload(aeEventLoop *el, int fd, void *privdata, int mask) {
    char buf[4096];
    ssize_t nread, readlen;
    off_t left;

    //无效参数，避免警告
    KVDATA_NOTUSED(el);
    KVDATA_NOTUSED(privdata);
    KVDATA_NOTUSED(mask);

    // 读取 RDB 文件的大小
    // server.repl_transfer_size的值表示从节点要读取的RDB数据的总长度
    // 如果当前其值为-1，说明本次是第一次接收RDB数据。
    if (server.repl_transfer_size == -1) {

        // 调用读函数，在server.repl_syncio_timeout*1000时间内从fd中读取一行内容到buf
        if (read(fd,buf,1024) == -1) {
            printf("I/O error reading bulk count from MASTER: %s\n", strerror(errno));
            goto error;
        }

        //主节点将数据保存到RDB文件后，将文件内容加上"$<len>/r/n"的头部，len表示RDB文件的大小
        //如果读取到的内容既不是上述两种情况，也不是'$'开头，则说明读取的内容格式错误
        if (buf[0] != '$') {
            // 读入的内容出错，和协议格式不符
            printf("Bad protocol from MASTER, the first byte is not '$' (we received '%s'), are you sure the host and port are right?", buf);
            goto error;
        }
        
        // 获得 RDB 文件大小，本处strtol函数会根据十进制将buf中的字符串转换成长整型
        server.repl_transfer_size = strtol(buf+1,NULL,10);

        //在日志中打印RDB文件的大小
        printf("MASTER <-> SLAVE sync: receiving %lld bytes from master\n", (long long) server.repl_transfer_size);
        return;
    }
    /*读数据*/
    // 计算还有多少字节要读
    left = server.repl_transfer_size - server.repl_transfer_read;
    //计算本次可读取内容的长度，一次最大读取4KB
    readlen = (left < (signed)sizeof(buf)) ? left : (signed)sizeof(buf);

    // 从RDB文件中读取读取readlen长度内容到buf
    nread = read(fd,buf,readlen);
    if (nread <= 0) {
        printf("I/O error trying to sync with MASTER: %s", (nread == -1) ? strerror(errno) : "connection lost");
        goto error;
    }
    // 更新最近一次从 RDB 读入内容的时间
    server.repl_transfer_lastio = server.unixtime;
    //将从RDB读取到的内容写入保存 RDB 文件的临时文件的描述符中
    if (write(server.repl_transfer_fd,buf,nread) != nread) {
        printf("Write error or short write writing to the DB dump file needed for MASTER <-> SLAVE synchronization: %s", strerror(errno));
        goto error;
    }
    // 更新已读 RDB 文件内容的字节数
    server.repl_transfer_read += nread;

    // 定期将读入的文件 fsync 到磁盘，以免 buffer 太多，一下子写入时撑爆 IO，至少每次同步8M内容
    if (server.repl_transfer_read >= server.repl_transfer_last_fsync_off + REPL_MAX_WRITTEN_BEFORE_FSYNC)
    {
        off_t sync_size = server.repl_transfer_read - server.repl_transfer_last_fsync_off;
        fsync(server.repl_transfer_fd);  
        //更新最近一次读到的数据量
        server.repl_transfer_last_fsync_off += sync_size;
    }
       
    // 检查 RDB 中的内容是否已经传送完毕
    if (server.repl_transfer_read == server.repl_transfer_size) {

        // 先清空旧数据库
        printf( "MASTER <-> SLAVE sync: Flushing old data\n");
        //清空所有数据
        emptyDb();

        // 先删除对主服务器的读事件监听，因为 Load() 函数也会监听读事件
        // 从节点在加载RDB数据时，是不能处理主节点发来的其他数据的
        aeDeleteFileEvent(server.eventsLoop,server.repl_transfer_s,AE_READABLE);

        // 载入 RDB
        if (rdbLoad(server.repl_transfer_tmpfile) != AE_OK) {
            printf("Failed trying to load the MASTER synchronization DB from disk\n");
            goto error;
        }

        // 如果传送完毕，将临时文件改名为 dump.rdb
        if (rename(server.repl_transfer_tmpfile,server.rdb_filename) == -1) {
            printf("Failed trying to rename the temp DB into dump.rdb in MASTER <-> SLAVE synchronization: %s\n", strerror(errno));
            goto error;
        }

        // 关闭临时文件
        zfree(server.repl_transfer_tmpfile);
        // 关闭保存 RDB 文件的临时文件的描述符
        close(server.repl_transfer_fd);
        // 将主服务器设置成一个 KVDATA client
        // 注意 createClient 会为主服务器绑定事件，为接下来接收命令做好准备
        server.master = createClient(server.repl_transfer_s);
        // 标记这个客户端为主服务器
        server.master->flags |= KVDATA_MASTER;
        // 更新复制状态，表示主从节点已完成握手和接收RDB数据的过程；
        server.repl_state = KVDATA_REPL_CONNECTED;
        // 设置主服务器的复制偏移量
        server.master->reploff = server.repl_master_initial_offset;
    }
    return;

//错误处理
error:
    //停止下载RDB文件，关闭与主服务器的连接，将从服务器状态调整至KVDATA_REPL_CONNECT
    cancelReplicationHandshake();
    return;
}

/*
 * 这个函数由 freeClient() 函数调用，它将当前的 master 记录到 master cache 里面，
 * 然后返回。
 * 在 PSYNC 请求部分同步成功时将缓存中的 master 提取出来，重新成为新的 master 。
 */
void replicationCacheMaster(KVClient *c) {
    listNode *ln;

    //确保当前从服务器与主服务器存在连接，并且server.cached_master为空
    assert(server.master != NULL && server.cached_master == NULL);
    printf("Caching the disconnected master state.\n");

    // 从客户端链表中移除主服务器
    ln = listSearchKey(server.clients,c);
    assert(ln != NULL);
    listDelNode(server.clients,ln);

    // 迁移到备份的 master
    server.cached_master = server.master;

    // 删除对主服务器的事件监视，关闭 socket
    aeDeleteFileEvent(server.eventsLoop,c->fd,AE_READABLE);
    aeDeleteFileEvent(server.eventsLoop,c->fd,AE_WRITABLE);
    close(c->fd);

    c->fd = -1;

    // 重置复制状态，并将 server.master 设为 NULL
    // 并强制断开这个服务器的所有从服务器，让它们执行
    server.master = NULL;
    //从从节点复制状态调整至待连接KVDATA_REPL_CONNECT
    server.repl_state = KVDATA_REPL_CONNECT;
    // 和主服务器失联，强制所有这个服务器的所有从服务器重同步
    if (server.masterhost != NULL) disconnectSlaves();
}

/*
 *  在重连接之后，尝试进行部分重同步。
 *  可能情况如下：
 *
 * （1）该服务器不是第一个进行主从复制，尝试部分重同步，命令为 "PSYNC <master_run_id> <repl_offset>"
 * （2）该服务器是第一个进行主从复制，server.cached_master为空，发送 "PSYNC ? -1" ，要求完整重同步
 *
 *  在向主服务器发送命令后，获得命令回复，情况如下：
 * （1）接收到 "+FULLRESYNC <runid>  <offset>" ，进行完整重同步
 * （2）接收到”+CONTINUE“，进行部分重同步
 * （3）接收到错误回复
 *
 * 针对服务器回复命令的上述三种情况，本函数依次有如下返回值：
 * PSYNC_FULLRESYNC
 * PSYNC_CONTINUE
 * PSYNC_NOT_SUPPORTED
 */
int slaveTryPartialResynchronization(int fd) {
    char *psync_runid;
    char psync_offset[32];
    sds reply;
    int ret;
    char cmd[256];
    /*
     * 从节点发送"PSYNC  <runid> <offset>"命令后，
     * 如果主节点不支持部分重同步，则会回复信息为"+FULLRESYNC <runid>  <offset>"，表示要进行完全重同步，
     * 其中<runid>表示主节点的运行ID，记录到server.repl_master_runid中，
     * <offset>表示主节点的初始复制偏移，记录到server.repl_master_initial_offset中。
     * */
    server.repl_master_initial_offset = -1;
    //该服务器不是第一个进行主从复制
    if (server.cached_master) {
        // 缓存存在，尝试部分重同步
        // 命令为 "PSYNC <master_run_id> <repl_offset>"
        psync_runid = server.cached_master->replrunid;
        snprintf(psync_offset,sizeof(psync_offset),"%lld", server.cached_master->reploff+1);
        printf("Trying a partial resynchronization (request %s:%s).\n", psync_runid, psync_offset);
    } else {
        // 缓存不存在
        // 发送 "PSYNC ? -1" ，要求完整重同步
        printf("Partial resynchronization not possible (no cached master).\n");
        psync_runid = "?";
        memcpy(psync_offset,"-1",3);
    }

    // 向主服务器发送 PSYNC  <runid> <offset> 命令，并获得主服务器的回复reply
    printf("Send to Master: %s  %s  %s","PSYN.\n",psync_runid,psync_offset);
    ret = snprintf(cmd, sizeof(cmd),"*3\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n$%ld\r\n%s\r\n", 
          strlen("PSYNC"),"PSYNC", strlen(psync_runid),psync_runid, strlen(psync_offset),psync_offset);

    //此处必须同步发送，否则接收到的数据不同步
    reply = sendSynchronousCommand(fd,cmd);
    // 接收到 "+FULLRESYNC <runid>  <offset>" ，进行完整重同步
    printf("Reply from Master: %s.\n",reply);
    if (!strncmp(reply,"+FULLRESYNC",11)) {
        char *runid = NULL;
        char*offset = NULL;
        // 分析并记录主服务器的运行ID <runid>
        runid = strchr(reply,' ');
        if (runid) {
            runid++;
            // 分析并记录主服务器的初始复制偏移量
            offset = strchr(runid,' ');
            if (offset) offset++;
        }

        // 检查 run id 的合法性
        if (!runid || !offset || (offset-runid-1) != KVDATA_RUN_ID_SIZE) {
            printf("Master replied with wrong +FULLRESYNC syntax.\n");
            // 主服务器支持 PSYNC ，但是却发来了异常的 run id
            // 只好将 run id 设为 0 ，让下次 PSYNC 时失败
            memset(server.repl_master_runid,0,KVDATA_RUN_ID_SIZE+1);
        } else {
            // 保存主服务器 runid
            memcpy(server.repl_master_runid, runid, offset-runid-1);
            server.repl_master_runid[KVDATA_RUN_ID_SIZE] = '\0';
            // 保存主服务器可用于复制的初始复制偏移量 initial offset
            server.repl_master_initial_offset = strtoll(offset,NULL,10);
            // 打印日志，这是一个 FULL resync
            printf("Full resync from master: %s:%lld\n", server.repl_master_runid, server.repl_master_initial_offset);
        }
        // 要开始完整重同步，不可能执行部分同步，备份中的 master 已经没用了，清除它
        replicationDiscardCachedMaster();
        sdsfree(reply);
        // 返回状态
        return PSYNC_FULLRESYNC;
    }
    // 接收到”+CONTINUE“，进行部分重同步
    else if (!strncmp(reply,"+CONTINUE",9)) {
        printf("Successful partial resynchronization with master.\n");
        sdsfree(reply);
        // 由于执行的是部分重同步，因此可以直接承接上次主服务器的客户端
        // 将缓存中的 master 设为当前 master
        replicationResurrectCachedMaster(fd);
        // 返回状态
        return PSYNC_CONTINUE;
    //其它回复一律认为是错误回复
    }else {
        printf("Master does not support PSYNC or is in " "error state (reply: %s)\n", reply);
    }
    sdsfree(reply);
    //执行到这说明已经不可能执行部分同步，备份中的 master 已经没用了，清除它
    replicationDiscardCachedMaster();
    // 主服务器不支持 PSYNC
    return PSYNC_ERR;
}
/*
 * 将缓存中的 cached_master 设置为服务器的当前 master 。
 *
 * 当部分重同步准备就绪之后，调用这个函数。
 * master 断开之前遗留下来的数据可以继续使用。
 */
void replicationResurrectCachedMaster(int newfd) {
    
    // 设置 master
    server.master = server.cached_master;
    // 此时处于KVDATA_REPL_CONNECTED，不用保存备份主服务器
    server.cached_master = NULL;
    // 更新主服务器客户端的套接字
    server.master->fd = newfd;

    //更新从节点与主服务器客户端最近一次互动时间
    server.master->lastinteraction = server.unixtime;

    // 回到已连接状态
    server.repl_state = KVDATA_REPL_CONNECTED;

    // 将 master 重新加入到客户端列表中
    listAddNodeTail(server.clients,server.master);
    // 监听 master 的读事件
    if (aeCreateFileEvent(server.eventsLoop, newfd, AE_READABLE, recvData, server.master)) {
        printf("Error resurrecting the cached master, impossible to add the readable handler: %s\n", strerror(errno));
        //后期可以改成异步释放客户端
        freeClient(server.master); 
    }

    //如果写缓冲区中有挂起的数据，我们可能还需要安装写处理程序
    if (server.master->bufpos || listLength(server.master->reply)) {
        if (aeCreateFileEvent(server.eventsLoop, newfd, AE_WRITABLE,sendReplyToClient, server.master)) {
            printf("Error resurrecting the cached master, impossible to add the writable handler: %s.\n", strerror(errno));
            freeClient(server.master); 
        }
    }
}

/* 
 * 向主机发送同步命令,获取主服务器对从节点部分同步的回复
 * 此处使用同步命令，主要是方便不用多次调用该函数
 */
char *sendSynchronousCommand(int fd, char* cmd) {
    char buf[256];
    // 发送命令到主服务器
    if (write(fd,cmd,strlen(cmd)) == -1) {
        return sdscatprintf(sdsnewlen("",0),"-Writing to master: %s",
                strerror(errno));
    }
    // 从主服务器中读取回复
    if (read(fd,buf,sizeof(buf)) == -1)
    {   
        return sdscatprintf(sdsnewlen("",0),"-Reading from master: %s",
                strerror(errno));
    }
    return sdsnew(buf);
}

/*---------------------------------------------------主服务器---------------------------------------------------*/

/*
 * 创建复制积压缓冲区backlog
 */ 
void createReplicationBacklog(void) {

    //先验复制积压缓冲区为空
    assert(server.repl_backlog == NULL);
    // 为复制积压缓冲区分配空间
    server.repl_backlog = zmalloc(server.repl_backlog_size);
    // 初始化backlog中数据长度
    server.repl_backlog_histlen = 0;
    // 初始化backlog 的当前索引，增加数据时使用
    server.repl_backlog_idx = 0;
    // 尽管没有任何数据，
    // 但 backlog 第一个字节的逻辑位置应该是 master_reploff 后的第一个字节
    server.repl_backlog_off = server.master_reploff+1;
}


/*  从节点在握手过程中第一个发来的命令是”PING”，
 * 主节点调用的pingCommand函数处理，只是回复字符串”+PONG”即可。
 */
void pingCommand(KVClient *c) {
    addReply(c,shared.pong);
    printf("reply pong\n");
}

/* 
 * REPLCONF ACK <offset> 
 * 从中取出<offset>信息，保存在客户端的c->repl_ack_off属性中，
 * 记录从节点已处理的复制流的偏移量，并更新最近一次发送 ack 的时间
 */
void replconfCommand(KVClient *c) {
    int j;
    char reply[256];
    //检查参数是否出现命令格式错误
    if ((c->argc % 2) == 0) {
        addReply(c,shared.syntaxerr);
        return;
    }
    if (!strcasecmp(c->argv[1]->ptr,"ack")) {
        // 从服务器使用 REPLCONF ACK 告知主服务器，
        // 从服务器目前已处理的复制流的偏移量
        long long offset;

        if (!(c->flags & KVDATA_SLAVE)) return;
        //将参数中从服务器目前的复制偏移量写入从服务器对应客户端的c->repl_ack_off中
        if ((getLongLongFromObject(c->argv[2], &offset) != AE_OK))
            return;
        // 如果 offset 已改变，那么更新
        if (offset > c->repl_ack_off)
            c->repl_ack_off = offset;
        return;
    //格式错误
    } else {
            // 创建临时文件"temp-getpid().rdb"
        snprintf(reply,256,"Unrecognized REPLCONF option: %s", (char*)c->argv[j]->ptr);
        addReplySds(c, sdsnew(reply));
        return;
    }
    addReply(c,shared.ok);
}

/*
 * 释放 dst 客户端原有的回复缓冲区中的内容
 * 并将 src 客户端的回复缓冲区的内容复制给 dst
 */
void copyClientOutputBuffer(KVClient *dst, KVClient *src) {

    // 释放 dst 原有的回复链表
    listRelease(dst->reply);
    // 复制新src回复链表到 dst
    dst->reply = listDup(src->reply);

    // 复制内容到回复 buf
    //即使src中的内容不dst短，复制以后dst的内容将会被src的内容覆盖（多余的不会出现）
    memcpy(dst->buf,src->buf,src->bufpos);

    // 同步偏移量和字节数
    dst->bufpos = src->bufpos;
    dst->reply_bytes = src->reply_bytes;
}


/*
 * 根据参数的个数，分别执行部分重同步PSYNC 或完整重同步SYNC 命令
 * 此时的客户端乃是从服务器
 */
void syncCommand(KVClient *c) {

    // 如果这是一个从服务器，但其复制状态不是KVDATA_REPL_CONNECTED，
    // 说明当前的从节点实例还没有到接收并加载完其主节点发来的RDB数据的步骤，
    // 这种情况下，该从节点实例是不能为其下游从节点进行同步的
    if (server.masterhost && server.repl_state != KVDATA_REPL_CONNECTED) {
        addReplySds(c,sdsnew("Can't SYNC while not connected with my master"));
        return;
    }
    // 因为主节点接下来需要为该从节点进行后台RDB数据转储
    // 同时需要将前台接收到的其他客户端命令请求缓存到该从节点客户端的输出缓存中，
    // 这就需要一个完全清空的输出缓存，才能为该从节点保存从执行BGSAVE开始的命令流。
    // 因此，如果从节点客户端的输出缓存中尚有数据，直接回复错误信息，不能 SYNC。
    if (listLength(c->reply) != 0 || c->bufpos != 0) {
        addReplySds(c,sdsnew("SYNC and PSYNC are invalid with pending output"));
        return;
    }

    printf("Slave asks for synchronization.\n");

    /* 如果这是一个 PSYNC 命令：
     * （1）那么尝试进行部分重同步PSYNC;
     * （2）如果部分重同步失败，那么检查是否为强制执行完整重同步命令;
     */
    if (!strcasecmp(c->argv[0]->ptr,"psync")) {
        // 尝试进行部分重同步PSYNC
        if (masterTryPartialResynchronization(c) == AE_OK)  return;
    } else {
        addReply(c,shared.syntaxerr);
    }
    /*------------------------------------------------------以下是完整重同步的情况---------------------------------------------------*/ 
    // 检查是否有 BGSAVE 在执行
    if (server.rdb_child_pid != -1) {
        KVClient *slave;
        listNode *ln = server.slaves->head;

       //遍寻从节点列表
        while(ln != NULL) {
            slave = ln->value;
            if (slave->replstate == KVDATA_REPL_WAIT_BGSAVE_END) break;
            ln = ln->next;
        }
        /* 情况1：遍历从服务器列表，如果有至少一个 slave 在等待这个 BGSAVE 完成
         * 那么说明正在进行的 BGSAVE 所产生的 RDB 也可以为其他 slave 所用*/
        if (ln) {
            //在后台进行有RDB数据转储尚未完成时，如果又有新的从节点B发来了"PSYNC"命令，同样需要完全重同步。
            //此时主节点后台正在进行RDB数据转储，而且已经为A缓存了命令流。
            //那么从节点B完全可以重用这份RDB数据，而无需再执行一次RDB转储了。
            //而且将A中的输出缓存复制到B的输出缓存中，就能保证B的数据库状态也能与主节点一致了。
            //因此，直接将B的复制状态直接置为KVDATA_REPL_WAIT_BGSAVE_END，等到后台RDB数据转储完成时，直接将该转储文件同时发送给从节点A和B即可。
            copyClientOutputBuffer(c,slave);
            c->replstate = KVDATA_REPL_WAIT_BGSAVE_END;
            printf("Waiting for end of BGSAVE for SYNC\n");

        /* 情况2：如果找不到这样的从节点客户端，则主节点需要在当前的BGSAVE操作完成之后，重新执行一次BGSAVE操作*/
        } else {
            c->replstate = KVDATA_REPL_WAIT_BGSAVE_START;
            printf("Waiting for next BGSAVE for SYNC\n");
        }

    /* 情况3：如果当前没有子进程在进行RDB转储，则调用rdbSaveBackground开始进行BGSAVE操作*/
    } else {
        // 没有 BGSAVE 在进行，开始一个新的 BGSAVE
        printf("Starting BGSAVE for SYNC\n");
        if (rdbSaveBackground(server.rdb_filename) != AE_OK) {
            printf("Replication failed, can't BGSAVE.\n");
            addReplySds(c,sdsnew("Unable to perform background save"));
            return;
        }
        // 设置状态
        c->replstate = KVDATA_REPL_WAIT_BGSAVE_END;
    }

    //初始化从服务器客户端中用于保存主服务器传来的 RDB 文件的文件描述符
    c->repldbfd = -1;
    // 如果当前客户端不是从节点客户端，则将其状态调整为从节点客户端
    if (!(c->flags & KVDATA_SLAVE)){
    //将KVDATA_SLAVE标记记录到从节点客户端的标志位中，以标识该客户端为从节点客户端
    c->flags |= KVDATA_SLAVE;
    // 将从节点客户端添加到 slave 列表中
    listAddNodeTail(server.slaves,c);
    }

    //强制后续将回复缓冲区的命令发送给从服务器时，选择正确的数据库
    server.slaveseldb = -1;

    // 如果是第一个 slave ，那么初始化 backlog
    if (listLength(server.slaves) == 1 && server.repl_backlog == NULL)
        createReplicationBacklog();
    return;
}


/*
 * 主服务器尝试进行部分同步
 * 成功则将复制部分写入客户端的回复缓冲区中，并向客户端文件描述符符写入+CONTINUE，并返回 KVDATA_OK
 * 失败则向客户端文件描述符符写入+FULLRESYNC %s %lld，并返回 KVDATA_ERR 。
 *
 * 此时从服务器以客户端的形式向主服务器发送命令 PSYNC <runid> <offset>
 */
int masterTryPartialResynchronization(KVClient *c) {
    long long psync_offset, psync_len;

    //获取从服务器欲要复制的主服务器运行id
    char *master_runid = c->argv[1]->ptr;
    char buf[128];
    int buflen;

    // 检查从服务器欲要复制的主服务器运行id是否和当前主服务器运行id一致，只有一致的情况下才有部分重同步（PSYNC）的可能
    if (strcasecmp(master_runid, server.serverid)) {

        // 从服务器欲要复制的主服务器运行id是否和当前主服务器运行id不一致,且<runid>参数不为'?'，即不是强制执行完整同步的命令
        if (master_runid[0] != '?') {
            printf("Partial resynchronization not accepted: " "Runid mismatch (Client asked for runid '%s', my runid is '%s')\n", master_runid, server.serverid);
        
        // 从服务器提供的<runid>为 '?' ，表示强制完整重同步(FULL RESYNC)
        } else {
            printf("Full resync requested by slave.\n");
        }
        //直接跳转到完整重同步
        goto need_full_resync;
    }

    // 取出命令中从服务器请求同步起始偏移量位置，即<offset>对象中取出psync_offset参数
    if (getLongLongFromObject(c->argv[2],&psync_offset) != AE_OK) goto need_full_resync;

    // 如果服务器复制挤压缓冲区中无内容、或想要恢复的那部分数据已经被覆盖、或起始复制偏移量不在复制挤压缓冲区偏移量范围内
    // 直接跳转到完整重同步
    if (!server.repl_backlog ||
        psync_offset < server.repl_backlog_off ||
        psync_offset > (server.repl_backlog_off + server.repl_backlog_histlen))
    {
        // 执行 FULL RESYNC
        printf("Unable to partial resync with the slave for lack of backlog (Slave request was: %lld).\n", psync_offset);
        //如果从服务器欲要复制的起始偏移量大于全局偏移量，发出警告
        if (psync_offset > server.master_reploff) {
            printf("Warning: slave tried to PSYNC with an offset that is greater than the master replication offset.\n");
        }
        goto need_full_resync;
    }

    /*
     * 程序运行到这里，说明可以执行局部复制
     * 1) 将客户端状态设为 salve
     * 2) 向 slave 发送 +CONTINUE ，表示局部复制的请求被接受
     * 3) 发送 backlog 中，客户端所需要的数据
     */
    if (!(c->flags & KVDATA_SLAVE)){
    //将KVDATA_SLAVE标记记录到从节点客户端的标志位中，以标识该客户端为从节点客户端
    c->flags |= KVDATA_SLAVE;
    // 将从节点客户端添加到 slave 列表中
    listAddNodeTail(server.slaves,c);
    }
    //从服务器的复制状态设置”KVDATA_REPL_ONLINE“
    c->replstate = KVDATA_REPL_ONLINE;

    // 向从服务器发送一个同步 +CONTINUE ，表示 PSYNC 局部复制可以执行
    buflen = snprintf(buf,sizeof(buf),"+CONTINUE\r\n");
    if (write(c->fd,buf,buflen) != buflen) {
        //发送同步回复信号失败，异步关闭该从服务器
        freeClient(c);
        return AE_OK;
    }
    // 发送 backlog 中的内容（也即是从服务器缺失的那些内容）到从服务器
    psync_len = addReplyReplicationBacklog(c,psync_offset);
    printf("Partial resynchronization request accepted. Sending %lld bytes of backlog starting from offset %lld.\n", psync_len, psync_offset);

    return AE_OK;

//完整重同步预处理操作
need_full_resync:
    // 刷新 psync_offset，使其变成全局偏移量
    psync_offset = server.master_reploff;
    // 刷新 psync_offset
    if (server.repl_backlog == NULL) psync_offset++;

    // 向从服务器发送 +FULLRESYNC ，表示需要完整重同步
    printf("Sends +FULLRESYNC to the slave server.\n");
    buflen = snprintf(buf,sizeof(buf),"+FULLRESYNC %s %lld\r\n", server.serverid,psync_offset);
    if (write(c->fd,buf,buflen) != buflen) {
        freeClient(c);
        return AE_OK;
    }
    return AE_ERR;
}

/*
 * 向从服务器 c 发送 backlog 中从 offset 到 backlog 尾部之间的数据
 */
long long addReplyReplicationBacklog(KVClient *c, long long offset) {
    long long j, skip, len;

    //打印服务器需要局部复制的起始偏移量
    printf("[PSYNC] Slave request offset: %lld\n", offset);

    //判断复制积压缓冲区内无内容则直接返回
    if (server.repl_backlog_histlen == 0) {
        printf("[PSYNC] Backlog history len is zero\n");
        return 0;
    }

    //打印复制积压缓冲区的大小
    printf("[PSYNC] Backlog size: %lld\n", server.repl_backlog_size);
    //打印复制积压缓冲区的首地址（可以被还原的第一个字节）的偏移量
    printf("[PSYNC] First byte: %lld\n", server.repl_backlog_off);
    //打印复制积压缓冲区中数据的长度
    printf("[PSYNC] History len: %lld\n", server.repl_backlog_histlen);
    //打印复制积压缓冲区当前最新偏移量
    printf("[PSYNC] Current index: %lld\n", server.repl_backlog_idx);

    //从offset开始复制，跳过前面无用的内容
    skip = offset - server.repl_backlog_off;
    printf("[PSYNC] Skipping: %lld\n", skip);

    //计算获得当前最新偏移量在backlog中的索引
    j = (server.repl_backlog_idx +
        (server.repl_backlog_size-server.repl_backlog_histlen)) %
        server.repl_backlog_size;
    printf("[PSYNC] Index of first byte: %lld\n", j);

    //计算从服务器局部复制的起始偏移量在backlog中的索引
    j = (j + skip) % server.repl_backlog_size;

    //计算本次局部复制的内容长度
    len = server.repl_backlog_histlen - skip;
    printf("[PSYNC] Reply total length: %lld\n", len);
    while(len) {
        //判断backlog中的内容是否已经成环
        //如果成环，则需要分两次取出内容，如果不成环，可以一次直接取出内容
        long long thislen =
            ((server.repl_backlog_size - j) < len) ?
            (server.repl_backlog_size - j) : len;
        //打印出局部复制内容的长度
        printf("[PSYNC] addReply() length: %lld\n", thislen);
        //将局部复制的内容以字符串对象的形式写入客户端c的回复缓冲区中
        addReplySds(c,sdsnewlen(server.repl_backlog + j, thislen));
        len -= thislen;
        j = 0;
    }
    //返回局部复制的内容长度
    return server.repl_backlog_histlen - skip;
}



/*
 * 复制 cron 函数，每秒调用一次
 */ 
void replicationCron(void) {
    
    // 尝试连接主服务器
    if (server.repl_state == KVDATA_REPL_CONNECT) {
        printf("Connecting to MASTER %s:%d\n", server.masterhost, server.masterport);
        if (connectWithMaster() == AE_OK) {
            printf("MASTER <-> SLAVE sync started\n");
        }
    }
}
/*
 * 如果主节点为从节点在后台进行RDB数据转储时，如果有新的从节点的SYNC或PSYNC命令到来。
 * 则在该新从节点无法复用当前正在转储的RDB数据的情况下，主节点需要在当前BGSAVE操作之后，重新进行一次BGSAVE操作。
 *
 * 这个函数就是在 BGSAVE 完成之后的异步回调函数，它指导该怎么执行和 slave 相关的 RDB 下一步工作。
 *
 * 参数bgsaveerr表示后台子进程的退出状态
 */
void updateSlavesWaitingBgsave(int bgsaveerr) {
    int startbgsave = 0;
    listNode *ln = server.slaves->head;
    // 遍历列表server.slaves
    while(ln != NULL) {
        KVClient *slave = ln->value;
        // 如果从节点客户端当前的复制状态为KVDATA_REPL_WAIT_BGSAVE_START，
        // 说明该从节点是在后台子进程进行RDB数据转储期间，连接到主节点上的，并且没有合适的其他从节点可以进行复用。
        if (slave->replstate == KVDATA_REPL_WAIT_BGSAVE_START) {
            // 需要开始新的 BGSAVE，并修改从节点的复制状态为KVDATA_REPL_WAIT_BGSAVE_END
            startbgsave = 1;
            slave->replstate = KVDATA_REPL_WAIT_BGSAVE_END;

        //当服务器正在进行RDB数据转储，且从节点的复制状态为KVDATA_REPL_WAIT_BGSAVE_END时说明该从节点正在等待RDB数据处理完成
        } else if (slave->replstate == KVDATA_REPL_WAIT_BGSAVE_END) {
            /* 执行到这里，说明有 slave 在等待 BGSAVE 完成 */ 
            struct stat buf;

            // 参数bgsaveerr表示后台子进程的退出状态，如果 BGSAVE 执行错误
            if (bgsaveerr != AE_OK) {
                // 释放临时 slave，继续遍历下一个从节点
                freeClient(slave);
                printf("SYNC failed. BGSAVE child returned an error\n");
                continue;
            }
            // 当存在从节点复制状态为KVDATA_REPL_WAIT_BGSAVE_END且当前BGSAVE成功时
            // 打开 RDB 文件
            if ((slave->repldbfd = open(server.rdb_filename,O_RDONLY)) == -1 ||
                fstat(slave->repldbfd,&buf) == -1) {
                freeClient(slave);
                printf("SYNC failed. Can't open/stat DB after BGSAVE: %s\n", strerror(errno));
                continue;
            }
            /*设置偏移量*/
            //已经向从节点发送的RDB数据的字节数；
            slave->repldboff = 0;
            //获取RDB文件的大小
            slave->repldbsize = buf.st_size;
            // 更新从节点复制状态为KVDATA_REPL_SEND_BULK
            slave->replstate = KVDATA_REPL_SEND_BULK;

            //更新需要发送给从节点客户端的RDB文件的长度信息
            slave->replpreamble = sdscatprintf(sdsnewlen("",0),"$%lld\r\n",
                (unsigned long long) slave->repldbsize);
            // 清空之前的写事件处理器
            aeDeleteFileEvent(server.eventsLoop,slave->fd,AE_WRITABLE);
            // 将 sendBulkToSlave 安装为 该从节点slave 的写事件处理器
            // 它用于将 RDB 文件发送给 该从节点slave
            if (aeCreateFileEvent(server.eventsLoop, slave->fd, AE_WRITABLE, sendBulkToSlave, slave) == AE_ERR) {
                freeClient(slave);
                continue;
            }
        }
        ln = ln->next;
    }
    // 需要执行新的 BGSAVE
    if (startbgsave) {

        //执行后台BGSAVE
        //如果执行后台BGSAVE失败，则释放所有依赖该BGSAVE的从节点客户端
        if (rdbSaveBackground(server.rdb_filename) != AE_OK) {
            ln = server.slaves->head;
            printf("SYNC failed. BGSAVE failed\n");
            while(ln != NULL) {
                KVClient *slave = ln->value;
                //释放所有复制状态为KVDATA_REPL_WAIT_BGSAVE_START的从节点
                if (slave->replstate == KVDATA_REPL_WAIT_BGSAVE_START)
                    freeClient(slave);
               ln = ln->next;
            }
        }
    }
}


/*
 * 把RDB文件中的内容发送给所有从节点
 * 并向该从节点客户端发送主服务器进行RDB期间累积的命令流
 */
void sendBulkToSlave(aeEventLoop *el, int fd, void *privdata, int mask) {
    KVClient *slave = privdata;
    KVDATA_NOTUSED(el);
    KVDATA_NOTUSED(mask);
    char buf[KVDATA_IOBUF_LEN];
    ssize_t nwritten, buflen;

    //向从节点发送要发送给从节点客户端的RDB文件的长度信息；
    if (slave->replpreamble) {
        nwritten = write(fd,slave->replpreamble,sdslen(slave->replpreamble));
        //写入错误则打印日志，释放用于表示从节点的临时客户端
        if (nwritten == -1) {
            freeClient(slave);
            return;
        }
    }

    //调用lseek将文件指针定位到该文件中未发送的位置，也就是slave->repldboff的位置
    lseek(slave->repldbfd,slave->repldboff,SEEK_SET);

    // 然后调用read，读取RDB文件中KVDATA_IOBUF_LEN个字节到buf中；
    //检查读取失败则打印日志，释放用于表示从节点的临时客户端
    if ((buflen = read(slave->repldbfd,buf,KVDATA_IOBUF_LEN)) <= 0) {
        printf("Read error sending DB to slave: %s\n", (buflen == 0) ? "premature EOF" : strerror(errno));
        freeClient(slave);
        return;
    }
    // 调用write，将已读取的数据发送给从节点客户端，write返回值为nwritten，将其加到slave->repldboff中。
    if ((nwritten = write(fd,buf,buflen)) == -1) {
        if (errno != EAGAIN) {
            printf("Write error sending DB to slave: %s\n", strerror(errno));
            freeClient(slave);
        }
        return;
    }
    // 如果写入成功，那么更新写入字节数到 repldboff ，等待下次继续写入
    slave->repldboff += nwritten;

    // 如果从节点已经将主服务器传来的RDB文件内容写入完成
    if (slave->repldboff == slave->repldbsize) {
        // 关闭 RDB 文件描述符
        close(slave->repldbfd);
        slave->repldbfd = -1;

        // 删除之前绑定的写事件处理器，将从节点对应的监听写时间从监听句柄中移除
        aeDeleteFileEvent(server.eventsLoop,slave->fd,AE_WRITABLE);

        // 将状态更新为 KVDATA_REPL_ONLINE
        slave->replstate = KVDATA_REPL_ONLINE;

        /*接下来就可以开始向该从节点客户端发送累积的命令流*/
        // 创建向从服务器发送累积命令的写事件处理器
        // 将保存并发送 RDB 期间的回复全部发送给从服务器
        if (aeCreateFileEvent(server.eventsLoop, slave->fd, AE_WRITABLE,
            sendReplyToClient, slave) == AE_ERR) {
            printf("Unable to register writable event for slave bulk transfer: %s\n", strerror(errno));
            freeClient(slave);
            return;
        }
        printf("Synchronization with slave succeeded\n");
    }
}

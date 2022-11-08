#include "client.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
#include "server.h"
#include "zmalloc.h"
#include <string.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "assert.h" 
#include <unistd.h>
/*
 * 根据已连接文件描述符创建新的客户端状态c
 * (1)设置文件描述符cfd为O_NONBLOCK(非阻塞)
 * (2)禁用 Nagle 算法
 * (3)开启 TCP 的 keep alive 选项
 * (4)将已连接描述符添加进行红黑树句柄中进行监听读事件,并设置回调函数为recvData函数
 * 
 * 返回值：客户端状态
 */
KVClient *createClient(int fd)
{
    int flags;

    //获取文件描述符fd的flags
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        printf("fcntl(F_GETFL) err: %s\n", strerror(errno));
        return NULL;
    }

    //设置文件描述符fd为O_NONBLOCK(非阻塞)
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        printf("fcntl(F_SETFL,O_NONBLOCK) err: %s\n", strerror(errno));
        return NULL;
    }

    //禁用 Nagle 算法,小报文可以发送，毕竟客户端的每个请求命令都不大
    int val=1; 
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) == -1)
    {
        printf("setsockopt close TCP_NODELAY err: %s\n", strerror(errno));
        return NULL;
    }

    //开启 TCP 的 keep alive 选项
    int yes = 1;
    if (server.tcpkeepalive && setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) == -1) {
        printf("setsockopt open SO_KEEPALIVE err: %s\n", strerror(errno));
        return NULL;
    }
    // 从查询缓存重读取内容，创建参数，并执行命令
    // 为新创建的客户端分配空间
    KVClient *c = zmalloc(sizeof(KVClient));

    //将已连接描述符添加进行红黑树句柄中进行监听读事件
    //并设置回调函数为recvData函数
    aeCreateFileEvent(server.eventsLoop, fd, AE_READABLE,recvData, c);

    // 默认选0号数据库
    selectDb(c,0);
    // client的套接字
    c->fd = fd;
    // 回复缓冲区的偏移量
    c->bufpos = 0;
    // 查询缓存区
    c->querybuf = sdsnewlen("",0);
    // 命令参数数量
    c->argc = 0;
    // 命令参数
    c->argv = NULL;
    // 当前执行的命令和最近一次执行的命令
    c->cmd = c->lastcmd = NULL;  
    // 查询缓冲区中未读入的命令内容数量
    c->multibulklen = 0;
    // 读入的参数的长度
    c->bulklen = -1; 
    //当前命令名字的长度
    c->execlen = 0;
    // client的状态 FLAG
    c->flags = 0;
    // 设置创建client的时间和最后一次互动的时间
    c->ctime = c->lastinteraction = server.unixtime;
    // 回复链表
    c->reply = listCreate();
    // 回复链表的字节量
    c->reply_bytes = 0;

    // 将真正的client放在服务器的客户端链表中
    if (fd != -1) 
    listAddNodeTail(server.clients,c);

    //初始化被监视的键列表
    c->watched_keys = listCreate();
    // 返回客户端
    return c;
}


/*
 * 释放客户端
 * 注意！！！！：释放客户端不单单需要关闭文件描述符，还需要将对应的文件描述符从epoll红黑树并中移除
 * 否则下次新创建的客户端已连接描述符无法成功加入红黑树柄中进行监听
 */
void freeClient(KVClient *c)
{
    listNode *ln;
    //如果是从服务器与主节点断开连接
    if (server.master && c->flags & KVDATA_MASTER) {
        printf("Connection with master lost.\n");
        //则将server.master对应客户端复制到则将server.CacheMaster上
        replicationCacheMaster(c);
        return;
    }

    //如果是主服务器与从节点断开连接
    if ((c->flags & KVDATA_SLAVE)) {
        printf("Connection with slave %s:%d lost.\n", c->ip,c->port);
    }
    
    //释放客户端对应的查询缓冲区
    sdsfree(c->querybuf);
    c->querybuf = NULL;
    // 清空 WATCH 信息
    unwatchAllKeysCommand(c);
    listRelease(c->watched_keys);

    // 关闭套接字，并从事件处理器中删除该描述符对应的事件
    if (c->fd != -1) {
        aeDeleteFileEvent(server.eventsLoop,c->fd,AE_READABLE);
        aeDeleteFileEvent(server.eventsLoop,c->fd,AE_WRITABLE);
        close(c->fd);
    }
    //如果客户端是从节点
    if (c->flags & KVDATA_SLAVE) {
      
        //关闭从服务器客户端用于接收RDB文件的文件描述符
        if (c->repldbfd != -1) close(c->repldbfd);
        //释放发送给从节点客户端的RDB文件的长度信息
        if (c->replpreamble) sdsfree(c->replpreamble);
        
        //如果客户端也是监视器，则删除监视器列表中对应的客户端节点
        list *l = server.slaves;
        ln = listSearchKey(l,c);
        assert(ln != NULL);
        listDelNode(l,ln);
    }


    // 清空回复缓冲区
    listRelease(c->reply);
    // 清空命令参数
    freeClientArgv(c);
    // 清除参数空间
    zfree(c->argv);
    // 释放客户端名字对象
    if (c->name) decrRefCount(c->name);
    // // 清除事务状态信息
    // freeClientMultiState(c);

    // 从服务器的客户端链表中删除自身
    if (c->fd != -1) {
        ln = listSearchKey(server.clients,c);
        listDelNode(server.clients,ln);
    }
    // 释放客户端 KVClient 结构本身
    // if(c != NULL)
    // zfree(c);
}

/*
 * 清空所有命令参数
 */
void freeClientArgv(KVClient *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}


/*
 * 在客户端执行完命令之后执行：重置客户端以准备执行下个命令
 */ 
void resetClient(KVClient *c) {
    freeClientArgv(c);
    c->multibulklen = 0;
    c->bulklen = -1;
}
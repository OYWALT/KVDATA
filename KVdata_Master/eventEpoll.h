#include "server.h"
#ifndef KVDATA_EVENTEPOLL_H
#define KVDATA_EVENTEPOLL_H
#define MAX_EVENTS 1024  //红黑树句柄最大监听事件数量

typedef struct eventLoop_State {

    // epoll红黑树句柄
    int epfd;
    // 保存满足监听条件的文件事件结构数组
    struct epoll_event *events;

} eventLoop_State;


int aeApiCreate(aeEventLoop *eventLoop);
void init_ListenSocket(aeEventLoop *eventLoop, short port);
void acceptTcpHandler(aeEventLoop *ae, int lfd, void *privdata, int mask);
int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask);
void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask);
int aeEpoll_wait(aeEventLoop *eventLoop, struct timeval *tvp);
int anetTcpNonBlockConnect(char *err, char *addr, int port);
#endif


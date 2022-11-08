#include "eventEpoll.h"
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <stddef.h>
#include "client.h"
#include <string.h>
#include "zmalloc.h"
/*
 * 初始化满足监听条件事件槽空间， 创建 epoll红黑树句柄，建议最大监听事件数为1024
 * 将事件状态（epoll红黑树句柄+满足监听条件事件槽）存入事件处理器的状态结构eventLoop
 */
int aeApiCreate(aeEventLoop *eventLoop) {

    //为事件状态分配空间
    eventLoop_State *state = zmalloc(sizeof(eventLoop_State));
    if (!state) return -1;

    // 初始化满足监听条件事件槽空间,也就是事件处理器的容量
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }

    // 创建 epoll红黑树句柄，建议最大监听事件数为1024
    state->epfd = epoll_create(MAX_EVENTS);
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }

    // 将事件状态（epoll红黑树句柄+满足监听条件事件槽）存入事件处理器的状态结构eventLoop
    eventLoop->apidata = state;
    return 0;
}


/*
 * 初始化服务监听文件描述符，设置为非阻塞状态，将其加入epoll句柄。
 * 并将其与服务器套接字绑定
*/ 
void init_ListenSocket(aeEventLoop *eventLoop, short port)
{
    //初始化服务器的监听文件描述符
    int lfd = socket(AF_INET, SOCK_STREAM, 0);

    //将服务器监听文件描述符设置为非阻塞状态
    if(fcntl(lfd, F_SETFL, O_NONBLOCK)<0)
    printf("fcntl error.\n");
    
    //-------------------------------------------------------------------------------------------------------
    //将监听文件描述符加入到epoll红黑树句柄中进行监听，并设置回调函数为acceptTcpHandler函数
    aeCreateFileEvent(eventLoop, lfd, AE_READABLE,acceptTcpHandler, NULL);

    //初始化服务器套接字
    struct sockaddr_in sin;
    memset(&sin,0,sizeof(sin));//将套接字结构内容清0
    sin.sin_family = AF_INET;//IPv4协议
    sin.sin_addr.s_addr = INADDR_ANY;//初始化服务器IP地址为本机任意IP
    sin.sin_port = htons(port);//初始化服务器端口号
    
    //将服务器监听描述符与服务器套接字绑定
    if(bind(lfd, (struct sockaddr *)&sin, sizeof(sin))<0)
    printf("bind error.\n");
    
    //设置最大待连接客户端数
    if(listen(lfd, 20)<0)
    printf("listen error.\n");
    
    server.listenfd = lfd;
    printf("Listening....\n");
    return;
}


/*
 * 创建一个 TCP 连接处理器
 * 服务器使用
 */
void acceptTcpHandler(aeEventLoop *ae, int lfd, void *privdata, int mask) {

    char ip[20];
    int port;
    struct sockaddr_in cin;//客户端套接字
    socklen_t len = sizeof(cin);//客户端套接字长度
    
    //客户端与服务器建立连接
    int cfd = accept(lfd, (struct sockaddr *)&cin, &len);
    if ((cfd) == -1)
    {
        printf("accept: %s" , strerror(errno));
        return ;
    }
    //获取新连接客户端的ip地址：port端口号
    inet_ntop(AF_INET, (void *)(&cin.sin_addr), ip, 16);
    port = ntohs(cin.sin_port);

    //根据已连接文件描述符创建新的客户端状态
    KVClient *c = createClient(cfd);
    c->ip = ip;
    c->port = port;
    
    //打印建立连接的客户端相关信息
    printf("new connect [%s:%d][time:%ld], cfd[%d], ip[%s], port[%d].\n",
           inet_ntoa(cin.sin_addr), ntohs(cin.sin_port), ae->events[cfd].last_active,cfd,ip,port);
}



/*
 * 根据已注册事件结构数组中文件描述符fd对应的事件类型，以及给定的事件类型掩码mask
 * 将文件描述符fd加入到epoll句柄中进行监听
 */
int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    struct epoll_event epv = {0, {0}};
    int option;

    //获取文件描述符的事件监听类型
    epv.events = 0; //监听的事件类型
    mask |= eventLoop->events[fd].mask; //保留改描述符原先的事件类型
    if (mask & AE_READABLE) epv.events |= EPOLLIN;
    if (mask & AE_WRITABLE) epv.events |= EPOLLOUT;
    epv.data.u64 = 0; //避免警告
    epv.data.fd = fd;

    if (eventLoop->events[fd].status == 1)
    {
        //已经在红黑树里
        option = EPOLL_CTL_MOD;     //修改其属性
    } else
    {
        //不在红黑树里
        option = EPOLL_CTL_ADD;     //将其加入红黑树efd, 并将 status 置 1
        eventLoop->events[fd].status = 1;
    }

    //取出存放epoll红黑树句柄+保存满足监听条件的文件事件结构数组
    eventLoop_State *state = eventLoop->apidata;
    
    //注册事件到红黑树句柄epfd
    if (epoll_ctl(state->epfd, option, fd, &epv) < 0)   //实际添加/修改
    {
         printf("epoll_ctl error.\n");
         return -1;
    }
    eventLoop->events[fd].last_active = time(NULL);
    return 0;
}

/*
 * 将已注册事件结构数组中文件描述符fd删除给定delmask掩码对应的事件
 * 如果删除给定事件后，fd不在监听任何事件，则将该文件描述符从epoll红黑树句柄中移除
 * 否则，即将修改监听事件类型后的文件描述符加入epoll红黑树句柄中
 */
void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask)
{
    //不在红黑树上,则直接返回
    if (eventLoop->events[fd].status != 1)
        return ;

    //将已注册事件结构数组中文件描述符fd删除给定delmask掩码对应的事件
    int mask = eventLoop->events[fd].mask & (~delmask);

    struct epoll_event epv;
    epv.events = 0;
    if (mask & AE_READABLE) epv.events |= EPOLLIN;
    if (mask & AE_WRITABLE) epv.events |= EPOLLOUT;
    epv.data.u64 = 0; /* avoid valgrind warning */
    epv.data.fd = fd;

    //取出存放epoll红黑树句柄+保存满足监听条件的文件事件结构数组
    eventLoop_State *state = eventLoop->apidata;

    if (mask != AE_NONE) {
        //将删除delmask事件类型后的文件描述符加入epoll红黑树句柄中
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&epv);
    } else {
        //如果删除给定事件后，fd不在监听任何事件，则将该文件描述符从epoll红黑树句柄中移除
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&epv);
        eventLoop->events[fd].status=0;

    }

}

/*
 * 阻塞等待红黑树句柄epfd监听的事件中满足监听条件的事件发生
 * 将满足监听条件的文件事件取出对应监听事件类型mask以及文件描述符fd
 * 并加入到事件处理器状态eventLoop 的已就绪文件事件fired 数组中，最后方便根据fired数组去已注册events数组中找处理函数等
 */
int aeEpoll_wait(aeEventLoop *eventLoop, struct timeval *tvp)
{
    int retval, numevents = 0;
    eventLoop_State *state = eventLoop->apidata;

    // 阻塞等待红黑树句柄epfd监听的事件中满足监听条件的事件发生
    // 并将满足监听条件的事件结构存入到state->events数组中
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
                        tvp ? (tvp->tv_sec*1000 + tvp->tv_usec/1000) : -1);

    // 有至少一个事件就绪
    if (retval > 0) {
        int j;

        // 将满足监听条件的文件事件取出对应监听事件类型mask以及文件描述符fd
        // 并加入到事件处理器状态的已就绪文件事件eventLoop->fired 数组中
        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;

            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
        }
    }
    // 返回已就绪事件个数
    return numevents;

}


/*
 * 创建非阻塞 TCP 连接
 * 服务器作为从节点客户端使用
 */
int anetTcpNonBlockConnect(char *err, char *addr, int port)
{
    int cfd;
    cfd=socket(AF_INET,SOCK_STREAM,0);

    //将服务器监听文件描述符设置为非阻塞状态
    if(fcntl(cfd, F_SETFL, O_NONBLOCK)<0)
    printf("fcntl error.\n");

    //初始化服务器的scokaddr_in结构体
    struct sockaddr_in sever_addr;
    memset(&sever_addr,0,sizeof(sever_addr));
    sever_addr.sin_family=AF_INET;


    sever_addr.sin_port=htons(port);

    //将字符串点分法表示的IP地址转换成网络字节序列
    int re=inet_pton(AF_INET,addr,&sever_addr.sin_addr.s_addr);

    //向服务器提出连接请求
    int ret=connect(cfd,(struct sockaddr*)&sever_addr,sizeof(sever_addr));
    
    return cfd;
}







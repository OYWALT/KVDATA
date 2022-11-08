#include <stdio.h>
#include <stdlib.h>
#include "eventEpoll.h"
#define EVENTS_NUM  100     //事件处理器事件槽总数
#define SERV_PORT   6668    //服务器默认端口号


KVServer server;//全局服务器变量

int main(int argc, char *argv[])
{
    //初始化服务器
    initServer(&server);

    //使用用户指定端口.如未指定,用默认端口
    short port = SERV_PORT;
    if (argc == 2)
        port = atoi(argv[1]);


    //打印服务器的端口号
    printf("server running:port[%d]\n", port);
    
    //初始化事件处理器，并创建epoll句柄,设置事件处理器的最大容量为EVENTS_NUM
    server.eventsLoop = aeCreateEventLoop(EVENTS_NUM);

    //初始化服务监听文件描述符，设置为非阻塞状态，将其加入epoll句柄，并将其与服务器套接字绑定。
    init_ListenSocket(server.eventsLoop, port);

    printf("Start the main loop of the event handler to start processing events... ...\n");
    //创建时间事件到事件处理器中
    aeCreateTimeEvent(server.eventsLoop, 5000, serverCron, NULL);

    while(1)
    {
        aeMain(server.eventsLoop);
    }

 return 0;
}

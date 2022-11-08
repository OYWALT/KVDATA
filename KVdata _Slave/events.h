#ifndef KVDATA_EVENTS_H
#define KVDATA_EVENTS_H

#include <sys/time.h>
/*事件执行状态*/
#define AE_OK 0   // 成功
#define AE_ERR -1 // 出错

/*文件事件状态*/
#define AE_NONE     0   // 未设置
#define AE_READABLE 1   // 读事件
#define AE_WRITABLE 2   // 写事件

/*时间处理器的执行 flags*/
#define AE_FILE_EVENTS 1 // 文件事件
#define AE_TIME_EVENTS 2 // 时间事件
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS) // 所有事件
#define AE_DONT_WAIT 4  // 不阻塞，也不进行等待

/*决定时间事件是否要持续执行的 flag */
#define AE_TIMECIRCLE -1

#define KVDATA_IOBUF_LEN  (1024*16)  //默认命令读入长度

struct aeEventLoop;//【此处要声明一下结构体，因为是在后面才定义的，定义之前要使用便需要声明】

/*事件接口*/
//文件事件处理函数
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
//时间事件处理函数
typedef int aeTimeProc(struct aeEventLoop *eventLoop, void *clientData);

/*
 * 已注册文件事件结构
 */
typedef struct aeFileEvent {

    // 监听事件类型掩码，
    // 值可以是 AE_READABLE 或 AE_WRITABLE 或 AE_READABLE | AE_WRITABLE
    int mask;

    //是否在监听:1->在红黑树上(监听), 0->不在(不监听)
    int status;

    // 读事件处理器
    aeFileProc *rfileProc;

    // 写事件处理器
    aeFileProc *wfileProc;

    // 多路复用的私有数据,【待用】
    void *clientData;    

    //记录每次加入红黑树句柄 epfd 的时间
    time_t last_active;

} aeFileEvent;

/*
 * 已就绪事件结构
 */
typedef struct aeFiredEvent {

    // 已就绪文件描述符
    int fd;

    // 事件类型掩码，
    // 值可以是 AE_READABLE 或 AE_WRITABLE 或 AE_READABLE | AE_WRITABLE
    int mask;

} aeFiredEvent;

/*
 * 时间事件结构
 */
typedef struct aeTimeEvent {

    // 时间事件的唯一标识符
    long long id;

    // 事件的到达时间
    long when_sec; // s 
    long when_ms;  // ms 

    // 事件处理函数
    aeTimeProc *timeProc;

    // 指向下个时间事件结构，形成链表
    struct aeTimeEvent *next;

    // 多路复用库的私有数据
    void *clientData;

} aeTimeEvent;

typedef struct aeEventLoop{
    // 目前已注册的最大描述符
    int maxfd;   

    // 事件处理器当前容量
    int setsize;

    // 用于生成时间事件 id
    long long timeEventNextId;

    // 最近一次执行时间事件的时间（已执行）
    time_t lastTime;    

    // 已注册的文件事件
    aeFileEvent *events; 

    // 已就绪的文件事件
    aeFiredEvent *fired;

    // 时间事件链表（时间事件采用头插法）
    aeTimeEvent *timeEventHead;

    // 事件处理器的开关
    int stop;

    // 多路复用的私有数据（存储监听事件状态结构：红黑树句柄epfd+满足监听条件文件事件数组）
    void *apidata;

} aeEventLoop;

aeEventLoop *aeCreateEventLoop(int setsize);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
void recvData(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
void aeMain(aeEventLoop *eventLoop);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop);
void aeGetTime(long *seconds, long *milliseconds);
int processTimeEvents(aeEventLoop *eventLoop);
void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds, aeTimeProc *proc, void *clientData);
#endif
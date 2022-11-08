#include <stdio.h>
#include "eventEpoll.h"
#include <time.h>
#include "zmalloc.h"
#include "client.h"
#include "errno.h"
#include <string.h>
#include <unistd.h>


#include <poll.h>
#include<sys/socket.h>
#include <ctype.h> 
#include <string.h>
#include <unistd.h>
#include "util.h"

/*
 * 初始化事件处理器eventLoop
 */
aeEventLoop *aeCreateEventLoop(int setsize)
{
    aeEventLoop *eventLoop = zmalloc(sizeof(aeEventLoop));
    if(eventLoop == NULL)
        return NULL;
    // 初始化事件处理器的开关
    eventLoop->stop = 0;
    // 初始化目前已注册的最大文件事件描述符
    eventLoop->maxfd = -1;

    // 为事件处理器分配空间
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;

    // 为已注册文件事件结构和已就绪文件事件结构数组分配空间
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*setsize);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*setsize);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;

    // 设置事件处理器的容量，即文件事件数组的大小
    eventLoop->setsize = setsize;

    // 初始化执行最近一次执行时间事件的时间
    eventLoop->lastTime = time(NULL);

    // 初始化时间事件链表，以及时间事件id
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;

    //初始化满足监听条件事件槽空间， 创建 epoll红黑树句柄，建议最大监听事件数为1024
    //将事件状态（epoll红黑树句柄+满足监听条件事件槽）存入事件处理器的状态结构eventLoop
    if (aeApiCreate(eventLoop) == -1) goto err;

    // 初始化已注册文件事件监听事件为空
    for (int i = 0; i < setsize; i++)
        eventLoop->events[i].mask = AE_NONE;//无设置

    // 返回事件处理器
    return eventLoop;

    //错误处理
    //释放所有已分配空间
    err:
    if (eventLoop) {
        printf("aeCreateEventLoop error.\n");
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);

    }
    return NULL;
}


/*
 * 事件监听类型mask，事件处理函数proc，文件描述符fd、事件私有数据clientData
 * （1）将文件描述符fd对应的事件信息添加到已注册文件事件数组eventLoop->events[fd]中.
 * （2）将文件描述符fd以及对应事件mask加入到红黑树句柄epfd中进行监听。
 */
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask, aeFileProc *proc, void *clientData)
{
    //文件描述符fd超出槽大小，返回错误，并设置errno
    if (fd >= eventLoop->setsize) {
        return AE_ERR;
    }
    // 取出已注册文件事件数组eventLoop->events中文件描述fd对应下标的元素指针
    aeFileEvent *fe = &eventLoop->events[fd];

    //监听指定 fd 的指定事件
    //将文件描述符fd以及其对应的事件结构加入到epoll句柄中
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    
    // 设置fd监听的事件类型，以及事件的处理器的回调函数
    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;

    // 私有数据，将包含fd文件描述符详细信息放在fd事件结构的私有数据中
    // 在已注册文件事件结构中关联已连接描述符对应的客户端c
    fe->clientData = clientData;

    //更新每次将文件描述符加入/修改到红黑树柄中的时间
    fe->last_active = time(NULL);
    // 如果有需要，更新事件处理器的最大fd
    
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;

    return AE_OK;
}

/*
 * （1）将已注册文件事件数组eventLoop->events[fd]中对应的mask监听事件移除
 * （2）取消对给定文件描述符fd 的mask事件类型的监视
 */
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;

    // 取出文件事件结构
    aeFileEvent *fe = &eventLoop->events[fd];

    // 未设置监听的事件类型，直接返回
    if (fe->mask == AE_NONE) return;

    // 计算新掩码，将文件事件结构中，文件描述符fd对应的mask监听事件删除
    fe->mask = fe->mask & (~mask);

    // 根据移除监听事件的类型，将对应的事件处理函数清空
    if (mask & AE_READABLE) fe->rfileProc = NULL;
    if (mask & AE_WRITABLE) fe->wfileProc = NULL;

    // 如果修改监听事件的文件描述符是事件处理器中已注册的最大文件秒描述符
    // 且该文件描述符对应的监听事件在取出指定监听事件mask后，无监听事件
    // 则更新已注册的最大文件描述符
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;
    }

    // 取消红黑树句柄epfd对给定文件描述符fd的mask事件类型的监视
    aeApiDelEvent(eventLoop, fd, mask);
}

/*
 * 从客户端中读取输入命令，并将其保存在查询缓冲区中
 */
void recvData(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask)
{
    KVClient *c = (KVClient*) clientData;

    int nread, readlen;
    size_t qblen;
    KVDATA_NOTUSED(eventLoop);
    KVDATA_NOTUSED(mask);

    // 获取查询缓冲区当前内容的长度
    // 如果读取出现 short read ，那么可能会有内容滞留在读取缓冲区里面,这些滞留内容也许不能完整构成一个符合协议的命令，
    qblen = sdslen(c->querybuf);
   
    // 为查询缓冲区分配空间，确保至少会有readlen+1的空闲空间
    c->querybuf = sdsMakeRoomFor(c->querybuf, KVDATA_IOBUF_LEN);

    // 读入命令内容到查询缓存
    nread = read(fd, c->querybuf+qblen, KVDATA_IOBUF_LEN);
    // 读入出错
    if (nread == -1) {
        if (errno == EAGAIN) {
            //errno == EAGAIN当前不可读写，需要继续重试
            printf("Reading from client error: %s\n",strerror(errno));
            nread = 0;
        } else {
            printf("Reading from client error: %s\n",strerror(errno));
            //读入出错，释放客户端
            freeClient(c);
            return;
        }
    // 遇到 EOF，读取结束，释放客户端
    } else if (nread == 0) {
        printf("Client closed connection, read 0 word.\n");
        freeClient(c);
        return;
    }

    if (nread) {
        // 根据内容，更新查询缓冲区（SDS） free 和 len 属性
        // 并将 '\0' 正确地放到内容的最后
        sdsIncrLen(c->querybuf,nread);
        // 记录服务器和客户端最后一次互动的时间
        c->lastinteraction = server.unixtime;
        // 如果客户端是主服务器 master 的话，更新它的复制偏移量
        if (c->flags & KVDATA_MASTER) c->reploff += nread;

    } 
    // 函数会执行到缓存中的所有内容都被处理完为止
    processInputBuffer(c);

}


/*
 * 开启事件处理器的主循环，开始处理事件
 */
void aeMain(aeEventLoop *eventLoop) {

    //开启事件处理器
    eventLoop->stop = 0;

    while (!eventLoop->stop) {
        // 开始处理事件
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}


/*
 * 处理所有已到达的时间事件，以及所有已就绪的文件事件。
 *
 * 如果不传入特殊 flags 的话，那么函数睡眠直到文件事件就绪，或者下个时间事件到达（如果有的话）。
 *
 * 如果 flags 为 0 ，那么函数不作动作，直接返回。
 * 如果 flags 包含 AE_ALL_EVENTS ，所有类型的事件都会被处理。
 * 如果 flags 包含 AE_FILE_EVENTS ，那么处理文件事件。
 * 如果 flags 包含 AE_TIME_EVENTS ，那么处理时间事件。
 *
 * 函数的返回值为已处理事件的数量
 */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    //没有时间事件且没有文件事件需要处接理，直退出
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    //判断事件槽中存在已注册事件，或存在时间事件，且可以阻塞监听文件事件
    if (eventLoop->maxfd != -1 || ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        //定义一个最近要到达的时间事件
        aeTimeEvent *shortest = NULL;

        //之所以定义一个变量，一个指针是因为：
        //用tv变量去分别获得最近时间事件的s和ms，如果用指针，需要分配内存后才能去承接这两个值，造成频繁分配堆上的内存，还得手动释放，麻烦
        struct timeval tv, *tvp;

        // 如果需要处理时间事件，且可以阻塞处理文件事件
        // 则获取最近的时间事件
        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest) {
            // 如果最近时间事件存在的话
            // 那么根据最近可执行时间事件和现在时间的时间差来决定文件事件的阻塞时间

            // 计算距今最近的时间事件还要多久才能达到
            // 并将该时间距保存在 tv 结构中
            long now_sec, now_ms;
            aeGetTime(&now_sec, &now_ms);//取出当前时间的秒和毫秒，

            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;            
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms+1000) - now_ms)*1000;
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms)*1000;
            }

            // 时间差小于 0 ，说明事件已经可以执行了，将秒和毫秒设为 0 （不阻塞）
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {

            // 执行到这一步，说明没有时间事件
            // 那么根据 AE_DONT_WAIT 是否设置来决定是否阻塞，以及阻塞的时间长度

            if (flags & AE_DONT_WAIT) {
                // 设置文件事件不阻塞
                tv.tv_sec = tv.tv_usec = 0;
                tvp = &tv;
            } else {
                // 没有时间事件，且没有设置执行完非阻塞文件事件立即退出的AE_DONT_WAIT标志
                // 文件事件可以阻塞直到有事件到达为止
                tvp = NULL;
            }
        }
       
        // 处理文件事件，阻塞时间由 tvp 决定
        numevents = aeEpoll_wait(eventLoop, tvp);

        for (j = 0; j < numevents; j++) {
            
            // 从已就绪数组中获取事件
            aeFileEvent *fe = &eventLoop->events[eventLoop->fired[j].fd];
            
            //获取已就绪事件eventLoop->fired[j]的事件类型
            int mask = eventLoop->fired[j].mask;
            //获取已就绪事件eventLoop->fired[j]的文件描述符
            int fd = eventLoop->fired[j].fd;
            
            int rfired = 0;//一个标志位
         
            // 当读事件和写事件同时满足监听条件时，先处理读事件，再处理写事件
            // 读事件
            if (fe->mask & mask & AE_READABLE) {
               
                // rfired 确保读/写事件只能执行其中一个
                rfired = 1;
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
            }
            // 写事件
            if (fe->mask & mask & AE_WRITABLE) {
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
            }
            processed++;
        }
    }

    // 执行时间事件
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    //返回已处理的时间事件和文件事件总和
    return processed;
}


/*
 * 寻找里目前时间最近的时间事件
 * 因为链表是乱序的，所以查找复杂度为 O（N）
*/
aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;
    //遍历时间事件链表，寻找下一个即将执行的时间事件（最近的时间事件）
    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
            (te->when_sec == nearest->when_sec &&
             te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/*
 * 取出当前时间的秒和毫秒，
 * 并分别将它们保存到 seconds 和 milliseconds 参数中
 */
void aeGetTime(long *seconds, long *milliseconds)
{
    struct timeval tv;
    //gettimeofday函数会将当时的时间转换成s和ms的形式放到timeval结构中
    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec/1000;
}

/*
 * 处理所有已到达的时间事件
 * 返回处理时间事件的数量
 */
int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;
    time_t now = time(NULL);

    // 当上一次执行时间事件的时间>now，则将所有时间事件的执行事件设置为now
    // 通过重置事件的运行时间，
    // 防止因时间穿插（skew）而造成的事件处理混乱
    if (now < eventLoop->lastTime) {
        te = eventLoop->timeEventHead;
        while(te) {
            te->when_sec = 0;
            te = te->next;
        }
    }

    // 更新最后一次处理时间事件的时间
    eventLoop->lastTime = now;

    // 遍历链表
    // 执行那些已经到达的事件
    te = eventLoop->timeEventHead;

    while(te) {
        long now_sec, now_ms;

        // 获取当前时间
        aeGetTime(&now_sec, &now_ms);

        // 如果当前时间等于或等于事件的执行时间，那么说明事件已到达，执行这个事件
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;
            // 执行事件处理器，并获取返回值
            retval = te->timeProc(eventLoop, te->clientData);
            processed++;

            // 记录是否有需要循环执行这个事件时间
            if (retval != AE_TIMECIRCLE) {
                // 是的， retval 毫秒之后继续执行这个时间事件
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                // 不，将这个事件删除
                aeDeleteTimeEvent(eventLoop, te->id);
            }

            // 因为执行事件之后，事件列表可能已经被改变了
            // 因此需要将 te 放回表头，继续开始执行事件
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    }
    return processed;
}

/*
 * 在当前时间上加上 milliseconds 毫秒，
 * 并且将加上之后的秒数和毫秒数分别保存在 sec 和 ms 指针中。
 */
void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    // 获取当前时间
    aeGetTime(&cur_sec, &cur_ms);

    // 计算增加 milliseconds 之后的秒数和毫秒数
    when_sec = cur_sec + milliseconds/1000;
    when_ms = cur_ms + milliseconds%1000;

    // 进位：
    // 如果 when_ms 大于等于 1000
    // 那么将 when_sec 增大一秒
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }

    // 保存到指针中
    *sec = when_sec;
    *ms = when_ms;
}

/*
 * 删除给定 id 的时间事件
 * 成功返回AE_OK，失败返回AE_ERR
 */
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te, *prev = NULL;

    // 遍历时间事件链表
    te = eventLoop->timeEventHead;
    while(te) {

        // 发现目标事件，删除
        if (te->id == id) {

            //如果目标事件是表头位置，更新表头
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
                //如果目标事件不是表头，连接该节点前后
            else
                prev->next = te->next;

            // 释放时间事件
            zfree(te);

            return AE_OK;
        }
        prev = te;
        te = te->next;
    }
    return AE_ERR; /* 没有找到具有指定ID的时间事件 */
}

/*
 * 创建时间事件，并设置事件启动事件，回调函数等
 * 返回时间事件id
 */
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
                            aeTimeProc *proc, void *clientData)
{
    // 更新时间计数器
    long long id = eventLoop->timeEventNextId++;

    // 创建时间事件结构
    aeTimeEvent *te = zmalloc(sizeof(aeTimeEvent));
    if (te == NULL)
        return AE_ERR;
    // 设置时间事件 ID
    te->id = id;

    // 设定处理事件的时间
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    // 设置事件处理器
    te->timeProc = proc;
    // 设置私有数据
    te->clientData = clientData;
    // 将新事件放入时间事件表头【头插法】
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;

    //返回时间事件id
    return id;
}



/*--------------------------------【重点观察】---------------------------------------------------*/

/*
 * 阻塞milliseconds时间等待fd变成可读、可写或异常、或发生挂起
 */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    //存在监听的四种情况中的一个，返回事件类型掩码retmask
    //否则返回retval（0－超时，-1－出错）
    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;//可读
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;//可写
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;//异常
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;//发生挂起
        return retmask;
    } else {
        return retval;
    }
}
/*
 * 从ptr中读取指定的字节数写入到'fd'中
 * 如果在'timeout'毫秒内写入了所有字节，则操作成功并返回写入长度。
 * 否则操作失败，返回-1，并且可以对文件描述符执行未指定的部分写操作。
 */
ssize_t syncWrite(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nwritten, ret = size;
    long long start = mstime();
    long long remaining = timeout;
    while(1) {
        //阻塞时间最短KVDATA_SYNCIO_RESOLUTIONms
        long long wait = (remaining > 10) ?
                          remaining : 10;
        long long elapsed;

        /* Optimistically try to write before checking if the file descriptor
         * is actually writable. At worst we get EAGAIN. */
        nwritten = write(fd,ptr,size);
        if (nwritten == -1) {
            if (errno != EAGAIN) return -1;
        } else {
            ptr += nwritten;
            size -= nwritten;
        }
        if (size == 0) return ret;

        /* Wait */
        aeWait(fd,AE_WRITABLE,wait);
        elapsed = mstime() - start;
        if (elapsed >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining = timeout - elapsed;
    }
}
/*
 * 从'fd'中读取指定的字节数到ptr。
 * 如果在'timeout'毫秒内读取了所有字节，则操作成功并返回'size'。
 * 否则操作失败，返回-1，并且可以从文件描述符读取未指定数量的数据。
 */
ssize_t syncRead(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nread, totread = 0;
    long long start = mstime();
    long long remaining = timeout;

    if (size == 0) return 0;
    while(1) {
        long long wait = (remaining > 10) ?
                          remaining : 10;
        long long elapsed;

        /* Optimistically try to read before checking if the file descriptor
         * is actually readable. At worst we get EAGAIN. */
        nread = read(fd,ptr,size);
        if (nread == 0) return -1; /* short read. */
        if (nread == -1) {
            if (errno != EAGAIN) return -1;
        } else {
            ptr += nread;
            size -= nread;
            totread += nread;
        }
        if (size == 0) return totread;

        /* Wait */
        aeWait(fd,AE_READABLE,wait);
        elapsed = mstime() - start;
        if (elapsed >= timeout) {
            errno = ETIMEDOUT;
            return -1;
        }
        remaining = timeout - elapsed;
    }
}
/*
 * 在规定时间timeout内，从fd中读取一行内容到ptr，（ptr末尾不带/r）
 * 返回读取到内容的长度
 * */
ssize_t syncReadLine(int fd, char *ptr, ssize_t size, long long timeout) {
    ssize_t nread = 0;
    size--;
    while(size) {
        char c;
        //在规定timeout时间内将fd内的1个字节读到c中
        if (syncRead(fd,&c,1,timeout) == -1) return -1;
        //当读取到换行符时，根据情况在ptr末尾加上结束符'\0'
        if (c == '\n') {
            //情况一
            *ptr = '\0';
            //情况二
            if (nread && *(ptr-1) == '\r') *(ptr-1) = '\0';
            return nread;
        } else {
            *ptr++ = c;
            *ptr = '\0';
            nread++;
        }
    }
    return nread;
}

char *sendSynchronousCommand(int fd, char* cmd) {
    char buf[256];
    // 发送命令到主服务器
    if (syncWrite(fd,cmd,strlen(cmd),5*1000) == -1) {
        return sdscatprintf(sdsnewlen("",0),"-Writing to master: %s",
                strerror(errno));
    }
    // 从主服务器中读取回复
    if (syncReadLine(fd,buf,sizeof(buf), 5*1000) == -1)
    {   
        return sdscatprintf(sdsnewlen("",0),"-Reading from master: %s",
                strerror(errno));
    }
    return sdsnew(buf);
}






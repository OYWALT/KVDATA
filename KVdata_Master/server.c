#include "server.h"
#include "util.h"
#include <string.h>
#include <time.h>
#include "assert.h"
#include "zmalloc.h"
#include "dict.h"
#include "multi.h"
#include <errno.h>
#include <unistd.h> 
#include <sys/wait.h>
#include "multi.h"
#include "slave.h"
#include "rdb.h"
extern struct sharedObjectsStruct shared;

/*------------------------不同类型字典对应的键值释放函数以及哈希函数算法-----------------------------------------*/
//命令字典，哈希函数以及值释放函数
dictType commandTableDictType = {
    dictSdsCaseHash,           /* 哈希函数 */
    NULL                       /* 值释放函数 */
};
//正常数据库键值对字典，哈希函数以及值释放函数
dictType dbDictType = {
    dictSdsHash,                /* 哈希函数 */
    dictKVDATAObjectDestructor   /* 值释放函数 */
};
//过期键字典，哈希函数以及值释放函数
dictType expiresDictType = {
    dictSdsHash,               /* 哈希函数 */
    NULL                       /* 值释放函数 */
};
//被监视的键字典，哈希函数以及值释放函数
dictType clientDictType = {
    dictObjHash,                /* 哈希函数 */
    dictListDestructor          /* 值释放函数 */
};


/* 
 * 命令表
 * 表中的每个项都由以下域组成：
 * name: 命令的名字
 * proc: 一个指向命令的实现函数的指针
 * counts: 参数的数量。可以用 -N 表示 >= N 
 * len: 命令名字的长度
 */
struct KVDataCommand KVDATACommandTable[] = {
    {"set",setCommand,3,3}, //SET KEY VALUE
    {"get",getCommand,2,3}, //get KEY
    {"tset",setCommand,5,4}, //TSET KEY VALUE [ss/ms] [expire]
    {"multi",multiCommand,1,5},  //MULTI
    {"exec",execCommand,1,4},    //EXEC
    {"watch",watchCommand,1,5},  //WATCH KEY1
    {"unwatchkeys",unwatchAllKeysCommand,1,11},//UNWATCHKEYS
    {"discard",unwatchAllKeysCommand,1,7},//UNWATCHKEYS
    {"save",saveCommand,1,4},//SAVE
    {"load",loadCommand,1,4},//LOAD
    {"slaveof",slaveofCommand,3,7},//SLAVEOF ip port
    {"psync",syncCommand,3,5},//PSYNC runid offset
    {"ping",pingCommand,1,4}//PING
};

void initCommand(dict*command)
{
    for(int i=0;i<13;i++)
    { 
        sds name = sdsnewlen((KVDATACommandTable+i)->name, (KVDATACommandTable+i)->len);      
        if(dictAdd(command, name, KVDATACommandTable+i)==DICT_ERR)
        printf("initCommand ERR\n");
        //printf("Command name %s\n",name);
    }
}

void initServer(KVServer *server)
{
    // 设置服务器的运行 ID
    getRandomHexChars(server->serverid,KVDATA_RUN_ID_SIZE);
    //设置默认服务器频率,触发时间事件用的
    server->hz = 10;
    //初始化共享对象
    createSharedObjects();
    //创建客户端链表
    server->clients=listCreate();
    //更新服务器全局状态下的unix时间的缓存值
    updateCachedTime();
    //创建命令字典
    server->commands = dictCreate(&commandTableDictType);
    initCommand(server->commands);
    /*--------------------------------持久化相关参数初始化--------------------------------*/
    //初始化RDB默认文件名
    server->rdb_filename = "dump.rdb";
    /*--------------------------------数据库初始化--------------------------------*/
    //初始化数据库数量
    server->dbnum = DB_NUM;
    //为数据库分配空间
    server->db = zmalloc(sizeof(KVdataDb)*server->dbnum);
    // 创建并初始化数据库结构
    for (int j = 0; j < server->dbnum; j++) {
        server->db[j].DB = dictCreate(&dbDictType);
        server->db[j].expires = dictCreate(&expiresDictType);
        server->db[j].watched_keys = dictCreate(&clientDictType);
        server->db[j].id = j;
    } 
    /*--------------------------------主从复制初始化--------------------------------*/
    server->rdb_child_pid = -1;
    //创建从服务器链表
    server->slaves=listCreate();
    return ;
}

/* 我们使用全局状态下的unix时间的缓存值，
 * 因为有了虚拟内存和老化，在每次对象访问时都将当前时间存储在对象中，
 * 不需要准确性。访问全局变量要比调用时间(NULL)快得多*/
void updateCachedTime(void) {
    server.unixtime = time(NULL);
}

/*
 * 服务器的时间事件
 */
int serverCron(struct aeEventLoop *eventLoop, void *clientData)
{
    KVDATA_NOTUSED(eventLoop);
    KVDATA_NOTUSED(clientData);
    int pid;
    int exitStatus;
    //更新服务器全局状态下的unix时间的缓存值
    updateCachedTime();
    
    //判断是否存在backgroundRDB子进程结束
    if(server.rdb_child_pid != -1)
    {
        //等待子进程结束（不阻塞）
        if((pid = waitpid(-1,&exitStatus,WNOHANG))!=0){
            int exitcode = WEXITSTATUS(exitStatus);
            int signal = WTERMSIG(exitStatus);
            //后台RDB结束
            if(pid==server.rdb_child_pid){
                backgroundSaveDoneHandler(exitcode,signal);
                printf("Successfully RDB bgsave.\n");

            //结束进程id与RDB子进程不一致
            }else{
            printf(" Warning, detected child with unmatched pid: %ld.\n",(long)pid);
            }
            pid = 0;
        }  
    }
    // 重连接主服务器
    replicationCron();
    return 5000;
}


/*
 * 修剪查询缓冲区
 * 如果在读入协议内容时，发现内容不符合协议，那么异步地关闭这个客户端。【只是给了关闭的标志，并没有执行关闭】
 */
void setProtocolError(KVClient *c, int pos) {

    printf("Protocol error from client: %d\n",c->fd);
    //KVDATA_CLOSE_AFTER_REPLY表示有用户对这个客户端执行了CLIENT KILL命令，
    // 或者客户端发送给服务器的命令请求中包含了错误的协议内容。
    // 服务器会将客户端积存在输出缓冲区中的所有内容发送给客户端，然后关闭客户端。
    c->flags |= KVDATA_CLOSE_AFTER_REPLY;
    //删除输入缓冲区中区间之外的内容
    sdsrange(c->querybuf,pos,-1);
}


/* 
 * 处理客户端输入的命令内容，将其解析后执行命令
 */
void processInputBuffer(KVClient *c) {
    // 尽可能地处理查询缓冲区中的内容
    // 如果读取出现 short read ，那么可能会有内容滞留在读取缓冲区里面
    // 这些滞留内容也许不能完整构成一个符合协议的命令，
    // 需要等待下次读事件的就绪
    while(sdslen(c->querybuf)>2) {
        // 表示有用户对这个客户端执行了CLIENT KILL命令，
        // 或者客户端发送给服务器的命令请求中包含了错误的协议内容，没有必要处理命令了
        if (c->flags & KVDATA_CLOSE_AFTER_REPLY) return;
        // 将client的querybuf中的协议内容转换为client的参数列表中的对象
        processMultibulkBuffer(c);
        
        for(int i=0;i<c->argc;i++)
        printf("NO.[%d]: %s  \n",i,(char *)c->argv[i]->ptr);
        //多条查询可以看到参数长度<=0的情况
        //当参数个数为0，直接重置客户端，无需执行命令
        if (c->argc == 0) {
            resetClient(c);
        } else {
            // 执行命令，并重置客户端
            if (processCommand(c) == AE_OK)
                resetClient(c);
        }
    }
}


/* 
 * 将客户端命令输入缓冲区 c->querybuf 中的协议内容转换成 c->argv 中的参数对象
 *
 * 比如 *3\r\n$3\r\nSET\r\n$3\r\nMSG\r\n$5\r\nHELLO\r\n
 * 将被转换为：
 * argv[0] = SET
 * argv[1] = MSG
 * argv[2] = HELLO
 */
int processMultibulkBuffer(KVClient *c) {
    char *newline = NULL;
    int pos = 0, ok;
    long long ll;

    // 读入命令的参数个数
    // 比如 *3\r\n$3\r\nSET\r\n... 将令 c->multibulklen = 3
    if (c->multibulklen == 0) {
        //确定目前客户端的参数个数初始化为0
        assert(c->argc == 0);
        // 检查缓冲区的内容第一个 "\r\n"
        newline = strchr(c->querybuf,'\r');
        // 参数数量之后的位置
        // 比如对于 *3\r\n$3\r\n$SET\r\n... 来说，
        // pos 指向 *3\r\n$3\r\n$SET\r\n...
        //            ^
        //            |
        //         newline
        //如果找不到第一个 "\r\n"
        if (newline == NULL) {
            //报告错误，内容不符合协议
            printf("Protocol error: too big mbulk count string\n");
            //如果在读入协议内容时，发现内容不符合协议，那么异步地关闭这个客户端。
            setProtocolError(c,0);
            return AE_ERR;
        }

        // 协议的第一个字符必须是 '*'
        assert(c->querybuf[0] == '*');

        // 将参数个数，也即是 * 之后， \r\n 之前的数字取出并保存到 ll 中
        // 比如对于 *3\r\n ，那么 ll 将等于 3
        ok = string2ll(c->querybuf+1,newline-(c->querybuf+1),&ll);

        // 参数个数转换失败，或参数的数量超出限制
        if (!ok) {
            //报告错误，内容不符合协议
            printf("Protocol error: invalid multibulk length.\n");
            //如果在读入协议内容时，发现内容不符合协议，那么异步地关闭这个客户端。
            setProtocolError(c,pos);
            return AE_ERR;
        }

        // 设置该条命令的参数数量
        c->multibulklen = ll;
        
        // 根据参数数量，为各个参数对象分配空间
        if (c->argv) zfree(c->argv);
        c->argv = zmalloc(sizeof(robj*)*c->multibulklen);

        // 参数数量之后的位置
        // 比如对于 *3\r\n$3\r\n$SET\r\n... 来说，
        // pos 指向 *3\r\n$3\r\n$SET\r\n...
        //                ^
        //                |
        //               pos
        pos = (newline-c->querybuf)+2;  
    }
   //确定当前命令的参数数量>0
    assert(c->multibulklen > 0);

    // 从 c->querybuf 中读入参数，并创建各个参数对象到 c->argv
    while(c->multibulklen) {
        // 读入参数长度
        if (c->bulklen == -1) {
        // 比如对于 *3\r\n$3\r\n$SET\r\n... 来说，
        // pos 指向 *3\r\n$3\r\n$SET\r\n...
        //                   ^
        //                   |
        //                newline
            newline = strchr(c->querybuf+pos,'\r');
            // 确保 "\r\n" 存在
            if (newline == NULL) {
                //报告错误，内容不符合协议
                printf("Protocol error: invalid bulklen length.\n");
                //如果在读入协议内容时，发现内容不符合协议，那么异步地关闭这个客户端。
                setProtocolError(c,pos);
                return AE_ERR;
            }
            // 确保协议符合参数格式，检查其中的 $...
            // 比如 $3\r\nSET\r\n
            if (c->querybuf[pos] != '$') {
                printf("Protocol error: expected '$', got '%c'\n", c->querybuf[pos]);
                setProtocolError(c,pos);
                return AE_ERR;
            }

            // 读取长度
            // 比如 $3\r\nSET\r\n 将会让 ll 的值设置 3
            ok = string2ll(c->querybuf+pos+1,newline-(c->querybuf+pos+1),&ll);
            if (!ok) {
                printf("Protocol error: invalid bulk length\n");
                setProtocolError(c,pos);
                return AE_ERR;
            }
            // 定位到参数的开头
            // 比如 
            // $3\r\nSET\r\n... 
            //       ^
            //       |
            //      pos
            pos += newline-(c->querybuf+pos)+2;
            
            // 参数的长度
            c->bulklen = ll;

            if(c->execlen == 0)
            c->execlen = c->bulklen;
        }
       
        // 读入参数
        // 为参数创建字符串对象      
        c->argv[c->argc++] = createStringObject(c->querybuf+pos,c->bulklen);
        //将游标后移
        pos += c->bulklen+2;
        
        // 清空参数长度
        c->bulklen = -1;

        // 减少还需读入的参数个数
        c->multibulklen--;
    }
    // 从 querybuf 中删除已被读取的内容
    if (pos) sdsrange(c->querybuf,pos,-1);

    // 如果本条命令的所有参数都已读取完，那么返回
    if (c->multibulklen == 0) return AE_OK;
      
    // 如果还有参数未读取完，那么就协议内容有错
    return AE_ERR;
}


/* 
 * 这个函数执行时，我们已经读入了一个完整的命令到客户端，
 * 这个函数负责执行这个命令，
 * 或者服务器准备从客户端中进行一次读取。
 *
 * 如果这个函数返回 1 ，那么表示客户端在执行命令之后仍然存在，
 * 调用者可以继续执行其他操作。
 * 否则，如果这个函数返回 0 ，那么表示客户端已经被销毁。
 */
int processCommand(KVClient *c) {
    // 如果执行命令伪quit命令，特别处理 quit 命令
    // strcasecmp进行字符串比较时会自动忽略大小写
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        printf("QUIT\n");

        //表示有用户对这个客户端执行了CLIENT KILL命令
        c->flags |= KVDATA_CLOSE_AFTER_REPLY;
        return AE_ERR;
    }
    sds name = sdsnewlen((char*)c->argv[0]->ptr, c->execlen);
    c->execlen = 0;
    // 查找命令，并进行命令合法性检查，以及命令参数个数检查
    c->cmd = c->lastcmd = lookupCommand(name);
    //查找命令出错
    if (!c->cmd) {
        // 没找到指定的命令，如果客户端正在执行事务，则事务执行将失败
        flagTransaction(c);
        printf("unknown command '%s'\n", name);
        addReply(c,shared.syntaxerr);
        return AE_OK;

    //参数个数错误
    } else if ((c->cmd->counts > 0 && c->cmd->counts != c->argc) ||
               (c->argc < c->cmd->counts)) {
        // 参数个数错误，如果客户端正在执行事务，则事务执行将失败
        flagTransaction(c);
        printf("wrong number of arguments for '%s' command", c->cmd->name);
        return AE_OK;
    }
    /* 避开事务状态下需要立即执行的命令 */
    if (c->flags & KVDATA_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand)
    {
        // 在事务上下文中
        // 除 EXEC 、 DISCARD 、 MULTI 和 WATCH 命令之外
        // 其他所有命令都会被入队到事务队列中   
        queueMultiCommand(c);
        addReply(c,shared.queued);
        printf("QUEUE\n");
    } else {
        // 执行命令
        call(c,0);
    }
    return AE_OK;
}

/*
 * 调用命令的实现函数，执行命令
 */ 
void call(KVClient *c, int flags) {
    // start 记录命令开始执行的时间
    long long dirty, start, duration;
    // 执行实现函数
    c->cmd->proc(c);
}



/*---------------------------------------回复客户端处理函数---------------------------------------*/
/*
 * 这个函数在每次向客户端发送数据时都会被调用。函数的行为如下：
 *
 * 当客户端可以接收新数据时（通常情况下都是这样），函数返回 AE_OK ，
 * 并将写处理器（write handler）安装到事件循环中，
 * 这样当套接字可写时，新数据就会被写入。
 *
 * 对于那些不应该接收新数据的客户端，
 * 比如伪客户端、 master 以及 未 ONLINE 的 slave ，
 * 或者写处理器安装失败时，
 * 函数返回 AE_ERR 。
 *
 * 通常在每个回复被创建时调用，如果函数返回 AE_ERR ，
 * 那么没有数据会被追加到输出缓冲区。
 */
int prepareClientToWrite(KVClient *c) {
   
    // 客户端是主服务器, 从服务器需要向主服务器发送REPLICATION ACK命令
    // 在发送这个命令之前必须打开对应客户端的KVDATA_MASTER_FORCE_REPLY标志
    // 否则发送操作会被拒绝执行。
    if ((c->flags & KVDATA_MASTER) &&
        !(c->flags & KVDATA_MASTER_FORCE_REPLY)) return AE_ERR;

    // 无连接的伪客户端总是不可写的
    if (c->fd <= 0) return AE_ERR;

    // 一般情况，为客户端套接字安装写处理器到事件循环
    // 注意：在从节点的复制状态变为KVDATA_REPL_ONLINE之前，是不能将命令流发送给从节点的
    if (aeCreateFileEvent(server.eventsLoop, c->fd, AE_WRITABLE, sendReplyToClient, c) == AE_ERR) 
        return AE_ERR;
    return AE_OK;
}

/*
 * 为客户端安装写处理器到事件循环
 * 将对象obj根据编码类型加入到回复缓冲区中
 * obj对象可以是字符串编码也可以是整数编码
 */
void addReply(KVClient *c, robj *obj) {
    // 为客户端安装写处理器到事件循环
    if (prepareClientToWrite(c) != AE_OK) return;
    //如果对象obj为字符串编码
    if (obj->encoding == STRING || obj->encoding == INT) {
        // 首先尝试复制内容到固定回复缓冲区 c->buf 中，这样可以避免内存分配
        if (addReplyToBuffer(c,obj->ptr,sdslen(obj->ptr)) != AE_OK)
            // 如果 c->buf 中的空间不够，就复制到 c->reply 链表中
            // 可能会引起内存分配
            addReplyObjectToList(c,obj);
    //如果对象为整数编码
    } else {
        printf("Wrong obj->encoding in addReply().\n");
    }
}

/*
 * 将 SDS 中的内容复制到回复缓冲区
 * 函数结束会释放sds的s
 */
void addReplySds(KVClient *c, sds s) {
    // 为客户端安装写处理器到事件循环
    if (prepareClientToWrite(c) != AE_OK) {
       //释放s
        sdsfree(s);
        return;
    }
    //尝试将回复长度为len的内容s添加到固定回复缓冲区c->buf 中
    if (addReplyToBuffer(c,s,sdslen(s)) == AE_OK) {
        sdsfree(s);
    } else {
       // 将回复对象（一个 SDS ）添加到 c->reply 回复链表中
        robj *o = createObject(STRING, s);
        addReplyObjectToList(c,o);
    }
}

/*
 * 尝试将回复长度为len的内容s添加到固定回复缓冲区c->buf 中
 */
int addReplyToBuffer(KVClient *c, char *s, size_t len) {
    //获取回复缓冲区中buf数组还有多少空闲空间
    size_t available = sizeof(c->buf)-c->bufpos;

    // 回复链表里已经有内容，再添加内容到 c->buf 里面就是错误了
    if (listLength(c->reply) > 0) 
    return AE_ERR;

    // 空间必须满足，新添加的内容不能比回复缓冲区中空闲空间大
    if (len > available) 
    return AE_ERR;

    // 复制内容到 c->buf 里面
    memcpy(c->buf+c->bufpos,s,len);
    c->bufpos+=len;

    return AE_OK;
}

/*
 * 将回复对象（一个 SDS ）添加到 c->reply 回复链表中 
 */
void addReplyObjectToList(KVClient *c, robj *o) {
    robj *tail;

    // 客户端即将被关闭，无须再发送回复
    if (c->flags & KVDATA_CLOSE_AFTER_REPLY) return;

    // 链表中无缓冲块，直接将对象追加到链表中
    if (listLength(c->reply) == 0) {
        incrRefCount(o);
        listAddNodeTail(c->reply,o);
        c->reply_bytes += zmalloc_size_sds(o->ptr);

    // 链表中已有缓冲块，尝试将回复添加到块内
    // 如果当前的块不能容纳回复的话，那么新建一个块
    } else {
        // 取出表尾的 SDS
        tail = listNodeValue(listLast(c->reply));

        // 如果表尾 SDS 的已用空间加上对象的长度，小于 KVDATA_REPLY_CHUNK_BYTES
        // 那么将新对象的内容拼接到表尾 SDS 的末尾
        if (tail->ptr != NULL && sdslen(tail->ptr)+sdslen(o->ptr) <= KVDATA_REPLY_CHUNK_BYTES)
        {
            //先减去计算表尾缓冲块已用缓冲区
            c->reply_bytes -= zmalloc_size_sds(tail->ptr);

            //将表尾缓冲块复制一块新的引用计数为1的对象替代原来缓冲块
            //因为当该回复内容发送给客户端以后，该缓冲需要被释放，如果共享原来的对象，会影响其它客户端的使用
            tail = dupLastObject(c->reply);

            // 将对象o中的内容拼接到新的表尾缓存块中
            tail->ptr = sdscatlen(tail->ptr,o->ptr,sdslen(o->ptr));

            //重新计算可变回复缓冲区的大小
            c->reply_bytes += zmalloc_size_sds(tail->ptr);

        // 如果新对象的长度比原表尾缓冲块空用空闲空间大
        // 直接将对象追加到末尾
        } else {
            incrRefCount(o);
            listAddNodeTail(c->reply,o);
            c->reply_bytes += zmalloc_size_sds(o->ptr);
        }
    }
}

/*
 * 创建批量回复的长度前缀，示例:$2234\r\n
 */
void addReplyBulkLen(KVClient *c, robj *obj) {
    size_t len;
    //计算对象的长度
    len = sdslen(obj->ptr);
   //判断对象的长度是否符合共享对象，符合则直接将对应的共享对象填入回复缓冲区中
   //否则编码成协议格式后再存入回复缓冲区
    if (len < KVDATA_SHARED_BULKHDR_LEN)
        addReply(c,shared.bulkhdr[len]);
    else
        addReplyLongLongWithPrefix(c,len,'$');
}


/* 添加一个 long long 为整数回复，或者 bulk 或 multi bulk 的数目
 * 输出格式为 <prefix><long long><crlf>
 * 根据选择的前缀prefix不同，有如下两个例子:
 * *5\r\n10086\r\n
 * $5\r\n10086\r\n
 */
void addReplyLongLongWithPrefix(KVClient *c, long long ll, char prefix) {
    char buf[128];
    int len;

    /* 协议经常会发出$3\r\n或*2\r\n这样的值，所以如果整数很小，我们就有几个共享对象可以使用，就像大多数时候一样 */
    if (prefix == '*' && ll < KVDATA_SHARED_BULKHDR_LEN) {
        // 多条批量回复
        addReply(c,shared.mbulkhdr[ll]);
        return;
    } else if (prefix == '$' && ll < KVDATA_SHARED_BULKHDR_LEN) {
        // 批量回复
        addReply(c,shared.bulkhdr[ll]);
        return;
    }
    //如果整数够大，无法用共享对象，则需要将ll编码成协议格式
    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    //将编码后的ll存入回复缓冲中
    robj *o = createObject(STRING,sdsnewlen(buf,len+3));
    addReply(c,o);
}

/*
 * 返回一个对象作为回复
 * 例如对象obj为一个字符串对象name
 * 则三个函数可以向回复缓冲区中写入：$4\r\nname\r\n
 */
void addReplyBulk(KVClient *c, robj *obj) {
    addReplyBulkLen(c,obj);
    addReply(c,obj);
    addReply(c,shared.crlf);
}

/*
 * 负责传送命令回复的写处理器
 * 将客户端对应的回复缓冲区中内容和回复缓冲链表中的内容发送给对应客户端
 */
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    KVClient *c = privdata;
    int nwritten = 10, totwritten = 0, objlen;
    size_t objmem; 
    robj *o;
    //无用参数避免警告
    KVDATA_NOTUSED(el);
    KVDATA_NOTUSED(mask);
    // 一直循环，直到回复缓冲区为空
    // 或者指定条件满足为止
    while(c->bufpos > 0 || listLength(c->reply)) {
        //固定缓冲区buf
        if (c->bufpos > 0) {
            // 将固定回复缓冲区的内容写入到套接字
            // c->sentlen 是用来处理短写的
            // 当出现短写，导致写入未能一次完成时，下次触发写函数
            // c->buf+c->sentlen 就会偏移到正确（未写入）内容的位置上。
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);
            // 出错则跳出
            if (nwritten <= 0) break;
            // 成功写入则更新已写入计数器变量
            c->sentlen += nwritten;
            totwritten += nwritten;
            // 如果缓冲区中的内容已经全部写入完毕
            // 那么清空客户端的两个计数器变量
            if (c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        //可变缓冲区链表
        } else {
            // 取出位于链表最前面的对象
            o = listNodeValue(listFirst(c->reply));
            //链表节点对象的长度
            objlen = sdslen(o->ptr);
            objmem = zmalloc_size_sds(o->ptr);

            // 略过空对象
            if (objlen == 0) {
                listDelNode(c->reply,listFirst(c->reply));
                c->reply_bytes -= objmem;
                continue;
            }
            // 将固定回复缓冲区的内容写入到套接字
            // c->sentlen 是用来处理短写的
            // 当出现短写，导致写入未能一次完成时，下次触发写函数
            // c->buf+c->sentlen 就会偏移到正确（未写入）内容的位置上。
            nwritten = write(fd, ((char*)o->ptr)+c->sentlen,objlen-c->sentlen);
            // 写入出错则跳出
            if (nwritten <= 0) break;
            // 成功写入则更新写入计数器变量
            c->sentlen += nwritten;
            totwritten += nwritten;
            // 如果缓冲区内容全部写入完毕，那么删除已写入完毕的节点
            if (c->sentlen == objlen) {
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                c->reply_bytes -= objmem;
            }
        }
        /*
         * 为了避免一个非常大的回复独占服务器，
         * 当写入的总数量大于 KVDATA_MAX_WRITE_PER_EVENT ，
         * 临时中断写入，将处理时间让给其他客户端，
         * 剩余的内容等下次写入就绪再继续写入
         */
        if (totwritten > KVDATA_MAX_WRITE_PER_EVENT) break;
    }
    // 写入出错检查
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            printf("Error writing to client: %s\n", strerror(errno));
            freeClient(c);
            return;
        }
    }
    //更新最近一次客户端服务器的互动时间
    if (totwritten > 0) {
        if (!(c->flags & KVDATA_MASTER)) c->lastinteraction = server.unixtime;
    }

    //当客户端回复缓冲区中没有内容，则将该客户写事件从epoll红黑树中移除，
    //并从已注册事件数组中移除
    if (c->bufpos == 0 && listLength(c->reply) == 0) {
        c->sentlen = 0;
        // 删除 write handler
        //aeDeleteFileEvent(server.eventsLoop,c->fd,AE_WRITABLE);
        // 如果指定了写入之后关闭客户端 FLAG ，那么关闭客户端
        if (c->flags & KVDATA_CLOSE_AFTER_REPLY) 
        freeClient(c);
    }
}



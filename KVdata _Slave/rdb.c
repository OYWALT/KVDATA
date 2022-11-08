#include "rdb.h"
#include "errno.h"
#include <string.h>
#include "db.h"
#include <stdio.h>
#include <unistd.h>
#include "saveStream.h"
#include <time.h>
#include "server.h"
#include <stdlib.h>
#include "slave.h"
extern struct sharedObjectsStruct shared;//共享对象
extern KVServer server;//全局服务器变量

/*--------------------------------------------将数据库中的数据载入到RDB文件中--------------------------------------------*/
/*
 * 先创建利用saveStream*RDB创建一个文件描述符，
 * 将所有数据库中的键值对信息保存到对应的文件中，
 * 将数据库保存到磁盘上。
 * 
 * 保存成功返回 KVDATA_OK ，出错/失败返回 KVDATA_ERR 。
 */
int rdbSave(char *filename) {
    //数据库字典迭代器
    dictIterator *di = NULL;
    //字典节点
    dictEntry *de;
    //用于保存RDB文件名
    char tmpfile[256];
    //用于标识RDB
    char magic[4];
    //获取当前时间，用于判断键是否过期，从而决定是否将该键持久化
    long long now = mstime();

    //用于保存RDB文件的文件指针
    FILE *fp;
    saveStream rdb;
    uint64_t cksum;

    // 创建临时文件"temp-getpid().rdb"
    snprintf(tmpfile,256,"temp-%d.rdb", (int) getpid());

    //fopen创建一个用于写入的空文件tmpfile。如果文件名称与已存在的文件相同，则会删除已有文件的内容，文件被视为一个新的空文件。
    //创建成功返回一个 FILE 指针，失败返回NULL
    fp = fopen(tmpfile,"w");
    if (!fp) {
        //文件开启失败写入日志
        printf("Failed opening .rdb for saving: %s\n", strerror(errno));
        return RDB_ERR;
    }

    // 初始化 I/O saveStream文件流，就是说将saveStream*RDB与文件流fp关联起来
    saveStreamInitWithFile(&rdb,fp);

    // // 初始化校验和函数
    // if (server.rdb_checksum)
    //     rdb.update_cksum = rioGenericUpdateChecksum;

    // 向magic缓冲区写入 RDB 版本标志
    memcpy(magic,"RDB",3);
    magic[3] = '\0';

    //将RDB标志写入rdb中
    if (saveStreamWrite(&rdb,magic,3) == 0) goto werr;

    // 遍历所有数据库，将所有数据库中的键值对信息存入rdb中
    for (int j = 0; j < server.dbnum; j++) {

        // 指向数据库
        KVdataDb *db = server.db+j;

        // 指向数据库键空间
        dict *d = db->DB;

        // 跳过空数据库
        if (dictSize(d) == 0) continue;

        // 创建键空间安全迭代器
        di = dictGetIterator(d);
        if (!di) {
            //如果迭代器创建失败，关闭流 fp。刷新所有的缓冲区。
            fclose(fp);
            return RDB_ERR;
        }

        // 向rdb中写入数据库 DB 选择器
        // <KVDATA_RDB_OPCODE_SELECTDB><数据库序号>
        if (rdbSaveType(&rdb,RDB_OPCODE_SELECTDB) == -1) goto werr;
        if (rdbSaveLen(&rdb,j) == -1) goto werr;

        //遍历数据库，并写入每个键值对的数据
        while((de = dictNext(di)) != NULL) {
            //获取键
            sds keystr = de->key;
            robj *key=createStringObject(keystr,strlen(keystr));
            //获取值
            robj *o = dictGetVal(de);
            // 获取键key的过期时间
            long long expire = getExpire(db,key);
            // 保存键值对数据
            if (rdbSaveKeyValuePair(&rdb,key,o,expire,now) == -1) goto werr;
        }
        //当前数据库遍历完毕，释放字典迭代器，移动到下一数据库
        dictReleaseIterator(di);
    }
    //释放内存后的指针变量赋空，防止出现野指针
    di = NULL; 

    //将长度为 1 字节的字符 KVDATA_RDB_OPCODE_EOF写入到 rdb 文件中，标志着RDB文件正文内容的结束
    if (rdbSaveType(&rdb,RDB_OPCODE_EOF) == -1) goto werr;

    /*-----------------------------------CRC64 校验和-----------------------------------*/
    //获取saveStream文件流结构中实时更新的校验和
    cksum = rdb.cksum;
    //根据需要进行大小端转换
    //memrev64ifbe(&cksum);
    //将长度为 8 字节的校验和cksum写入到 rdb 文件中
    saveStreamWrite(&rdb,&cksum,8);

    /*-----------------------------------将数据写入内核缓冲区后，再写入磁盘-----------------------------------*/
    // 冲洗缓存，确保数据已写入内核缓冲区
    if (fflush(fp) == EOF) goto werr;
    //确保流fp的所有内存都写入了磁盘.
    if (fsync(fileno(fp)) == -1) goto werr;
    //关闭流 stream。刷新所有的缓冲区。
    if (fclose(fp) == EOF) goto werr;

    //把 old_filename 所指向的文件名改为 new_filename。错误返回-1
    if (rename(tmpfile,filename) == -1) {
        printf("Error moving temp DB file on the final destination: %s\n", strerror(errno));
        //对文件的连接计数-1，为0时才会删除该文件
        unlink(tmpfile);
        return RDB_ERR;
    }

    // 写入完成，打印日志
    printf("DB saved on disk.\n");

    //情况一：同步运行，生成RDB文件时服务器不响应命令
    //情况二：后台运行，则此处修改的子进程中的server.dirty，不会影响主进程在backgroundSaveDoneHandler函数中对server.dirty的计算
    server.dirty = 0;
    // 记录最近一次完成 SAVE 的时间
    //time(NULL）函数的返回值是从1970年1月1日0时整到此时此刻所持续的秒数。
    server.lastsave = time(NULL);

    return RDB_OK;

    werr:
    // 关闭文件
    fclose(fp);
    // 删除文件
    unlink(tmpfile);
    printf("Write error saving DB on disk: %s\n", strerror(errno));
    //如果有需要，释放字典迭代器
    if (di) dictReleaseIterator(di);

    return RDB_ERR;
}



/*
 * 将键值对的键、值、过期时间和类型写入到 RDB 中。
 * 出错返回 -1 ，成功保存返回 1 ，当键已经过期时，返回 0 。
 */
int rdbSaveKeyValuePair(saveStream *rdb, robj *key, robj *val,
                        long long expiretime, long long now)
{
    /* 保存键的过期时间 */
    if (expiretime != -1) {
        //不写入已经过期的键
        if (expiretime < now) return 0;
        //将1字节的时间类型写入rdb
        if (rdbSaveType(rdb,RDB_OPCODE_EXPIRETIME_MS) == -1) return -1;
        //将长度为 8 字节的毫秒过期时间写入到 rdb 中。
        if (rdbSaveMillisecondTime(rdb,expiretime) == -1) return -1;
    }
    /* 保存类型，键，值 */
    //将对象val的类型写入rdb
    if (rdbSaveType(rdb,RDB_TYPE_STRING) == -1) return -1;
    //以字符串类型将键key写入rdb
    if (rdbSaveStringObject(rdb,key) == -1) return -1;
    //将对象val写入rdb
    if (rdbSaveStringObject(rdb,val) == -1) return -1;
    return 1;
}


/*
 * 将长度为 len 的字符数组 p 写入到 rdb中。
 * 写入成功返回 len ，失败返回 -1 。
 */
int rdbWriteRaw(saveStream *rdb, void *p, size_t len) {
    if (rdb && saveStreamWrite(rdb,p,len) == 0)
        return -1;
    return len;
}

/*
 * 将长度为 8 字节的毫秒过期时间t写入到 rdb 中。
 * 写入成功返回 len ，失败返回 -1 。
 */
int rdbSaveMillisecondTime(saveStream *rdb, long long t) {
    int64_t t64 = (int64_t) t;
    return rdbWriteRaw(rdb,&t64,8);
}

/* 
 * 将长度为 1 字节的数据库编号len写入到 rdb 中。
 * 写入成功返回1，写入失败返回-1。
 */
int rdbSaveLen(saveStream *rdb, unsigned char len) {
    unsigned char buf = len;
    return rdbWriteRaw(rdb,&buf,1);
}

/*
 * 将长度为 1 字节的字符 type 写入到 rdb中。
 * 写入成功返回1 ，失败返回 -1 。
 */
int rdbSaveType(saveStream *rdb, unsigned char type) {
    unsigned char buf = type;
    return rdbWriteRaw(rdb,&buf,1);
}
/*
 * 将给定的字符串对象 obj 保存到 rdb 中。
 * 函数返回 rdb 保存字符串对象所需的字节数。
 */
int rdbSaveStringObject(saveStream *rdb, robj *obj) {
    int n;
    // 保存字符串对象
    if (obj->encoding == STRING) {
        n = rdbSaveRawString(rdb,obj->ptr,sdslen(obj->ptr));
    } else {
        printf("Unknown object encoding.\n");
    }
    return n;
}

/*
 * 直接以 [len][data] 的形式将字符串对象写入到 rdb 中
 * 函数返回保存字符串所需的空间字节数。
 */
int rdbSaveRawString(saveStream *rdb, unsigned char *s, size_t len) {
    int enclen;
    int n, nwritten = 0;
    // 写入长度
    if ((n = rdbSaveLen(rdb,len)) == -1) return -1;
    nwritten += n;

    // 写入内容
    if (len > 0) {
        if (rdbWriteRaw(rdb,s,len) == -1) return -1;
        nwritten += len;
    }
    return nwritten;
}

/*
 * SAVE命令
 * 执行手动SAVE
 */
void saveCommand(KVClient *c) {
    // 执行
    if (rdbSave(server.rdb_filename) == RDB_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}
/*
 * 主进程进程执行 fork 操作创建子进程，RDB 持久化过程由子进程负责，完成后自动结束。
 * 只会在 fork 期间发生阻塞，但是一般时间都很短。
 * 但是如果数据库数据量特别大， fork 时间就会变长，而且占用内存会加倍，这一点需要特别注意。
 */
int rdbSaveBackground(char *filename) {
    pid_t childpid;
    long long start;
    
    // 如果 持久化BGSAVE 已经在执行，那么出错
    if (server.rdb_child_pid != -1) return AE_ERR;
    //开辟子进程
    if ((childpid = fork()) == 0) {
        /* Child */
        // 子进程关闭监听套接字
        close(server.listenfd);

        // 执行db 写入操作，保存操作
        int retval = rdbSave(filename);

        // 打印 copy-on-write（写时复制） 时使用的内存数
        if (retval == AE_OK) {
            // 向父进程发送信号，退出子进程
            printf("BGSAVE successfully!\n");
            exit(0);
        }else{
            // 向父进程发送信号，退出子进程
            printf("BGSAVE failed!\n");
            exit(1);
        }
    } else {
        /* Parent */
        // 如果 fork() 出错，那么报告错误
        if (childpid == -1) {
            //写入错误日志
            printf("Can't save in background: fork: %s\n", strerror(errno));
            return AE_ERR;
        }
        // 打印 BGSAVE 开始的日志
        printf("Background saving started by pid %d\n",childpid);
        // 记录负责执行 BGSAVE 的子进程 ID
        server.rdb_child_pid = childpid;
        return AE_OK;
    }

    return AE_OK; /*不会到达,预防警告*/
}
/*--------------------------------------------将RDB文件中的数据导入到数据库中--------------------------------------------*/

/*
 * 将给定 rdb 中保存的数据载入到数据库中。
 */
int rdbLoad(char *filename) {
    uint32_t dbid;
    int type, rdbver;
    KVdataDb *db = server.db+0;
    char buf[1024];
    long long expiretime, now = mstime();
    FILE *fp;
    saveStream rdb;

    // 打开 rdb 文件（该文件必须存在）
    if ((fp = fopen(filename,"r")) == NULL) {
    printf("open fail errno reason = %s \n", strerror(errno));
    return RDB_ERR;
    }

    // 初始化写入流
    saveStreamInitWithFile(&rdb,fp);

    // //初始化 校验和计算函数
    // if (server.rdb_checksum)
    // rdb.update_cksum = rdbLoadProgressCallback;
   
    //从saveStream中读入RDB的版本号
    if (saveStreamRead(&rdb,buf,3) == 0) goto rdberr;
    buf[3] = '\0';
    // 获取RDB文件标志
    // 检查buf的前3个字节是否为"RDB"，不是则关闭流fp，报错直接退出
    if (memcmp(buf,"RDB",3) != 0) {
        fclose(fp);
        printf("Wrong signature trying to load DB from file\n");
        return RDB_ERR;
    }
    /*将服务器状态调整到开始载入状态*/ 
    // 服务器正在载入标志置位
    server.loading = 1;
    // 开始进行载入的时间
    server.loading_start_time = time(NULL);

    //往数据库中载入键值对信息
    while(1) {
        robj *key, *val;
        expiretime = -1;

        //获取类型指示TYPE
        if ((type = rdbLoadType(&rdb)) == -1) goto rdberr;
        // 读入过期时间值（秒）
        if (type == RDB_OPCODE_EXPIRETIME_MS) {
            // 以毫秒计算的过期时间
            if ((expiretime = rdbLoadMillisecondTime(&rdb)) == -1) goto rdberr;
            //在过期时间之后会跟着一个键值对，我们要读入这个键值对的类型
            if ((type = rdbLoadType(&rdb)) == -1) goto rdberr;
        }

        // 读入数据 EOF （不是 rdb 文件的 EOF）
        if (type == RDB_OPCODE_EOF)
            break;

        //读入切换数据库指示
        if (type == RDB_OPCODE_SELECTDB) {

            // 读入数据库号码
            if ((dbid = rdbLoadLen(&rdb)) == RDB_LENERR)
                goto rdberr;
            // 检查数据库号码的正确性
            if (dbid >= (unsigned)server.dbnum) {
                printf("FATAL: Data file was created with a KVDATA server configured to handle more than %d databases. Exiting.\n", server.dbnum);
                return RDB_ERR;
            }
            // 在程序内容切换数据库
            db = server.db+dbid;
            // 转到正确的数据库后，开始载入数据
            continue;
        }


        //读入键名
        if ((key = rdbGenericLoadStringObject(&rdb)) == NULL) goto rdberr;
        //读入type类型对象的键的值
        if ((val = rdbLoadObject(type,&rdb)) == NULL) goto rdberr;

        //那么在键已经过期的时候，不再将它们关联到数据库中去
        if (expiretime != -1 && expiretime < now) {
            decrRefCount(key);
            decrRefCount(val);
            // 跳过
            continue;
        }

        //将键值对关联到数据库中
        dbAdd(db,key,val);

        //数据库中设置过期时间
        if (expiretime != -1) setExpire(db,key,expiretime);

        decrRefCount(key);
    }
    //比对校验和
    if (server.rdb_checksum) {
        //获取载入RDB文件后的校验和（这个校验和是在载入RDB文件还原数据库过程中实时更新的最终结果）
        uint64_t cksum, expected = rdb.cksum;

        // 从rdb中读入文件的校验和(这个校验和是原RDB文件中末尾带上的)
        if (saveStreamRead(&rdb,&cksum,8) == 0) goto rdberr;
        //memrev64ifbe(&cksum);

        // 比对校验和，将rdb中的校验和cksum与通过
        if (cksum == 0) {
            printf("RDB file was saved with checksum disabled: no check performed.\n");
        } else if (cksum != expected) {
            printf("Wrong RDB checksum. Aborting now.\n");
            return RDB_ERR;
        }
    }
    // 关闭 RDB
    fclose(fp);
    // 服务器从载入状态中退出
    server.loading = 0;
    printf("RDB restores the database successfully for the first time.\n");
    return RDB_OK;

    /* 在这里处理文件的意外结束，使用一个致命的退出 */
    rdberr: 
    printf("Short read or OOM loading DB. Unrecoverable error, aborting now.\n");
    return RDB_ERR; 
}


/*
 * 从 rdb 中载出 1 字节长的 type 数据。
 * 正确返回类型type，错误返回-1
 */
int rdbLoadType(saveStream *rdb) {
    unsigned char type;
    //读出1字节的类型
    if (saveStreamRead(rdb,&type,1) == 0) return -1;
    return type;
}

/*
 * 从 rdb 中载出8字节长的毫秒过期时间
 */
long long rdbLoadMillisecondTime(saveStream *rdb) {
    int64_t t64;
    if (saveStreamRead(rdb,&t64,8) == 0) return -1;
    return (long long)t64;
}

/*
 * 从rdb中载出一个单字节的长度值
 * 载出成功返回一个整数，载出失败返回KVDATA_RDB_LENERR
 */
int rdbLoadLen(saveStream *rdb) {
    unsigned char buf;
    
    // 读入被编码的长度值保存在buf ，这个值可能已经是“编码类型”，也可能是一个被编码的长度
    if (saveStreamRead(rdb,&buf,1) == 0) return RDB_LENERR;
    return buf&0x3F;
   
}

/*
 * 从 rdb 文件中载出指定类型的对象并将该对象返回。
 * 载出成功返回一个新对象，否则返回 NULL 。
 */
robj *rdbLoadObject(int rdbtype, saveStream *rdb) {
    robj *o;
    // 载入字符串对象
    if (rdbtype == RDB_TYPE_STRING) {
        //从 rdb 中载入一个字符串对象
        if ((o = rdbGenericLoadStringObject(rdb)) == NULL) return NULL;
    } 
    else {
        printf("Unknown object type.\n");
    }
    return o;
}


/*
 * 从 rdb 中载入一个字符串对象
 * 返回该字符串对象
 */
robj *rdbGenericLoadStringObject(saveStream *rdb) {
    int isencoded;
    uint32_t len;
    sds val;

    // 读出字符串对象的长度
    len = rdbLoadLen(rdb);
    // 直接从 rdb 中读出它
    val = sdsnewlen(NULL,len);
    if (len && saveStreamRead(rdb,val,len) == 0) {
        //写入失败
        sdsfree(val);
        return NULL;
    }
    //返回从rdb读入的字符串对象val
    return createObject(STRING,val);
}

/*
 * LOAD命令
 * 执行手动LOAD
 */
void loadCommand(KVClient *c) {
    // 执行
    if (rdbLoad(server.rdb_filename) == RDB_OK) {
        addReply(c,shared.ok);
    } else {
        addReply(c,shared.err);
    }
}


/* 
 * 处理 BGSAVE 完成时发送的信号
 * exitcode=0表示正常退出，bysignal==1表示被信号中断
 */
void backgroundSaveDoneHandler(int exitcode, int bysignal) {
    // BGSAVE 成功
    if (!bysignal && exitcode == 0) {
        printf("Background saving terminated with success.\n");
    // BGSAVE 出错
    } else if (!bysignal && exitcode != 0) {
        printf("Background saving error.");
    // BGSAVE 被中断
    } else {
        printf("Background saving terminated by signal %d.\n", bysignal);
        // 移除临时文件
        char tmpfile[256];
        snprintf(tmpfile,256,"temp-%d.rdb\n", (int) server.rdb_child_pid);
        //将该文件的链接计数-1，当链接计数为0时，该文件将被删除
        unlink(tmpfile);
    
    }
    // 更新服务器状态
    server.rdb_child_pid = -1;

    // 处理正在等待 BGSAVE 完成的那些 slave
    updateSlavesWaitingBgsave(exitcode == 0 ? AE_OK : AE_ERR);
}
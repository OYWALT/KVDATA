# KVDATA
# 一个基于内存的可持久化数据库。
项目功能：主要分为四大块：支持Key-Value的数据存取，可将数据持久化到本地磁盘、支持事务操作、支持主从复制。主要采用epoll_reactor模型作为多路I/O复用主框架。其中在对键值的存取时采用惰性删除支持过期键，对事务提供监视键功能。

# 文件结构
KVDATA主要包含主服务器(KVdata_Master)、从服务器(KVdata_Slave)、客户端(KVdata_Client)三个文件夹。对于想深入研究并调试KVDATA代码的同学，应主要下载这三个文件中的代码。
主要命令如下示：
struct KVDataCommand KVDATACommandTable[] = {
    {"set",setCommand,3,3}, //SET KEY VALUE
    {"get",getCommand,2,3}, //GET KEY
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

# 运行环境
Linux + ubuntu

# A Quick Start
* step1:分别在分别在ubuntu中开辟三个终端，运行如下代码：

`<gcc *.c -o go>`

`<./go port>` 

此处主从服务器的端口不能一样，客户端运行时port应对应其连接的服务器

* step2:在客户端中输入命令来对服务器进行数据存取等操作。

# Contact Me：
本人水平、精力有限，花在造轮子上的时间实际也不多，所以源码中难免存在一些问题，欢迎前辈和同学与我讨论其中存在的问题，恳请各位大佬批评指正！
我的邮箱（保证随时可以联系到我！）：1627280448@qq.com

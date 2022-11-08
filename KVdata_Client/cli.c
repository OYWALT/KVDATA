#include<stdio.h>
#include<unistd.h>
#include<stdlib.h>
#include <fcntl.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include <errno.h>
#include<string.h>
#define SEV_PORT 6667   //连接的服务器端口号
#define SEV_IP "127.0.0.1"  //连接的服务器IP地址

char fbuf[100];//存放所有参数”$3\r\nset\r\n$4\r\nname\r\n$3\r\noyw\r\n“
char buf[100]; //存放输入命令”set name oyw“
char buff[100];//存放单个参数”$3\r\nset\r\n“
char ffbuf[100];//存放带有参数个数的所有参数”*3\r\n$3\r\nset\r\n$4\r\nname\r\n$3\r\noyw\r\n“
int pos = 0;//记录游标
int len;
int count=0;//记录参数个数

void cleanBuff();

int main(int argc,void *argv[])
{
/*------------------------------------------与服务器建立连接请求---------------------------------------------------------------*/
    int cfd;
    //初始化套接字
    cfd=socket(AF_INET,SOCK_STREAM,0);

    //初始化服务器的scokaddr_in结构体
    struct sockaddr_in sever_addr;
    memset(&sever_addr,0,sizeof(sever_addr));
    sever_addr.sin_family=AF_INET;

    int port;
    if(argc==2)
        port=atoi(argv[1]);
    else
        port=SEV_PORT;
    sever_addr.sin_port=htons(port);

    //将字符串点分法表示的IP地址转换成网络字节序列
    int re=inet_pton(AF_INET,SEV_IP,&sever_addr.sin_addr.s_addr);

    //向服务器提出连接请求
    int ret=connect(cfd,(struct sockaddr*)&sever_addr,sizeof(sever_addr));
    if(ret<0)
    {
        perror("Connect error:");
        exit(1);
    }

    while(1) 
    {
        char* blank;
        cleanBuff();//清空所有缓冲区
        /*----------------------------------------将输入命令转换成正确的格式---------------------------------------------------------*/
        fgets(buf, sizeof(buf), stdin);
        while((blank = strchr(buf+pos, ' '))!=NULL||(blank = strchr(buf+pos, '\n'))!=NULL)
        {
            count++;//参数个数计数

            len = blank - (buf+pos);//当前参数长度

            int ret = snprintf(buff, sizeof(buff),"$%d\r\n", len);//$3\r\n
            if (ret < 0)
                printf("snprintf error\n");

            strncat(buff, buf+pos, len);//$3\r\nSET

            strncat(buff, "\r\n ", 2);//$3\r\nSET\r\n

            strncat(fbuf,buff,strlen(buff));

            pos = blank - buf + 1;
        }
            int ret1 = snprintf(ffbuf, sizeof(ffbuf),"*%d\r\n", count);//*3\r\n
            if (ret1 < 0)
                printf("snprintf error\n");
            strncat(ffbuf,fbuf,strlen(fbuf));//*3\r\n$3\r\nset\r\n$4\r\nname\r\n$3\r\noyw\r\n
            //printf("%s\n", ffbuf);

        /*-------------------------------------------向服务器写入命令------------------------------------------------------*/
            int ret2;
            int ret3;
            char buf1[100]; 
            int flags;
            //还是不行，阻塞读在进行事务时收不到服务器的回复将阻塞，无法继续向服务区发送数据
            // //设置文件描述符fd为O_NONBLOCK(非阻塞)
            // if (fcntl(cfd, F_SETFL, flags | O_NONBLOCK) == -1) {
            //     printf("fcntl(F_SETFL,O_NONBLOCK) err: %s\n", strerror(errno));
            // }
            ret2=write(cfd,ffbuf,strlen(ffbuf));
            if(ret2<0)
            {
                printf("Write error:");
                exit(1);
            }
            else
                printf("Write Successfully!!!!\n");
            ret3=read(cfd,buf1,sizeof(buf1));
            if(ret3<=0)
            {
                //printf("read error:\n");
                //exit(1);
            }
            else
            {
                printf("%s\n",buf1);
                memset(buf1,'\0',sizeof(buf1));
            }
    }
       
    return 0;
}

/*
 * 清空缓冲区
 */
void cleanBuff()
{ 
        memset(fbuf,'\0',sizeof(fbuf));
        memset(buf,'\0',sizeof(buf));
        memset(buff,'\0',sizeof(buff));
        memset(ffbuf,'\0',sizeof(ffbuf));
        pos = 0;//记录游标
        len = 0;
        count=0;//记录参数个数
}
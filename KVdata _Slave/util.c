#include <sys/time.h>
#include <unistd.h>
#include <ctype.h>
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
//对于服务器的运行ID采用随机生成
//这样就可以用于用于判断是否访问的是同一个服务器，或者该服务器是否重启了
void getRandomHexChars(char *p, unsigned int len) {

    char *x = p;
    unsigned int l = len;
    struct timeval tv;
    int pid = getpid();
    char *charset = "0123456789abcdef";
    /* 使用时间和进程ID来初始化数组 */
    gettimeofday(&tv,NULL);
    if (l >= sizeof(tv.tv_usec)) {
        memcpy(x,&tv.tv_usec,sizeof(tv.tv_usec));
        l -= sizeof(tv.tv_usec);
        x += sizeof(tv.tv_usec);
    }
    if (l >= sizeof(tv.tv_sec)) {
        memcpy(x,&tv.tv_sec,sizeof(tv.tv_sec));
        l -= sizeof(tv.tv_sec);
        x += sizeof(tv.tv_sec);
    }
    if (l >= sizeof(pid)) {
        memcpy(x,&pid,sizeof(pid));
        l -= sizeof(pid);
        x += sizeof(pid);
    }
    /* 再用随机数与每一个p[j]按位异或*/
    for (int j = 0; j < len; j++)
        p[j] ^= rand();
    
    /* 得到的每一位随机id成员再去charset中索引一个字符. */
    for (int j = 0; j < len; j++)
        p[j] = charset[p[j] & 0x0F];

    // 为运行 ID 加上结尾字符
    p[len] = '\0';
}

/* Convert a string into a long long. Returns 1 if the string could be parsed
 * into a (non-overflowing) long long, 0 otherwise. The value will be set to
 * the parsed value when appropriate. */
int string2ll(const char *s, size_t slen, long long *value) {
    const char *p = s;
    size_t plen = 0;
    int negative = 0;
    unsigned long long v;

    if (plen == slen)
        return 0;

    /* Special case: first and only digit is 0. */
    if (slen == 1 && p[0] == '0') {
        if (value != NULL) *value = 0;
        return 1;
    }

    if (p[0] == '-') {
        negative = 1;
        p++; plen++;

        /* Abort on only a negative sign. */
        if (plen == slen)
            return 0;
    }

    /* First digit should be 1-9, otherwise the string should just be 0. */
    if (p[0] >= '1' && p[0] <= '9') {
        v = p[0]-'0';
        p++; plen++;
    } else if (p[0] == '0' && slen == 1) {
        *value = 0;
        return 1;
    } else {
        return 0;
    }

    while (plen < slen && p[0] >= '0' && p[0] <= '9') {
        if (v > (ULLONG_MAX / 10)) /* Overflow. */
            return 0;
        v *= 10;

        if (v > (ULLONG_MAX - (p[0]-'0'))) /* Overflow. */
            return 0;
        v += p[0]-'0';

        p++; plen++;
    }

    /* Return if not all bytes were used. */
    if (plen < slen)
        return 0;

    if (negative) {
        if (v > ((unsigned long long)(-(LLONG_MIN+1))+1)) /* Overflow. */
            return 0;
        if (value != NULL) *value = -v;
    } else {
        if (v > LLONG_MAX) /* Overflow. */
            return 0;
        if (value != NULL) *value = v;
    }
    return 1;
}

/* 将long long转换为字符串。返回表示该数字所需的字符数，
 * 如果传递的缓冲区长度不足以存储整个数字，则可以更短。*/
int ll2string(char *s, size_t len, long long value) {
    char buf[32], *p;
    unsigned long long v;
    size_t l;

    if (len == 0) return 0;
    v = (value < 0) ? -value : value;
    p = buf+31; /* point to the last character */
    do {
        *p-- = '0'+(v%10);
        v /= 10;
    } while(v);
    if (value < 0) *p-- = '-';
    p++;
    l = 32-(p-buf);
    if (l+1 > len) l = len-1; /* Make sure it fits, including the nul term */
    memcpy(s,p,l);
    s[l] = '\0';
    return l;
}



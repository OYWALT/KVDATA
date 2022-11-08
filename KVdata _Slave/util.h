#ifndef KVDATA_UTIL_H
#define KVDATA_UTIL_H

void getRandomHexChars(char *p, unsigned int len);
int string2ll(const char *s, size_t slen, long long *value);
int ll2string(char *s, size_t len, long long value);

#endif
#ifndef KVDATA_RDB_H
#define KVDATA_RDB_H

#include "saveStream.h"

#define RDB_OK 0
#define RDB_ERR -1


/*
 * 还原数据库特殊操作标识符
 */
// 字符串类型的对象
#define RDB_TYPE_STRING 0
// 表示读取/写入错误
#define RDB_LENERR 252
// 以毫秒计算的过期时间
#define RDB_OPCODE_EXPIRETIME_MS 253
// 选择数据库
#define RDB_OPCODE_SELECTDB   254
// 数据库的结尾（但不是 RDB 文件的结尾）
#define RDB_OPCODE_EOF        255




int rdbSave(char *filename);
int rdbSaveKeyValuePair(saveStream *rdb, robj *key, robj *val, long long expiretime, long long now);
int rdbWriteRaw(saveStream *rdb, void *p, size_t len);
int rdbSaveMillisecondTime(saveStream *rdb, long long t);
int rdbSaveLen(saveStream *rdb, unsigned char len);
int rdbSaveType(saveStream *rdb, unsigned char type);
int rdbSaveStringObject(saveStream *rdb, robj *obj);
int rdbSaveRawString(saveStream *rdb, unsigned char *s, size_t len);
int rdbSaveBackground(char *filename);

int rdbLoad(char *filename);
int rdbLoadType(saveStream *rdb);
long long rdbLoadMillisecondTime(saveStream *rdb);
int rdbLoadLen(saveStream *rdb);
robj *rdbLoadObject(int rdbtype, saveStream *rdb);
robj *rdbGenericLoadStringObject(saveStream *rdb);
void backgroundSaveDoneHandler(int exitcode, int bysignal);

#endif
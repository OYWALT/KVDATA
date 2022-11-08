#ifndef KVDATA_LIST_H
#define KVDATA_LIST_H

/*双端链表节点*/
typedef struct listNode {

    // 前置节点
    struct listNode *prev;
    // 后置节点
    struct listNode *next;
    // 节点的值
    void *value;

} listNode;

/*双端链表结构*/
typedef struct list {

    // 表头节点
    listNode *head;
    // 表尾节点
    listNode *tail;
    // 节点值复制函数
    void *(*dup)(void *ptr);
    // 节点值释放函数
    void (*free)(void *ptr);
    // 节点值对比函数
    int (*match)(void *ptr, void *key);
    // 链表所包含的节点数量
    unsigned long len;

} list;

// 返回给定节点的值
#define listNodeValue(n) ((n)->value)
// 返回给定链表所包含的节点数量
#define listLength(l) ((l)->len)
// 返回给定链表的表尾节点
#define listLast(l) ((l)->tail)
// 返回给定链表的表头节点
#define listFirst(l) ((l)->head)
list *listCreate(void);
list *listAddNodeTail(list *list, void *value);
list *listAddNodeHead(list *list, void *value);
void listRelease(list *list);
void listDelNode(list *list, listNode *node);
listNode *listSearchKey(list *list, void *key);
list *listDup(list *orig);
#endif
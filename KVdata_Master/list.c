#include "list.h"
#include <stdio.h>
#include "zmalloc.h"
/*
 * 创建一个新的链表
 * 创建成功返回链表，失败返回 NULL 。
 * T = O(1)
 */
list *listCreate(void)
{
    struct list *list;

    // 分配内存
    if ((list = zmalloc(sizeof(*list))) == NULL)
        return NULL;

    // 初始化属性
    list->head = list->tail = NULL;
    list->len = 0;
    list->dup = NULL;
    list->free = NULL;
    list->match = NULL;

    return list;
}

/*
 * 将一个包含有给定值指针 value 的新节点添加到链表的表尾
 * 如果为新节点分配内存出错，那么不执行任何动作，仅返回 NULL
 * 如果执行成功，返回传入的链表指针
 * T = O(1)
 */
list *listAddNodeTail(list *list, void *value)
{
    listNode *node;
    // 为新节点分配内存
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;
    // 保存值指针
    node->value = value;
    // 目标链表为空
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    // 目标链表非空
    } else {
        node->prev = list->tail;
        node->next = NULL;
        list->tail->next = node;
        list->tail = node;
    }
    // 更新链表节点数
    list->len++;
    return list;
}

/*
 * 将一个包含有给定值指针 value 的新节点添加到链表的表头
 * 如果为新节点分配内存出错，那么不执行任何动作，仅返回 NULL
 * 如果执行成功，返回传入的链表指针
 * T = O(1)
 */
list *listAddNodeHead(list *list, void *value)
{
    listNode *node;

    // 为节点分配内存
    if ((node = zmalloc(sizeof(*node))) == NULL)
        return NULL;

    // 保存值指针
    node->value = value;

    // 添加节点到空链表
    if (list->len == 0) {
        list->head = list->tail = node;
        node->prev = node->next = NULL;
    // 添加节点到非空链表
    } else {
        node->prev = NULL;
        node->next = list->head;
        list->head->prev = node;
        list->head = node;
    }

    // 更新链表节点数
    list->len++;
    return list;
}


/*
 * 释放整个链表，以及链表中所有节点
 * T = O(N)
 */
void listRelease(list *list)
{
    unsigned long len;
    listNode *current, *next;

    // 指向头指针
    current = list->head;
    // 遍历整个链表
    len = list->len;
    while(len--) {
        next = current->next;
        // 释放节点的值
        if(current->value != NULL)
        zfree(current->value);
         // 释放节点的结构    
        if(current != NULL)   
        zfree(current);

        current = next;
    }

    // 释放链表结构
    if(list != NULL)
    zfree(list);
}

/*
 * 从链表 list 中删除给定节点 node 
 * 对节点私有值(private value of the node)的释放工作由调用者进行。
 * T = O(1)
 */
void listDelNode(list *list, listNode *node)
{
    // 调整前置节点的指针
    if (node->prev)
        node->prev->next = node->next;
    else
        list->head = node->next;
    // 调整后置节点的指针
    if (node->next)
        node->next->prev = node->prev;
    else
        list->tail = node->prev;
    // 释放值
    zfree(node->value);
    // 释放节点
    zfree(node);
    // 链表数减一
    list->len--;
}

/*
 * 获取node节点的下一个节点
 */
listNode *listNext(listNode *node)
{
    listNode *next;
    if((next = zmalloc(sizeof(*next)))==NULL)
        return NULL;
    next = node->next;
    return next;
}



/* 
 * 查找链表 list 中值和 key 匹配的节点。
 * 那么直接通过对比值的指针来决定是否匹配。
 *
 * 如果匹配成功，那么第一个匹配的节点会被返回。
 * 如果没有匹配任何节点，那么返回 NULL 。
 *
 * T = O(N)
 */
listNode *listSearchKey(list *list, void *key)
{
    listNode *node;
    node = list->head;
    listNode *cur;
    while(node != NULL) {
        // 对比
        if (key == node->value) {
            // 找到并返回当前节点
            return node;
        }
        //没有找到则继续迭代
        node = node->next;
    }
    // 未找到
    return NULL;
}

/*
 * 复制整个链表。
 * 复制成功返回输入链表的副本，
 * 如果因为内存不足而造成复制失败，返回 NULL 。
 * 
 * 主要用于多个从节点复制同一主节点时，通过共享回复缓冲区中的内容来减少SAVE的次数
 * T = O(N)
 */
list *listDup(list *orig)
{
    list *copy;
    listNode *node;

    // 创建新链表
    if ((copy = listCreate()) == NULL)
        return NULL;
    // 迭代整个输入链表
    node = orig->head;
    while(node != NULL) {
         // 将节点添加到链表
        if (listAddNodeTail(copy, node->value) == NULL) {
            listRelease(copy);
            return NULL;
        }
        node = node->next;
    }
    // 返回副本
    return copy;
}


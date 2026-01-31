#ifndef __LINK_LIST_H__
#define __LINK_LIST_H__

#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <stddef.h>


#ifndef container_of
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})
#endif

#define LinkList_For_Each_Entry(pos, head, member)                              \
        for (pos = container_of((head)->Next, typeof(*pos), member);    \
             (head)->Next != (head) && pos->member.next != (head);      \
             pos = container_of(pos->member.Next, typeof(*pos), member))

/**
 * 双向链表节点结构体.
 */
typedef struct list_node {
        struct list_node                *Prev;
        struct list_node                *Next;
} ListNode;

/**
 * 双向链表结构体.
 */
typedef struct {
        pthread_mutex_t         Mutex;
        ListNode                        Head;
} LinkList;

/**
 * \brief  初始化一个链表节点。
 */
static inline void LinkList_NodeInit(ListNode *Node)
{
        Node->Next = Node->Prev = Node;
}

void LinkList_Init(LinkList *List);
void LinkList_DeInit(LinkList *List);

void LinkList_PushToHead(LinkList *List, ListNode *New);
void LinkList_PushToTail(LinkList *List, ListNode *New);
void LinkList_Push(LinkList *List, ListNode *Pre, ListNode *Next, ListNode *New);
void LinkList_Pop(LinkList *List, ListNode *Node);
void LinkList_PopFromHead(LinkList *List, ListNode **New);
void LinkList_PopFromTail(LinkList *List, ListNode **New);
int32_t LinkList_IsEmpty(LinkList *List);

// --- 无锁接口 ---
void LinkList_PushToHead_NoLock(LinkList *List, ListNode *New);
void LinkList_PushToTail_NoLock(LinkList *List, ListNode *New);
void LinkList_PopFromHead_NoLock(LinkList *List, ListNode **New);
void LinkList_PopFromTail_NoLock(LinkList *List, ListNode **New);
int32_t LinkList_IsEmpty_NoLock(LinkList *List);

#endif

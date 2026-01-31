#include "link_list.h"

void LinkList_Init(LinkList *List)
{
	pthread_mutex_init(&List->Mutex, NULL);

	pthread_mutex_lock(&List->Mutex);
	List->Head.Prev = (List->Head.Next = &List->Head);
	pthread_mutex_unlock(&List->Mutex);
}

void LinkList_DeInit(LinkList *List)
{
	pthread_mutex_lock(&List->Mutex);
	List->Head.Prev = (List->Head.Next = NULL);
	pthread_mutex_unlock(&List->Mutex);

	pthread_mutex_destroy(&List->Mutex);
}

// ============================================================================
// NoLock Implementation (core logic)
// ============================================================================

void LinkList_PushToHead_NoLock(LinkList *List, ListNode *New)
{
	ListNode *Head = &List->Head;
	New->Next = Head->Next;
	New->Prev = Head;
	Head->Next->Prev = New;
	Head->Next = New;
}

void LinkList_PushToTail_NoLock(LinkList *List, ListNode *New)
{
	ListNode *Head = &List->Head;
	New->Next = Head;
	New->Prev = Head->Prev;
	Head->Prev->Next = New;
	Head->Prev = New;
}

void LinkList_PopFromHead_NoLock(LinkList *List, ListNode **New)
{
	ListNode *Head = &List->Head;
	if (Head->Next == Head) {
		*New = NULL;
	} else {
		*New = Head->Next;
		Head->Next = Head->Next->Next;
		Head->Next->Prev = Head;
		(*New)->Next = (*New)->Prev = *New; // Safety
	}
}

void LinkList_PopFromTail_NoLock(LinkList *List, ListNode **New)
{
	ListNode *Head = &List->Head;
	if (Head->Prev == Head) {
		*New = NULL;
	} else {
		*New = Head->Prev;
		Head->Prev = Head->Prev->Prev;
		Head->Prev->Next = Head;
		(*New)->Next = (*New)->Prev = *New; // Safety
	}
}

int32_t LinkList_IsEmpty_NoLock(LinkList *List)
{
	ListNode *Head = &List->Head;
	return (Head->Next == Head) && (Head->Prev == Head);
}

// ============================================================================
// Standard Implementation (With Mutex)
// ============================================================================

void LinkList_PushToHead(LinkList *List, ListNode *New)
{
	ListNode *Head = &List->Head;
	
	pthread_mutex_lock(&List->Mutex);
	New->Next = Head->Next;
	New->Prev = Head;
	Head->Next->Prev = New;
	Head->Next = New;
	pthread_mutex_unlock(&List->Mutex);
}

void LinkList_PushToTail(LinkList *List, ListNode *New)
{
	ListNode *Head = &List->Head;
	
	pthread_mutex_lock(&List->Mutex);
	New->Next = Head;
	New->Prev = Head->Prev;
	Head->Prev->Next = New;
	Head->Prev = New;
	pthread_mutex_unlock(&List->Mutex);
}

void LinkList_Push(LinkList *List, ListNode *Prev, ListNode *Next, ListNode *New)
{
	pthread_mutex_lock(&List->Mutex);
	New->Next = Next;
	New->Prev = Prev;
	Prev->Next = New;
	Next->Prev = New;
	pthread_mutex_unlock(&List->Mutex);
}

void LinkList_PopFromHead(LinkList *List, ListNode **New)
{
	ListNode *Head = &List->Head;
	
	pthread_mutex_lock(&List->Mutex);
	if (Head->Next == Head) {
		*New = NULL;
	}
	else {
		*New = Head->Next;
		Head->Next = Head->Next->Next;
		Head->Next->Prev = Head;
		(*New)->Next = (*New)->Prev = *New;
	}
	pthread_mutex_unlock(&List->Mutex);
}

void LinkList_PopFromTail(LinkList *List, ListNode **New)
{
	ListNode *Head = &List->Head;
	
	pthread_mutex_lock(&List->Mutex);
	if (Head->Prev == Head) {
		*New = NULL;
	}
	else {
		*New = Head->Prev;
		Head->Prev = Head->Prev->Prev;
		Head->Prev->Next = Head;
		(*New)->Next = (*New)->Prev = *New;
	}
	pthread_mutex_unlock(&List->Mutex);
}

void LinkList_Pop(LinkList *List, ListNode *Node)
{
	pthread_mutex_lock(&List->Mutex);
	Node->Prev->Next = Node->Next;
	Node->Next->Prev = Node->Prev;
	Node->Next = (Node->Prev = Node);
	pthread_mutex_unlock(&List->Mutex);
}

int32_t LinkList_IsEmpty(LinkList *List)
{
	int32_t Ret;
	ListNode *Head = &List->Head;
	
	pthread_mutex_lock(&List->Mutex);
	Ret =  (Head->Next == Head) && (Head->Prev == Head);
	pthread_mutex_unlock(&List->Mutex);

	return Ret;
}

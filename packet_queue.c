#include "packet_queue.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>

#include "log.h"
#ifndef TAG
#define TAG "QUEUE"
#endif

/* ========================================================================== */
/* 内部辅助函数 */
/* ========================================================================== */

// 在 active_list 中查找并移除指定类型的最旧节点 (无锁，需外部持有锁)
static PacketNode* remove_oldest_of_type(PacketQueue *q, PacketType type) {
    LinkList *list = &q->active_list;
    ListNode *head_sentinel = &list->Head;
    ListNode *cur = head_sentinel->Next; // 第一个有效节点

    // 遍历链表 (直到回到哨兵节点)
    while (cur != head_sentinel) {
        PacketNode *pn = (PacketNode *)cur;
        
        if (pn->type == type) {
            // 找到同类型的最旧节点
            // 双向链表摘除节点
            cur->Prev->Next = cur->Next;
            cur->Next->Prev = cur->Prev;
            
            // 重置指针指向自己，防止悬空
            cur->Next = cur->Prev = cur;
            return pn;
        }
        cur = cur->Next;
    }
    return NULL;
}

/* ========================================================================== */
/* API 实现 */
/* ========================================================================== */

void packet_queue_init(PacketQueue *q, int max_video, int max_audio)
{
    if (!q) return;
    if (max_video <= 0) max_video = QUEUE_CAPACITY_VIDEO_DEFAULT;
    if (max_audio <= 0) max_audio = QUEUE_CAPACITY_AUDIO_DEFAULT;

    memset(q, 0, sizeof(PacketQueue));
    
    LinkList_Init(&q->active_list);
    LinkList_Init(&q->free_list_video);
    LinkList_Init(&q->free_list_audio);
    
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->abort_request = 0;
    
    // --- 1. Init Video Pool ---
    q->cap_video = max_video;
    q->pool_video = (PacketNode *)calloc(max_video, sizeof(PacketNode));
    if (q->pool_video) {
        for (int i = 0; i < max_video; i++) {
            av_init_packet(&q->pool_video[i].pkt);
            q->pool_video[i].type = PKT_TYPE_VIDEO;
            LinkList_NodeInit(&q->pool_video[i].node);
            LinkList_PushToTail_NoLock(&q->free_list_video, &q->pool_video[i].node);
        }
    } else {
        LOG_ERROR(TAG, "Failed to alloc video pool (cnt=%d)\n", max_video);
    }

    // --- 2. Init Audio Pool ---
    q->cap_audio = max_audio;
    q->pool_audio = (PacketNode *)calloc(max_audio, sizeof(PacketNode));
    if (q->pool_audio) {
        for (int i = 0; i < max_audio; i++) {
            av_init_packet(&q->pool_audio[i].pkt);
            q->pool_audio[i].type = PKT_TYPE_AUDIO;
            LinkList_NodeInit(&q->pool_audio[i].node);
            LinkList_PushToTail_NoLock(&q->free_list_audio, &q->pool_audio[i].node);
        }
    } else {
        LOG_ERROR(TAG, "Failed to alloc audio pool (cnt=%d)\n", max_audio);
    }
    
    LOG_INFO(TAG, "Queue Inited. VideoCap:%d, AudioCap:%d\n", max_video, max_audio);
}

void packet_queue_destroy(PacketQueue *q)
{
    if (!q) return;
    
    packet_queue_abort(q);
    pthread_mutex_lock(&q->mutex);

    // Free Video Pool Payloads
    if (q->pool_video) {
        for (int i = 0; i < q->cap_video; i++) {
            av_packet_unref(&q->pool_video[i].pkt);
        }
        free(q->pool_video);
        q->pool_video = NULL;
    }

    // Free Audio Pool Payloads
    if (q->pool_audio) {
        for (int i = 0; i < q->cap_audio; i++) {
            av_packet_unref(&q->pool_audio[i].pkt);
        }
        free(q->pool_audio);
        q->pool_audio = NULL;
    }

    pthread_mutex_unlock(&q->mutex);
    
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
    
    LinkList_DeInit(&q->active_list);
    LinkList_DeInit(&q->free_list_video);
    LinkList_DeInit(&q->free_list_audio);
}

void packet_queue_abort(PacketQueue *q)
{
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    q->abort_request = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mutex);
}

void packet_queue_get_stats(PacketQueue *q, int *size, int *nb_packets)
{
    if (!q) return;
    pthread_mutex_lock(&q->mutex);
    if (size)       *size       = q->size_bytes;
    if (nb_packets) *nb_packets = q->count_video + q->count_audio;
    pthread_mutex_unlock(&q->mutex);
}

// 清空队列 (用于切流/暂停时丢弃旧数据)
void packet_queue_flush(PacketQueue *q)
{
    ListNode *node = NULL;
    PacketNode *pnode = NULL;

    if (!q) return;

    pthread_mutex_lock(&q->mutex);
    
    // 循环取出所有节点并归还到 FreeList
    while (1) {
        LinkList_PopFromHead_NoLock(&q->active_list, &node);
        if (node == NULL) {
            break; // 队列已空
        }

        pnode = (PacketNode *)node;
        q->size_bytes -= pnode->pkt.size;
        
        // 释放数据负载 (Data Payload)
        av_packet_unref(&pnode->pkt);
        // 重置包参数
        av_init_packet(&pnode->pkt);
        pnode->pkt.data = NULL;
        pnode->pkt.size = 0;

        // 归还到对应的 FreeList
        if (pnode->type == PKT_TYPE_VIDEO) {
            q->count_video--;
            LinkList_PushToTail_NoLock(&q->free_list_video, node);
        } else {
            q->count_audio--;
            LinkList_PushToTail_NoLock(&q->free_list_audio, node);
        }
    }
    
    pthread_mutex_unlock(&q->mutex);
}

int packet_queue_put(PacketQueue *q, AVPacket *pkt, PacketType type)
{
    PacketNode *pnode = NULL;
    ListNode *node = NULL;
    LinkList *target_free_list = NULL;
    int dropped = 0;

    if (!q || !pkt) return -1;
    if (type != PKT_TYPE_VIDEO && type != PKT_TYPE_AUDIO) {
        type = PKT_TYPE_VIDEO; // Default
    }

    pthread_mutex_lock(&q->mutex);

    if (q->abort_request) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    // 1. Select Free List
    if (type == PKT_TYPE_VIDEO) {
        target_free_list = &q->free_list_video;
    } else {
        target_free_list = &q->free_list_audio;
    }

    // 2. Try to get free node
    LinkList_PopFromHead_NoLock(target_free_list, &node);

    if (node == NULL) {
        // --- Pool FULL ---
        // 策略：丢弃同类型最旧的包 (Smart Drop)
        pnode = remove_oldest_of_type(q, type);
        
        if (pnode) {
            // Free payload
            q->size_bytes -= pnode->pkt.size;
            av_packet_unref(&pnode->pkt);
            
            if (type == PKT_TYPE_VIDEO) q->count_video--;
            else q->count_audio--;
            
            dropped = 1;
            node = (ListNode*)pnode; // Reuse node
        } else {
            pthread_mutex_unlock(&q->mutex);
            // LOG_ERROR(TAG, "Queue Alloc Error: Pool full!\n");
            return -1;
        }
    }

    pnode = (PacketNode *)node;

    // 3. Move data
    if (av_packet_ref(&pnode->pkt, pkt) < 0) {
        LinkList_PushToTail_NoLock(target_free_list, node);
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }
    
    pnode->type = type;

    // 4. Add to Active List
    LinkList_PushToTail_NoLock(&q->active_list, node);
    
    q->size_bytes += pnode->pkt.size;
    if (type == PKT_TYPE_VIDEO) q->count_video++;
    else q->count_audio++;

    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mutex);

    if (dropped) {
        static int log_cnt = 0;
        if (log_cnt++ % 100 == 0) {
            LOG_WARN(TAG, "Queue Full (%s), dropped oldest! V:%d/%d A:%d/%d\n", 
                     (type == PKT_TYPE_VIDEO) ? "Video" : "Audio",
                     q->count_video, q->cap_video, q->count_audio, q->cap_audio);
        }
    }

    return 0;
}

int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block)
{
    ListNode *node = NULL;
    PacketNode *pnode = NULL;
    int ret = -1;

    if (!q) return -1;

    pthread_mutex_lock(&q->mutex);

    for (;;) {
        if (q->abort_request) {
            ret = -1;
            break;
        }

        // Pop from head (FIFO)
        LinkList_PopFromHead_NoLock(&q->active_list, &node);

        if (node != NULL) {
            pnode = (PacketNode *)node;
            
            q->size_bytes -= pnode->pkt.size;
            if (pnode->type == PKT_TYPE_VIDEO) q->count_video--;
            else q->count_audio--;

            *pkt = pnode->pkt; // Move data
            
            // Reset node
            av_init_packet(&pnode->pkt); 
            pnode->pkt.data = NULL;
            pnode->pkt.size = 0;
            
            // Return to Free List
            if (pnode->type == PKT_TYPE_VIDEO) {
                LinkList_PushToTail_NoLock(&q->free_list_video, node);
            } else {
                LinkList_PushToTail_NoLock(&q->free_list_audio, node);
            }
            
            ret = 1;
            break;
        } else if (!block) {
            ret = 0;
            break;
        } else {
            pthread_cond_wait(&q->cond, &q->mutex);
        }
    }

    pthread_mutex_unlock(&q->mutex);
    return ret;
}

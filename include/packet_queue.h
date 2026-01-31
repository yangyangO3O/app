#ifndef __PACKET_QUEUE_H__
#define __PACKET_QUEUE_H__

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <pthread.h>
#include "link_list.h"

// 默认容量配置
// 视频帧较大，数量相对少；音频帧极小，但数量多，需要足够深度的队列防止爆音
#define QUEUE_CAPACITY_VIDEO_DEFAULT 2048
#define QUEUE_CAPACITY_AUDIO_DEFAULT 1024

// 包类型枚举，用于选择内存池
typedef enum {
    PKT_TYPE_VIDEO = 0,
    PKT_TYPE_AUDIO = 1,
    PKT_TYPE_OTHER = 2
} PacketType;

// 队列节点
typedef struct {
    ListNode node;      // 链表节点接口 (必须是第一个成员)
    AVPacket pkt;       // FFmpeg 数据包
    PacketType type;    // 标记该节点属于哪个内存池
} PacketNode;

// 队列控制块
typedef struct {
    LinkList active_list;      // 活跃队列 (混合了音视频，保持严格的时间入队顺序)

    // --- 视频专用池 (LargeBlock) ---
    LinkList free_list_video;  // 视频空闲节点链表
    PacketNode *pool_video;    // 视频内存池首地址
    int cap_video;             // 视频容量上限
    int count_video;           // 当前视频包数量

    // --- 音频专用池 (SmallBlock) ---
    LinkList free_list_audio;  // 音频空闲节点链表
    PacketNode *pool_audio;    // 音频内存池首地址
    int cap_audio;             // 音频容量上限
    int count_audio;           // 当前音频包数量

    int size_bytes;            // 总数据字节数 (统计用)
    int abort_request;         // 退出标志

    pthread_mutex_t mutex;
    pthread_cond_t cond;
} PacketQueue;

// --- API 接口 ---

// 初始化：支持分别设置视频和音频的队列深度
void packet_queue_init(PacketQueue *q, int max_video, int max_audio);

// 销毁：释放所有内存池资源
void packet_queue_destroy(PacketQueue *q);

// 中止：唤醒所有阻塞线程
void packet_queue_abort(PacketQueue *q);

// 入队：需指定包类型
int packet_queue_put(PacketQueue *q, AVPacket *pkt, PacketType type);

// 出队：block=1 为阻塞模式
int packet_queue_get(PacketQueue *q, AVPacket *pkt, int block);

// 获取统计信息
void packet_queue_get_stats(PacketQueue *q, int *size, int *nb_packets);
void packet_queue_flush(PacketQueue *q);

// 宏映射 (保持部分兼容性)
#define PacketQueue_Init        packet_queue_init
#define PacketQueue_Destroy     packet_queue_destroy
#define PacketQueue_Abort       packet_queue_abort
#define PacketQueue_Put         packet_queue_put
#define PacketQueue_Get         packet_queue_get
#define PacketQueue_GetStats    packet_queue_get_stats

#endif

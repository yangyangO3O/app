#ifndef __STREAM_H__
#define __STREAM_H__

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "common.h"
#include "camera_manage.h"
#include "packet_queue.h"

typedef struct rtsp_ctx {
        StreamHandle *Stream;
        pthread_t Thread;
        int32_t running;
        int32_t TransProto;  //1: TCP, 0: UDP
    PacketQueue RecordQueue;
    PacketQueue P2pQueue;
        char url[64];
    AVFormatContext *AvFmtCtx;
        int32_t AdIndex;
        int32_t VdIndex;
    int CamIndex;
        int32_t   thread_created;

        // 【新增】暂停控制标志位
        // 0 = 正常运行, 1 = 暂停推流(但保持RTSP连接)
        volatile int paused;
} RtspCtx;

struct StreamHandle {
        StationHandle  *Station;
        RtspCtx         Rtsp[CAM_MAX_CNT];
        pthread_mutex_t Mutex;
        void            *Priv[0];
};

int32_t Stream_Init(StationHandle *Station);
void Stream_Deinit(StationHandle *Station);
int32_t Stream_Start(StationHandle *Station, int32_t Index);
int32_t Stream_Stop(StationHandle *Station, int32_t Index);
int32_t Stream_RequestIFrame(StationHandle *Station, int32_t Index);
int32_t Stream_SetPause(StationHandle *Station, int32_t Index, int32_t Pause);

#endif

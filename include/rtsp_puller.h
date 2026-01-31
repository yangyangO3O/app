/* rtsp_puller.h */
#ifndef _RTSP_PULLER_H_
#define _RTSP_PULLER_H_

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

typedef struct RtspPuller RtspPuller;

/**
 * Info 回调：
 * 与 main_app.c 中 on_rtsp_info_cb 完全一致的签名！
 */
typedef void (*RtspInfoCallback)(
        int video_stream_index,  AVCodecParameters *video_par,  AVRational video_timebase,
        int audio_stream_index,  AVCodecParameters *audio_par,  AVRational audio_timebase,
        void *user_data);

/**
 * Packet 回调：
 * 每读到一个视频/音频包就调用一次
 */
typedef void (*RtspPacketCallback)(AVPacket *pkt, void *user_data);

RtspPuller* RtspPuller_Create(const char *url,
                              RtspInfoCallback    cb_info,
                              RtspPacketCallback cb_pkt,
                              void *user_data);
void RtspPuller_Stop   (RtspPuller *ctx);
void RtspPuller_Destroy(RtspPuller *ctx);

#endif

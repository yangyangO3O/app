// rtsp_forwarder.h
#ifndef _RTSP_FORWARDER_H_
#define _RTSP_FORWARDER_H_

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RtspForwarder RtspForwarder;

RtspForwarder* RtspForwarder_Create(const char* url,
                                    int v_in_index, AVCodecParameters* v_par, AVRational v_time_base,
                                    int a_in_index, AVCodecParameters* a_par, AVRational a_time_base);

int  RtspForwarder_Write  (RtspForwarder* ctx, AVPacket* pkt);
void RtspForwarder_Destroy(RtspForwarder* ctx);

#ifdef __cplusplus
}
#endif

#endif

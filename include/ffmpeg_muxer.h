/* ffmpeg_muxer.h */
#ifndef FFMPEG_MUXER_H
#define FFMPEG_MUXER_H

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

typedef struct FFmpegMuxer FFmpegMuxer;

int FFmpegMuxer_Create(FFmpegMuxer **ppctx,
                       const char *filename,
                       const AVCodecParameters *vpar, AVRational v_tb,
                       const AVCodecParameters *apar, AVRational a_tb);

int FFmpegMuxer_Write(FFmpegMuxer *ctx, AVPacket *pkt,
                      int is_video, int is_audio);

void FFmpegMuxer_Close(FFmpegMuxer *ctx);

#endif

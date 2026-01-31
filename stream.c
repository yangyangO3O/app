#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include <sys/prctl.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>

#include "system.h"
#include "stream.h"
#include "packet_queue.h"
#include "camera_manage.h"
#include "record.h"
#include "p2p.h"
#include "log.h"
#include "network.h"

#define TAG "STREAM"
#define ENABLE_MP4_RECORD 1
#define RTSP_PORT 1234
#define SAMPLING_RATE 16000
#define H264_RBSP_BUF_SIZE 4096
#define NAL_TYPE_SLICE      1
#define NAL_TYPE_SLICE_IDR  5

// ==========================================
// Helper Structures
// ==========================================
typedef struct {
    const uint8_t *data;
    int size;
    int index; 
} GetBitContext;

typedef struct {
    int initialized;
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
    SwrContext *swr_ctx;
    AVAudioFifo *fifo;
    AVFrame *dec_frame;
    AVFrame *enc_frame;
    int64_t pts_counter;
} AudioTranscoder;

/* ========================================================================== */
/* H.264 Parsing Helpers (SPS/PPS/Keyframe)                                   */
/* ========================================================================== */

static void init_get_bits(GetBitContext *s, const uint8_t *buffer, int bit_len) {
    s->data = buffer;
    s->size = bit_len / 8;
    s->index = 0;
}

static int get_bits(GetBitContext *s, int n) {
    int val = 0;
    for (int i = 0; i < n; i++) {
        int byte_idx = s->index >> 3;
        int bit_idx  = 7 - (s->index & 7);
        if (byte_idx >= s->size) return 0;
        int bit = (s->data[byte_idx] >> bit_idx) & 1;
        val = (val << 1) | bit;
        s->index++;
    }
    return val;
}

static int get_1bit(GetBitContext *s) { return get_bits(s, 1); }

static unsigned int get_ue_golomb(GetBitContext *s) {
    int zeros = 0;
    while (get_1bit(s) == 0 && zeros < 32 && (s->index >> 3) < s->size) {
        zeros++;
    }
    if (zeros == 0) return 0;
    unsigned int val = get_bits(s, zeros);
    return (1U << zeros) - 1 + val;
}

static int get_se_golomb(GetBitContext *s) {
    unsigned int ue = get_ue_golomb(s);
    int val = (ue + 1) >> 1;
    return (ue & 1) ? val : -val;
}

static int EbspToRbsp(const uint8_t *src, int src_len, uint8_t *dst) {
    int i, j = 0;
    for (i = 0; i < src_len; i++) {
        if (i >= 2 && src[i] == 0x03 && src[i-1] == 0x00 && src[i-2] == 0x00) {
            continue; 
        }
        dst[j++] = src[i];
    }
    return j;
}

static int H264SpsGetWh(const uint8_t *sps, int sps_len, int *out_w, int *out_h)
{
    if (!sps || sps_len < 4) return -1;
    if (sps_len > H264_RBSP_BUF_SIZE) sps_len = H264_RBSP_BUF_SIZE;

    const uint8_t *nal_start = sps;
    int nal_len = sps_len;

    if (sps[0] == 0 && sps[1] == 0) {
        if (sps[2] == 1) { nal_start += 3; nal_len -= 3; }
        else if (sps[2] == 0 && sps[3] == 1) { nal_start += 4; nal_len -= 4; }
    }
    
    if (nal_len > 0) { nal_start++; nal_len--; }
    if (nal_len <= 0) return -1;

    uint8_t rbsp[H264_RBSP_BUF_SIZE];
    int rbsp_len = EbspToRbsp(nal_start, nal_len, rbsp); 

    GetBitContext gb;
    init_get_bits(&gb, rbsp, rbsp_len * 8);

    int profile_idc = get_bits(&gb, 8);
    get_bits(&gb, 8); 
    get_bits(&gb, 8); 
    get_ue_golomb(&gb); 

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44  || profile_idc == 83  ||
        profile_idc == 86  || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134) 
    {
        int chroma_format_idc = get_ue_golomb(&gb);
        if (chroma_format_idc == 3) get_1bit(&gb);
        get_ue_golomb(&gb); 
        get_ue_golomb(&gb); 
        get_1bit(&gb);      
        
        int seq_scaling_matrix_present_flag = get_1bit(&gb);
        if (seq_scaling_matrix_present_flag) {
            int limit = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < limit; i++) {
                if (get_1bit(&gb)) { 
                    int last_scale = 8;
                    int next_scale = 8;
                    int count = (i < 6) ? 16 : 64;
                    for (int j = 0; j < count; j++) {
                        if (next_scale != 0) {
                            int delta_scale = get_se_golomb(&gb);
                            next_scale = (last_scale + delta_scale + 256) % 256;
                        }
                        last_scale = (next_scale == 0) ? last_scale : next_scale;
                    }
                }
            }
        }
    }

    get_ue_golomb(&gb); 
    int pic_order_cnt_type = get_ue_golomb(&gb);
    if (pic_order_cnt_type == 0) {
        get_ue_golomb(&gb); 
    } else if (pic_order_cnt_type == 1) {
        get_1bit(&gb); 
        get_se_golomb(&gb); 
        get_se_golomb(&gb); 
        int num = get_ue_golomb(&gb);
        for (int i = 0; i < num; i++) get_se_golomb(&gb); 
    }

    get_ue_golomb(&gb); 
    get_1bit(&gb);      
    
    int pic_width_in_mbs_minus1 = get_ue_golomb(&gb);
    int pic_height_in_map_units_minus1 = get_ue_golomb(&gb);
    int frame_mbs_only_flag = get_1bit(&gb);
    
    if (!frame_mbs_only_flag) get_1bit(&gb); 
    get_1bit(&gb); 
    
    int frame_cropping_flag = get_1bit(&gb);
    int crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (frame_cropping_flag) {
        crop_left   = get_ue_golomb(&gb);
        crop_right  = get_ue_golomb(&gb);
        crop_top    = get_ue_golomb(&gb);
        crop_bottom = get_ue_golomb(&gb);
    }

    int width  = (pic_width_in_mbs_minus1 + 1) * 16;
    int height = (pic_height_in_map_units_minus1 + 1) * 16 * (2 - frame_mbs_only_flag);
    int crop_unit_x = 2; 
    int crop_unit_y = 2 * (2 - frame_mbs_only_flag);
    width  -= (crop_left + crop_right) * crop_unit_x;
    height -= (crop_top + crop_bottom) * crop_unit_y;

    if (width <= 0 || height <= 0 || width > 4096 || height > 2160) return -1;

    *out_w = width;
    *out_h = height;
    return 0;
}

static int H264CheckSliceIsI(const uint8_t *nal_payload, int len)
{
    if (len < 2) return 0;
    const uint8_t *ptr = nal_payload + 1;
    int size = len - 1;
    GetBitContext gb;
    uint8_t rbsp_head[32]; 
    int rbsp_len = EbspToRbsp(ptr, (size > 32 ? 32 : size), rbsp_head);
    init_get_bits(&gb, rbsp_head, rbsp_len * 8);
    get_ue_golomb(&gb); 
    unsigned int slice_type = get_ue_golomb(&gb);
    slice_type %= 5; 
    return (slice_type == 2); 
}

static int H264_Scan_KeyFrame(const uint8_t *p, int size)
{
    if (!p || size < 5) return 0;
    int i;
    for (i = 0; i < size - 5; i++) {
        if (p[i] == 0 && p[i+1] == 0) {
            if (p[i+2] == 1) { 
                int type = p[i+3] & 0x1F;
                if (type == NAL_TYPE_SLICE_IDR) return 1;
                if (type == NAL_TYPE_SLICE && H264CheckSliceIsI(&p[i+3], size - (i+3))) return 1;
            } else if (p[i+2] == 0 && p[i+3] == 1) { 
                int type = p[i+4] & 0x1F;
                if (type == NAL_TYPE_SLICE_IDR) return 1;
                if (type == NAL_TYPE_SLICE && H264CheckSliceIsI(&p[i+4], size - (i+4))) return 1;
            }
        }
    }
    return 0;
}

static int AvccGetFirstSps(const uint8_t *extra, int extra_size, const uint8_t **sps, int *sps_len)
{
    int num_sps, pos, i;
    if (!extra || extra_size < 7) return -1;
    if (extra[0] != 1) return -1;
    num_sps = extra[5] & 0x1f;
    pos = 6;
    for (i = 0; i < num_sps; i++) {
        int len; if (pos + 2 > extra_size) return -1;
        len = (extra[pos] << 8) | extra[pos + 1]; pos += 2;
        if (pos + len > extra_size) return -1;
        if (len > 0 && ((extra[pos] & 0x1f) == 7)) { 
            *sps = &extra[pos];
            *sps_len = len;
            return 0;
        }
        pos += len;
    }
    return -1;
}

/* ========================================================================== */
/* Audio Transcoder                                                           */
/* ========================================================================== */

static void FreeTranscoder(AudioTranscoder *tc) {
    if (!tc) return;
    if (tc->dec_ctx) avcodec_free_context(&tc->dec_ctx);
    if (tc->enc_ctx) avcodec_free_context(&tc->enc_ctx);
    if (tc->swr_ctx) swr_free(&tc->swr_ctx);
    if (tc->fifo)    av_audio_fifo_free(tc->fifo);
    if (tc->dec_frame) av_frame_free(&tc->dec_frame);
    if (tc->enc_frame) av_frame_free(&tc->enc_frame);
    memset(tc, 0, sizeof(AudioTranscoder));
}

static int InitTranscoder(AudioTranscoder *tc, AVCodecParameters *in_par) {
    int ret;
    AVCodec *dec = avcodec_find_decoder(in_par->codec_id);
    AVCodec *enc = avcodec_find_encoder(AV_CODEC_ID_AAC); 
    if (!enc) enc = avcodec_find_encoder_by_name("aac");

    if (!enc || !dec) return -1;

    tc->dec_ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(tc->dec_ctx, in_par);
    if ((ret = avcodec_open2(tc->dec_ctx, dec, NULL)) < 0) return ret;

    tc->enc_ctx = avcodec_alloc_context3(enc);
    tc->enc_ctx->sample_rate = in_par->sample_rate > 0 ? in_par->sample_rate : SAMPLING_RATE;
    tc->enc_ctx->channel_layout = AV_CH_LAYOUT_MONO;
    tc->enc_ctx->channels = 1;
    tc->enc_ctx->sample_fmt = enc->sample_fmts ? enc->sample_fmts[0] : AV_SAMPLE_FMT_FLTP;
    tc->enc_ctx->bit_rate = 32000; 
    tc->enc_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL; 
    tc->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    if ((ret = avcodec_open2(tc->enc_ctx, enc, NULL)) < 0) return ret;

    tc->swr_ctx = swr_alloc();
    av_opt_set_int(tc->swr_ctx, "in_channel_layout",  tc->dec_ctx->channel_layout ? tc->dec_ctx->channel_layout : av_get_default_channel_layout(tc->dec_ctx->channels), 0);
    av_opt_set_int(tc->swr_ctx, "in_sample_rate",     tc->dec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(tc->swr_ctx, "in_sample_fmt", tc->dec_ctx->sample_fmt, 0);

    av_opt_set_int(tc->swr_ctx, "out_channel_layout", tc->enc_ctx->channel_layout, 0);
    av_opt_set_int(tc->swr_ctx, "out_sample_rate",    tc->enc_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(tc->swr_ctx, "out_sample_fmt", tc->enc_ctx->sample_fmt, 0);

    if ((ret = swr_init(tc->swr_ctx)) < 0) return ret;

    tc->fifo = av_audio_fifo_alloc(tc->enc_ctx->sample_fmt, tc->enc_ctx->channels, 1024 * 50);
    tc->dec_frame = av_frame_alloc();
    tc->enc_frame = av_frame_alloc();
    tc->enc_frame->nb_samples = tc->enc_ctx->frame_size;
    tc->enc_frame->format = tc->enc_ctx->sample_fmt;
    tc->enc_frame->channel_layout = tc->enc_ctx->channel_layout;
    tc->enc_frame->sample_rate = tc->enc_ctx->sample_rate;
    
    if ((ret = av_frame_get_buffer(tc->enc_frame, 0)) < 0) return ret;

    tc->initialized = 1;
    tc->pts_counter = 0;
    return 0;
}

/* ========================================================================== */
/* Main Stream Logic                                                          */
/* ========================================================================== */

static inline int64_t NowMsMonotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000LL + (int64_t)ts.tv_nsec / 1000000LL;
}

static int32_t FfmpegInterruptCb(void *opaque) {
    RtspCtx *ctx = (RtspCtx *)opaque;
    return ctx ? (ctx->running == 0) : 1;
}

static void* Stream_RtspThread(void *Arg) {
    RtspCtx *ctx = (RtspCtx *)Arg;
    AVDictionary *opts = NULL;
    AVPacket pkt;
    int ret;
    AudioTranscoder tc = {0};
    
    // [FIX] Monotonic Timestamp Variables (Preserved)
    int64_t last_valid_pts = 0;
    int64_t pts_offset = 0;
    int is_first_connection = 1;
    
    prctl(PR_SET_NAME, "RTSP_Worker");
    av_init_packet(&pkt);

    while (ctx->running) {
        // [REMOVED] Low Power / Weak Signal Detection Logic

        ctx->running = 1; 
        ctx->VdIndex = -1;
        ctx->AdIndex = -1;
        
        FreeTranscoder(&tc); 

        ctx->AvFmtCtx = avformat_alloc_context();
        ctx->AvFmtCtx->interrupt_callback.callback = FfmpegInterruptCb;
        ctx->AvFmtCtx->interrupt_callback.opaque = ctx;
        ctx->AvFmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
        
        if (ctx->TransProto) av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "stimeout", "5000000", 0); 
        av_dict_set(&opts, "max_delay", "500000", 0);
        av_dict_set(&opts, "buffer_size", "1024000", 0);

        LOG_INFO(TAG, "[Ch%d] Connecting to %s...\n", ctx->CamIndex, ctx->url);
        
        ret = avformat_open_input(&ctx->AvFmtCtx, ctx->url, NULL, &opts);
        av_dict_free(&opts);

        if (ret < 0) {
            // [LOGGING] Keep useful signal logging
            int rssi=0, evm=0, rate=0;
            Network_GetHalowState(((StreamHandle*)ctx->Stream)->Station, &rssi, &evm, &rate);
            LOG_WARN(TAG, "[Ch%d] Open failed: %d. RSSI:%d EVM:%d Rate:%dKbps. Retry in 2s...\n", 
                     ctx->CamIndex, ret, rssi, evm, rate);
                     
            if (ctx->AvFmtCtx) avformat_close_input(&ctx->AvFmtCtx);
            ctx->AvFmtCtx = NULL;
            sleep(2); // [REVERT] Fixed 2s sleep
            continue;
        }

        if (avformat_find_stream_info(ctx->AvFmtCtx, NULL) < 0) {
            LOG_WARN(TAG, "[Ch%d] Find stream info failed\n", ctx->CamIndex);
            avformat_close_input(&ctx->AvFmtCtx);
            continue;
        }

        for (int i = 0; i < ctx->AvFmtCtx->nb_streams; i++) {
            if (ctx->AvFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && ctx->VdIndex < 0)
                ctx->VdIndex = i;
            else if (ctx->AvFmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ctx->AdIndex < 0)
                ctx->AdIndex = i;
        }

        if (ctx->VdIndex < 0) {
            LOG_ERROR(TAG, "[Ch%d] No Video Stream! Retry...\n", ctx->CamIndex);
            avformat_close_input(&ctx->AvFmtCtx);
            sleep(1);
            continue;
        }

        if (ctx->AdIndex >= 0) {
            AVStream *ast = ctx->AvFmtCtx->streams[ctx->AdIndex];
            if (ast->codecpar->codec_id != AV_CODEC_ID_AAC) {
                if (InitTranscoder(&tc, ast->codecpar) == 0) {
                    avcodec_parameters_from_context(ast->codecpar, tc.enc_ctx);
                    ast->time_base = (AVRational){1, tc.enc_ctx->sample_rate};
                }
            }
        }

        AVStream *vst = ctx->AvFmtCtx->streams[ctx->VdIndex];
        if (vst->codecpar->width == 0 && vst->codecpar->extradata_size > 0) {
            const uint8_t *sps = NULL; int sps_len = 0;
            if (AvccGetFirstSps(vst->codecpar->extradata, vst->codecpar->extradata_size, &sps, &sps_len) == 0) {
                int w=0, h=0;
                if (H264SpsGetWh(sps, sps_len, &w, &h) == 0) {
                    vst->codecpar->width = w; vst->codecpar->height = h;
                    LOG_INFO(TAG, "[Ch%d] SPS Parsed: %dx%d\n", ctx->CamIndex, w, h);
                }
            }
        }

        LOG_INFO(TAG, "[Ch%d] Running. V:%d A:%d\n", ctx->CamIndex, ctx->VdIndex, ctx->AdIndex);
        ctx->running = 2; 
        
        Stream_RequestIFrame(((StreamHandle*)ctx->Stream)->Station, ctx->CamIndex);

        // [FIX] Calculate timestamp offset on reconnection
        if (!is_first_connection) {
            pts_offset = last_valid_pts + 3000; 
            LOG_INFO(TAG, "[Ch%d] Reconnection detected. Preparing PTS offset base: %lld\n", ctx->CamIndex, last_valid_pts);
        }
        int calc_offset_flag = 1;

        while (ctx->running == 2) {
            ret = av_read_frame(ctx->AvFmtCtx, &pkt);
            if (ret < 0) {
                LOG_WARN(TAG, "[Ch%d] EOF/Error: %d\n", ctx->CamIndex, ret);
                break; 
            }

            // [FIX] Monotonic Timestamp Correction
            if (pkt.pts != AV_NOPTS_VALUE) {
                if (!is_first_connection && calc_offset_flag) {
                    pts_offset = (last_valid_pts + 3000) - pkt.pts;
                    calc_offset_flag = 0;
                    LOG_INFO(TAG, "[Ch%d] Calculated PTS Offset: %lld\n", ctx->CamIndex, pts_offset);
                }
                
                pkt.pts += pts_offset;
                pkt.dts += pts_offset;
                
                if (pkt.pts > last_valid_pts) {
                    last_valid_pts = pkt.pts;
                }
            }

            if (pkt.stream_index == ctx->VdIndex) {
                if (!(pkt.flags & AV_PKT_FLAG_KEY)) {
                    if (H264_Scan_KeyFrame(pkt.data, pkt.size)) {
                        pkt.flags |= AV_PKT_FLAG_KEY;
                    }
                }
            }

            if (pkt.stream_index == ctx->VdIndex) {
                #ifdef ENABLE_MP4_RECORD
                packet_queue_put(&ctx->RecordQueue, &pkt, PKT_TYPE_VIDEO);
                #endif
                if (!ctx->paused) {
                    packet_queue_put(&ctx->P2pQueue, &pkt, PKT_TYPE_VIDEO);
                }
                av_packet_unref(&pkt);
            } 
            else if (pkt.stream_index == ctx->AdIndex) {
                if (tc.initialized) {
                    int send_ret = avcodec_send_packet(tc.dec_ctx, &pkt);
                    if (send_ret >= 0) {
                        while (avcodec_receive_frame(tc.dec_ctx, tc.dec_frame) >= 0) {
                            // Transcoding logic placeholder
                        }
                    }
                    av_packet_unref(&pkt);
                } else {
                    #ifdef ENABLE_MP4_RECORD
                    packet_queue_put(&ctx->RecordQueue, &pkt, PKT_TYPE_AUDIO);
                    #endif
                    if (!ctx->paused) {
                        packet_queue_put(&ctx->P2pQueue, &pkt, PKT_TYPE_AUDIO);
                    }
                    av_packet_unref(&pkt);
                }
            } else {
                av_packet_unref(&pkt);
            }
        }

        if (ctx->AvFmtCtx) {
            avformat_close_input(&ctx->AvFmtCtx);
            ctx->AvFmtCtx = NULL;
        }
        
        FreeTranscoder(&tc); 
        is_first_connection = 0; 

        if (ctx->running == 0) break;
        ctx->running = 1; 
        sleep(1);
    }

    packet_queue_abort(&ctx->RecordQueue);
    packet_queue_abort(&ctx->P2pQueue);
    FreeTranscoder(&tc);
    
    LOG_INFO(TAG, "[Ch%d] Thread Exit\n", ctx->CamIndex);
    pthread_exit(NULL);
}

int32_t Stream_Init(StationHandle *Station) {
    StreamHandle *Stream = calloc(1, sizeof(StreamHandle));
    if (!Stream) return -1;
    
    Stream->Station = Station;
    ((StationHandle*)Station)->Stream = Stream;
    pthread_mutex_init(&Stream->Mutex, NULL);
    
    av_register_all();
    avformat_network_init();
    av_log_set_level(AV_LOG_ERROR);

    return 0;
}

void Stream_Deinit(StationHandle *Station) {
    if (Station->Stream) {
        StreamHandle *Stream = Station->Stream;
        for (int i = 0; i < CAM_MAX_CNT; i++) {
            Stream_Stop(Station, i);
        }
        pthread_mutex_destroy(&Stream->Mutex);
        free(Stream);
        Station->Stream = NULL;
    }
    avformat_network_deinit();
}

int32_t Stream_Start(StationHandle *Station, int32_t Index) {
    StreamHandle *Stream = Station->Stream;
    CamManageHandle *CamManage = Station->CameraMag;
    
    if (Index < 0 || Index >= CAM_MAX_CNT) return -1;
    
    Stream_Stop(Station, Index);
    
    RtspCtx *ctx = &Stream->Rtsp[Index];
    memset(ctx, 0, sizeof(RtspCtx));
    
    ctx->Stream = Stream;
    ctx->CamIndex = Index;
    ctx->running = 1;
    ctx->paused = 0; 
    ctx->TransProto = 1; 

    P2pHandle *p2p = (P2pHandle *)((StationHandle*)Station)->P2p;
    const char *user = (p2p && strlen(p2p->User)>0) ? p2p->User : "admin";
    const char *pwd  = (p2p && strlen(p2p->Passwd)>0) ? p2p->Passwd : "888888";
    
    snprintf(ctx->url, sizeof(ctx->url), "rtsp://%s:%s@%s:%d/live/ch0", 
             user, pwd, CamManage->Camera[Index].Addr, RTSP_PORT);

    packet_queue_init(&ctx->RecordQueue, 200, 250); 
    packet_queue_init(&ctx->P2pQueue, 60, 80);

    if (pthread_create(&ctx->Thread, NULL, Stream_RtspThread, ctx) != 0) {
        return -1;
    }
    ctx->thread_created = 1;

    #ifdef ENABLE_MP4_RECORD
    Record_Start(Station, Index);
    #endif
    P2P_Start(Station, Index);

    return 0;
}

int32_t Stream_Stop(StationHandle *Station, int32_t Index) {
    StreamHandle *Stream = Station->Stream;
    if (Index < 0 || Index >= CAM_MAX_CNT) return -1;

    RtspCtx *ctx = &Stream->Rtsp[Index];
    if (!ctx->thread_created) return 0;

    ctx->running = 0; 
    packet_queue_abort(&ctx->RecordQueue);
    packet_queue_abort(&ctx->P2pQueue);

    #ifdef ENABLE_MP4_RECORD
    Record_Stop(Station, Index);
    #endif
    P2P_Stop(Station, Index);

    pthread_join(ctx->Thread, NULL);
    ctx->thread_created = 0;

    packet_queue_destroy(&ctx->RecordQueue);
    packet_queue_destroy(&ctx->P2pQueue);

    return 0;
}

int32_t Stream_SetPause(StationHandle *Station, int32_t Index, int32_t Pause) {
    StreamHandle *Stream = Station->Stream;
    if (!Stream || Index < 0 || Index >= CAM_MAX_CNT) return -1;

    RtspCtx *ctx = &Stream->Rtsp[Index];
    
    if (ctx->running && ctx->thread_created) {
        if (Pause) {
            if (!ctx->paused) {
                ctx->paused = 1;
                packet_queue_flush(&ctx->P2pQueue);
                LOG_INFO(TAG, "[Ch%d] Paused & Flushed\n", Index);
            }
        } else {
            packet_queue_flush(&ctx->P2pQueue);
            if (ctx->paused) {
                ctx->paused = 0;
                LOG_INFO(TAG, "[Ch%d] Resumed\n", Index);
            }
            Stream_RequestIFrame(Station, Index);
        }
        return 0;
    }
    return -1;
}

int32_t Stream_RequestIFrame(StationHandle *Station, int32_t Index) {
    return CamManage_Send(Station, Index, MSG_REQ_IFRAME, NULL, 0);
}

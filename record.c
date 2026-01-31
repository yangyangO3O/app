#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>

#include "camera_manage.h"
#include "system.h"
#include "storage.h"
#include "record.h"
#include "packet_queue.h"

#define TAG "RECORD"

#define RECORD_BASE_DIR  "/tmp/mnt/sdcard/"
#ifndef RECORD_SLICE_MIN
#define RECORD_SLICE_MIN 1
#endif
#define SLICE_SECONDS    (RECORD_SLICE_MIN * 60)
#define SLICE_MS         ((int64_t)SLICE_SECONDS * 1000)
#define PREBUF_MAX_PKTS   1200
#define PREBUF_MAX_BYTES  (12 * 1024 * 1024)   // 12MB

// 定义 I/O 错误后的冷却时间 (ms)
#define IO_ERROR_COOLDOWN_MS 3000

// AAC 标准帧大小
#define AAC_FRAME_SIZE_DEFAULT 1024

// H.264 NAL Unit Types
#define NAL_TYPE_SLICE_IDR 5
#define NAL_TYPE_SPS       7
#define NAL_TYPE_PPS       8

/* ========================================================================== */
/* 调试辅助函数                                                               */
/* ========================================================================== */

static void LogHex(const char *func, const char *desc, const uint8_t *data, int size)
{
    int print_len;
    char buf[64] = {0};
    int i;
    
    if (!data || size <= 0) return;
    print_len = (size > 16) ? 16 : size;
    
    for (i = 0; i < print_len; i++) {
        snprintf(buf + i * 3, 4, "%02X ", data[i]);
    }
    LOG_INFO(TAG, "[%s] %s (Sz:%d): %s%s\n", func, desc, size, buf, (size > 16) ? "..." : "");
}


/* ========================================================================== */
/* 结构体定义                                                                 */
/* ========================================================================== */

// 简单的位读取辅助结构
typedef struct {
    const uint8_t *p;
    int size;   // bytes
    int bit;    // bit position
} BitReader;

static int64_t NowMsMonotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static inline unsigned BrReadBit(BitReader *b)
{
    unsigned v;
    if (!b || b->bit >= (b->size * 8)) {
        return 0;
    }
    v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return v;
}

static inline unsigned BrReadBits(BitReader *b, int n)
{
    unsigned v = 0;
    int i;
    for (i = 0; i < n; i++) {
        v = (v << 1) | BrReadBit(b);
    }
    return v;
}

static unsigned BrReadUe(BitReader *b)
{
    unsigned zeros = 0;
    unsigned info = 0;
    unsigned i;

    while (BrReadBit(b) == 0 && zeros < 32) {
        zeros++;
    }

    for (i = 0; i < zeros; i++) {
        info = (info << 1) | BrReadBit(b);
    }

    return ((1U << zeros) - 1U) + info;
}

static inline int BrReadSe(BitReader *b)
{
    unsigned ue = BrReadUe(b);
    return (ue & 1) ? (int)((ue + 1) / 2) : -(int)(ue / 2);
}

static int EbspToRbsp(uint8_t *dst, const uint8_t *src, int n)
{
    int di = 0;
    int zeros = 0;
    int i;

    for (i = 0; i < n; i++) {
        uint8_t v = src[i];
        if (zeros >= 2 && v == 0x03) {
            zeros = 0;
            continue; // Skip Emulation Prevention Byte
        }
        dst[di++] = v;
        if (v == 0x00) {
            zeros++;
        } else {
            zeros = 0;
        }
    }
    return di;
}

/* ========================================================================== */
/* 时间戳处理逻辑                                                             */
/* ========================================================================== */

static void NormalizeAndRescaleTs(RecordCtx *ctx, AVPacket *pkt, int is_video)
{
    AVStream    *st;
    AVRational  src_tb;
    int64_t     *p_start_dts;
    int         *p_start_inited;
    int64_t     *p_last_dts;
    int64_t     *p_last_pts;

    if (is_video) {
        st = ctx->v_st;
        src_tb = ctx->v_src_tb;
        p_start_dts = &ctx->v_start_dts;
        p_start_inited = &ctx->v_start_dts_inited;
        p_last_dts  = &ctx->v_last_dts;
        p_last_pts  = &ctx->v_last_pts;
    } else {
        st = ctx->a_st;
        src_tb = ctx->a_src_tb;
        p_start_dts = &ctx->a_start_dts;
        p_start_inited = &ctx->a_start_dts_inited;
        p_last_dts  = &ctx->a_last_dts;
        p_last_pts  = &ctx->a_last_pts;
    }

    if (!st) {
        return;
    }

    // 补全缺失的时间戳
    if (pkt->pts == AV_NOPTS_VALUE) pkt->pts = pkt->dts;
    if (pkt->dts == AV_NOPTS_VALUE) pkt->dts = pkt->pts;

    if (pkt->duration == 0) {
        if (is_video && st->avg_frame_rate.num > 0) {
             pkt->duration = av_rescale_q(1, (AVRational){st->avg_frame_rate.den, st->avg_frame_rate.num}, src_tb);
        } else {
             pkt->duration = av_rescale_q(40, (AVRational){1, 1000}, src_tb);
        }
    }

    if (pkt->dts == AV_NOPTS_VALUE) {
        if (*p_last_dts != AV_NOPTS_VALUE) {
            int64_t inc = pkt->duration > 0 ? pkt->duration : av_rescale_q(33, (AVRational){1, 1000}, src_tb);
            pkt->dts = *p_last_dts + inc;
            pkt->pts = pkt->dts;
        } else {
            pkt->dts = 0;
            pkt->pts = 0;
        }
    }

    int64_t in_dts = pkt->dts;
    int64_t in_pts = pkt->pts;

    /* 检测时间戳跳变 */
    if (*p_start_inited && *p_last_dts != AV_NOPTS_VALUE) {
        int64_t last_dts_src = av_rescale_q(*p_last_dts, st->time_base, src_tb);
        int64_t prev_raw_dts = *p_start_dts + last_dts_src;
        int64_t diff = in_dts - prev_raw_dts;
        if (diff < 0) diff = -diff;

        if (diff > 900000) { // 约10秒
            LOG_WARN(TAG, "Timestamp jump detected! PrevRaw: %lld, CurrRaw: %lld. Resetting base.\n", prev_raw_dts, in_dts);
            *p_start_dts = in_dts;
        }
    }

    /* 初始化基准时间 */
    if (!*p_start_inited) {
        int64_t base = (in_dts != AV_NOPTS_VALUE) ? in_dts : (in_pts != AV_NOPTS_VALUE) ? in_pts : 0;
        *p_start_dts = base;
        *p_start_inited = 1;
    }
    else if (*p_start_dts == 0 && in_dts != AV_NOPTS_VALUE && in_dts > 1000000) {
        *p_start_dts = in_dts;
    }

    /* 归一化 (相对时间) */
    if (in_dts != AV_NOPTS_VALUE) {
        int64_t v = in_dts - *p_start_dts;
        pkt->dts = (v < 0) ? 0 : v;
    }
    if (in_pts != AV_NOPTS_VALUE) {
        int64_t v = in_pts - *p_start_dts;
        pkt->pts = (v < 0) ? 0 : v;
    }

    // 转换到 MP4 Timebase
    if (pkt->dts != AV_NOPTS_VALUE) pkt->dts = av_rescale_q(pkt->dts, src_tb, st->time_base);
    if (pkt->pts != AV_NOPTS_VALUE) pkt->pts = av_rescale_q(pkt->pts, src_tb, st->time_base);
    if (pkt->duration > 0)          pkt->duration = av_rescale_q(pkt->duration, src_tb, st->time_base);

    // 保证单调递增
    if (*p_last_dts != AV_NOPTS_VALUE && pkt->dts <= *p_last_dts) {
        pkt->dts = *p_last_dts + 1;
    }
    if (pkt->pts < pkt->dts) {
        pkt->pts = pkt->dts;
    }

    *p_last_dts = pkt->dts;
    *p_last_pts = pkt->pts;
}

static void ResetTsState(RecordCtx *ctx)
{
    ctx->v_start_dts = ctx->a_start_dts = AV_NOPTS_VALUE;
    ctx->v_last_dts  = ctx->a_last_dts  = AV_NOPTS_VALUE;
    ctx->v_last_pts  = ctx->a_last_pts  = AV_NOPTS_VALUE;
    ctx->v_start_dts_inited = 0;
    ctx->a_start_dts_inited = 0;
}

/* ========================================================================== */
/* H.264 / AAC 解析逻辑                                                       */
/* ========================================================================== */

static int FindStartCode(const uint8_t *p, int from, int size, int *sc_size)
{
    int i;
    for (i = from; i + 3 < size; i++) {
        if (p[i] == 0x00 && p[i+1] == 0x00) {
            if (p[i+2] == 0x01) {
                *sc_size = 3;
                return i;
            }
            if (i + 4 < size && p[i+2] == 0x00 && p[i+3] == 0x01) {
                *sc_size = 4;
                return i;
            }
        }
    }
    return -1;
}

static int H264IsIframeNal(const uint8_t *nal, int nal_len)
{
    int nal_type;
    uint8_t rbsp[128];
    int pay;
    int rbsp_len;
    BitReader br;
    unsigned slice_type;

    if (!nal || nal_len < 2) return 0;
    nal_type = nal[0] & 0x1F;
    if (nal_type == NAL_TYPE_SLICE_IDR) return 1;
    if (nal_type != 1) return 0;

    pay = nal_len - 1;
    if (pay > sizeof(rbsp)) pay = sizeof(rbsp);

    rbsp_len = EbspToRbsp(rbsp, nal + 1, pay);
    br.p = rbsp; br.size = rbsp_len; br.bit = 0;
    
    BrReadUe(&br); 
    slice_type = BrReadUe(&br);
    slice_type %= 5;
    return (slice_type == 2 || slice_type == 4);
}

static int H264IsIframeAnnexb(const uint8_t *p, int n)
{
    int scsz = 0, pos = 0;
    if (!p || n < 5) return 0;
    
    while (1) {
        int sc = FindStartCode(p, pos, n, &scsz);
        int nal_start;
        int scsz2 = 0;
        int sc2;
        int nal_end;
        
        if (sc < 0) break;
        nal_start = sc + scsz;
        if (nal_start >= n) break;
        
        sc2 = FindStartCode(p, nal_start, n, &scsz2);
        nal_end = (sc2 >= 0) ? sc2 : n;
        
        if (H264IsIframeNal(&p[nal_start], nal_end - nal_start)) return 1;
        pos = nal_end;
        if (pos >= n) break;
    }
    return 0;
}

static void H264CollectSpsPps(const uint8_t *p, int n, uint8_t *sps, int *sps_sz, int sps_cap, uint8_t *pps, int *pps_sz, int pps_cap)
{
    int scsz = 0, pos = 0;
    while (1) {
        int sc = FindStartCode(p, pos, n, &scsz);
        int nal_start;
        int scsz2 = 0;
        int sc2;
        int nal_end;
        int nal_size;

        if (sc < 0) break;
        nal_start = sc + scsz;
        sc2 = FindStartCode(p, nal_start, n, &scsz2);
        nal_end = (sc2 >= 0) ? sc2 : n;
        nal_size = nal_end - sc;

        if (nal_start < n && nal_size > 0) {
            int nal_type = p[nal_start] & 0x1F;
            if (nal_type == NAL_TYPE_SPS) {
                int copy_len = (nal_size > sps_cap) ? sps_cap : nal_size;
                memcpy(sps, &p[sc], copy_len);
                *sps_sz = copy_len;
            } else if (nal_type == NAL_TYPE_PPS) {
                int copy_len = (nal_size > pps_cap) ? pps_cap : nal_size;
                memcpy(pps, &p[sc], copy_len);
                *pps_sz = copy_len;
            }
        }
        pos = nal_end;
        if (pos >= n) break;
    }
}

static int H264ParseSpsWh(const uint8_t *sps_payload, int sps_len, int *w, int *h)
{
    uint8_t rbsp[512];
    int rbsp_len;
    BitReader br;
    unsigned profile_idc;
    unsigned chroma_format_idc = 1;
    unsigned separate_colour_plane_flag = 0;
    unsigned pic_width_in_mbs_minus1;
    unsigned pic_height_in_map_units_minus1;
    unsigned frame_mbs_only_flag;
    unsigned frame_cropping_flag;
    unsigned crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    int width, height;
    int sub_width_c = 1, sub_height_c = 1;
    int crop_unit_x, crop_unit_y;
    int i;

    if (!sps_payload || sps_len <= 0) return -1;
    if (sps_len > sizeof(rbsp)) sps_len = sizeof(rbsp);

    rbsp_len = EbspToRbsp(rbsp, sps_payload, sps_len);
    br.p = rbsp; br.size = rbsp_len; br.bit = 0;

    profile_idc = BrReadBits(&br, 8);
    BrReadBits(&br, 8); // constraint_set_flags
    BrReadBits(&br, 8); // level_idc
    BrReadUe(&br);      // seq_parameter_set_id

    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 ||
        profile_idc == 244 || profile_idc == 44 || profile_idc == 83 ||
        profile_idc == 86 || profile_idc == 118 || profile_idc == 128 ||
        profile_idc == 138 || profile_idc == 139 || profile_idc == 134) {

        chroma_format_idc = BrReadUe(&br);
        if (chroma_format_idc == 3) {
            separate_colour_plane_flag = BrReadBit(&br);
        }
        BrReadUe(&br); // bit_depth_luma_minus8
        BrReadUe(&br); // bit_depth_chroma_minus8
        BrReadBit(&br); // qpprime_y_zero_transform_bypass_flag

        if (BrReadBit(&br)) { // seq_scaling_matrix_present_flag
            int scaling_list_count = (chroma_format_idc != 3) ? 8 : 12;
            for (i = 0; i < scaling_list_count; i++) {
                if (BrReadBit(&br)) {
                    int size = (i < 6) ? 16 : 64;
                    int last = 8, next = 8, j;
                    for (j = 0; j < size; j++) {
                        if (next != 0) {
                            int delta = BrReadSe(&br);
                            next = (last + delta + 256) % 256;
                        }
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    }

    BrReadUe(&br); // log2_max_frame_num_minus4
    unsigned pic_order_cnt_type = BrReadUe(&br);

    if (pic_order_cnt_type == 0) {
        BrReadUe(&br); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        BrReadBit(&br); 
        BrReadSe(&br);  
        BrReadSe(&br);  
        unsigned num = BrReadUe(&br);
        for (unsigned i = 0; i < num; i++) BrReadSe(&br);
    }

    BrReadUe(&br); // max_num_ref_frames
    BrReadBit(&br); // gaps_in_frame_num_value_allowed_flag

    pic_width_in_mbs_minus1 = BrReadUe(&br);
    pic_height_in_map_units_minus1 = BrReadUe(&br);
    frame_mbs_only_flag = BrReadBit(&br);

    if (!frame_mbs_only_flag) BrReadBit(&br); // mb_adaptive_frame_field_flag
    BrReadBit(&br); // direct_8x8_inference_flag

    frame_cropping_flag = BrReadBit(&br);
    if (frame_cropping_flag) {
        crop_left = BrReadUe(&br);
        crop_right = BrReadUe(&br);
        crop_top = BrReadUe(&br);
        crop_bottom = BrReadUe(&br);
    }

    width = (pic_width_in_mbs_minus1 + 1) * 16;
    height = (pic_height_in_map_units_minus1 + 1) * 16 * (2 - frame_mbs_only_flag);

    if (separate_colour_plane_flag) chroma_format_idc = 0;

    if (chroma_format_idc == 1) { sub_width_c = 2; sub_height_c = 2; }
    else if (chroma_format_idc == 2) { sub_width_c = 2; sub_height_c = 1; }

    crop_unit_x = sub_width_c;
    crop_unit_y = sub_height_c * (2 - frame_mbs_only_flag);

    width -= (crop_left + crop_right) * crop_unit_x;
    height -= (crop_top + crop_bottom) * crop_unit_y;

    *w = width;
    *h = height;
    return 0;
}

// AAC 辅助函数
static int AacCollectAsc(const uint8_t *p, int size, uint8_t *asc, int *asc_size)
{
    int i;
    if (!p || size < 7 || !asc || !asc_size) return -1;
    for (i = 0; i < size - 7; i++) {
        if (p[i] == 0xFF && (p[i+1] & 0xF0) == 0xF0) {
            uint8_t adts_profile = (p[i+2] & 0xC0) >> 6;
            uint8_t freq_idx     = (p[i+2] & 0x3C) >> 2;
            uint8_t chan_cfg     = ((p[i+2] & 0x01) << 2) | ((p[i+3] & 0xC0) >> 6);
            uint8_t object_type = adts_profile + 1;

            asc[0] = (object_type << 3) | (freq_idx >> 1);
            asc[1] = ((freq_idx & 1) << 7) | (chan_cfg << 3);
            *asc_size = 2;
            return 0;
        }
    }
    return -1;
}

/* ========================================================================== */
/* PreBuffer                                                                  */
/* ========================================================================== */

static void PrebufInit(PreBuf *b)
{
    int i;
    memset(b, 0, sizeof(*b));
    for (i = 0; i < PREBUF_MAX_PKTS; i++) {
        av_init_packet(&b->pkts[i]);
        b->pkts[i].data = NULL;
    }
}

static void PrebufDropOldest(PreBuf *b)
{
    AVPacket *p;
    if (b->count <= 0) return;
    p = &b->pkts[b->head];
    b->bytes -= p->size;
    av_packet_unref(p);
    b->head = (b->head + 1) % PREBUF_MAX_PKTS;
    b->count--;
}

static void PrebufPush(PreBuf *b, const AVPacket *src)
{
    int idx;
    if (!b || !src) return;
    while (b->count >= PREBUF_MAX_PKTS) PrebufDropOldest(b);
    while (b->count > 0 && (b->bytes + src->size) > PREBUF_MAX_BYTES) PrebufDropOldest(b);

    idx = (b->head + b->count) % PREBUF_MAX_PKTS;
    av_packet_unref(&b->pkts[idx]);
    if (av_packet_ref(&b->pkts[idx], (AVPacket*)src) == 0) {
        b->bytes += b->pkts[idx].size;
        b->count++;
    }
}

static void PrebufClear(PreBuf *b)
{
    if (!b) return;
    while (b->count > 0) PrebufDropOldest(b);
}

static int PrebufFindFirstIframe(const PreBuf *b, int video_index)
{
    int k;
    if (!b) return -1;
    for (k = 0; k < b->count; k++) {
        int idx = (b->head + k) % PREBUF_MAX_PKTS;
        const AVPacket *p = &b->pkts[idx];
        if (p->stream_index == video_index) {
            if ((p->flags & AV_PKT_FLAG_KEY) || H264IsIframeAnnexb(p->data, p->size)) {
                return k;
            }
        }
    }
    return -1;
}

/* ========================================================================== */
/* 文件操作                                                                   */
/* ========================================================================== */

static int EnsureDir(const char *dir)
{
    if (!dir || !dir[0]) return -1;
    if (access(dir, F_OK) == 0) return 0;
    if (mkdir(dir, 0755) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

static void MakeSegmentFilename(RecordCtx *ctx, int cam_index, int seg_no)
{
    char dt[32] = {0};
    char dir[64] = {0};
    System_GetTimeStamp(ctx->Record->Station, dt, 0);
    snprintf(dir, sizeof(dir), "%s/CAM%d", RECORD_BASE_DIR, cam_index);
    EnsureDir(RECORD_BASE_DIR);
    EnsureDir(dir);
    snprintf(ctx->FileName, sizeof(ctx->FileName), "%s/%s_seg%04d.MP4", dir, dt, seg_no);
}

static int OpenMp4Segment(RecordCtx *ctx, AVFormatContext **poc, AVStream *stream_in_audio,
                          const uint8_t *sps, int sps_sz, const uint8_t *pps, int pps_sz,
                          const uint8_t *asc, int asc_sz)
{
    int ret;
    AVFormatContext *oc = NULL;
    AVStream *vst = NULL;
    AVStream *ast = NULL;
    AVDictionary *wopt = NULL;
    time_t now;
    struct tm t;
    char buf[64];
    int exsz;
    uint8_t *ex = NULL;

    ret = avformat_alloc_output_context2(&oc, NULL, NULL, ctx->FileName);
    if (ret < 0 || !oc) {
        LOG_ERROR(TAG, "alloc_output_context2(%s) failed\n", ctx->FileName);
        return -1;
    }

    // --- Video Stream ---
    vst = avformat_new_stream(oc, NULL);
    if (!vst || !ctx->v_codecpar_cache) {
        avformat_free_context(oc);
        return -1;
    }
    avcodec_parameters_copy(vst->codecpar, ctx->v_codecpar_cache);

    // 填充 Video Extradata
    if ((vst->codecpar->extradata_size <= 0) && sps_sz > 0 && pps_sz > 0) {
        exsz = sps_sz + pps_sz;
        ex = av_malloc(exsz + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ex) {
            memcpy(ex, sps, sps_sz);
            memcpy(ex + sps_sz, pps, pps_sz);
            memset(ex + exsz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
            if (vst->codecpar->extradata) av_free(vst->codecpar->extradata);
            vst->codecpar->extradata = ex;
            vst->codecpar->extradata_size = exsz;
            LOG_INFO(TAG, "Filled Video Extradata: %d bytes\n", exsz);
        }
    }
    vst->codecpar->codec_tag = 0;
    vst->time_base = ctx->v_src_tb;
    ctx->v_st = vst;

    // --- Audio Stream ---
    ctx->a_st = NULL;
    if (stream_in_audio && stream_in_audio->codecpar->codec_id != AV_CODEC_ID_NONE) {
        ast = avformat_new_stream(oc, NULL);
        if (ast) {
            avcodec_parameters_copy(ast->codecpar, stream_in_audio->codecpar);
            ast->codecpar->codec_tag = 0;
            ast->time_base = stream_in_audio->time_base;

            // [重要] 填充 Audio Extradata (ASC)
            if (ast->codecpar->extradata_size <= 0 && asc && asc_sz > 0) {
                if (ast->codecpar->extradata) av_free(ast->codecpar->extradata);
                ast->codecpar->extradata = av_malloc(asc_sz + AV_INPUT_BUFFER_PADDING_SIZE);
                if (ast->codecpar->extradata) {
                    memcpy(ast->codecpar->extradata, asc, asc_sz);
                    memset(ast->codecpar->extradata + asc_sz, 0, AV_INPUT_BUFFER_PADDING_SIZE);
                    ast->codecpar->extradata_size = asc_sz;
                    LOG_INFO(TAG, "Filled AAC Extradata (ASC): %d bytes\n", asc_sz);
                    LogHex("OpenMp4", "AAC Extra", asc, asc_sz);
                }
            }
            
            // [调试]
            if (ast->codecpar->extradata_size > 0) {
                LOG_INFO(TAG, "Audio Stream Ready. ExtradataSize=%d\n", ast->codecpar->extradata_size);
            } else {
                LOG_ERROR(TAG, "CRITICAL: Audio Extradata is EMPTY! VLC will have no sound.\n");
            }

            if (ast->codecpar->frame_size == 0 && ast->codecpar->codec_id == AV_CODEC_ID_AAC) {
                ast->codecpar->frame_size = AAC_FRAME_SIZE_DEFAULT;
            }
            ctx->a_st = ast;
            ctx->a_src_tb = stream_in_audio->time_base;
        }
    }

    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&oc->pb, ctx->FileName, AVIO_FLAG_WRITE) < 0) {
            avformat_free_context(oc);
            return -1;
        }
    }

    av_dict_set(&wopt, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
    
    // 写入 Creation Time
    now = time(NULL);
    localtime_r(&now, &t);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
    av_dict_set(&oc->metadata, "creation_time", buf, 0);

    ret = avformat_write_header(oc, &wopt);
    av_dict_free(&wopt);

    if (ret < 0) {
        if (oc->pb) avio_closep(&oc->pb);
        avformat_free_context(oc);
        return -1;
    }

    *poc = oc;
    return 0;
}

static void CloseMp4Segment(AVFormatContext **poc, int file_opened)
{
    AVFormatContext *oc;
    if (!poc || !*poc) return;
    oc = *poc;
    if (file_opened) {
        av_write_trailer(oc);
        LOG_INFO(TAG, "Closed segment\n");
    }
    if (oc->pb) avio_closep(&oc->pb);
    avformat_free_context(oc);
    *poc = NULL;
}

static void* Record_Thread(void *Arg)
{
    int32_t ret;
    RecordCtx *ctx = (RecordCtx *)Arg;
    AVFormatContext *oc = NULL;
    AVStream *stream_in_audio = NULL;
    AVPacket pkt;
    int32_t video_idx, audio_idx;
    int file_opened = 0;
    int wait_audio_cnt = 0; 
    uint8_t sps_cache[512]; int sps_sz = 0;
    uint8_t pps_cache[256]; int pps_sz = 0;
    uint8_t asc_cache[2];   int asc_sz = 0;
    PreBuf pb;
    int64_t seg_start_ms = 0;
    int seg_no = 1;
    int cam_index;
    int k;

    prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
    PrebufInit(&pb);

    ctx->v_codecpar_cache = avcodec_parameters_alloc();
    ctx->v_extradata_cached = 0;
    ctx->last_io_error_ms = 0;

    cam_index = (int)(ctx - ctx->Record->Ctx);
    
    // 等待 RTSP 准备就绪
    while (1) {
        if (ctx->Rtsp->running == 0) goto Record_Thread_Exit;
        if (ctx->Rtsp->running == 2 && Storage_IsReady(ctx->Record->Station)) break;
        usleep(20 * 1000);
    }

    video_idx = ctx->Rtsp->VdIndex;
    audio_idx = ctx->Rtsp->AdIndex;

    // 复制参数
    {
        AVStream *src_v = ctx->Rtsp->AvFmtCtx->streams[video_idx];
        avcodec_parameters_copy(ctx->v_codecpar_cache, src_v->codecpar);
        ctx->v_src_tb = src_v->time_base;
        ctx->v_width_cache = src_v->codecpar->width;
        ctx->v_height_cache = src_v->codecpar->height;
        LOG_INFO(TAG, "Cached Video: %dx%d TB=%d/%d\n", ctx->v_width_cache, ctx->v_height_cache, ctx->v_src_tb.num, ctx->v_src_tb.den);
    }
    if (audio_idx >= 0) {
        stream_in_audio = ctx->Rtsp->AvFmtCtx->streams[audio_idx];
        if (stream_in_audio->codecpar->extradata_size > 0) {
            if (stream_in_audio->codecpar->extradata_size <= 2) {
                memcpy(asc_cache, stream_in_audio->codecpar->extradata, stream_in_audio->codecpar->extradata_size);
                asc_sz = stream_in_audio->codecpar->extradata_size;
                LOG_INFO(TAG, "Audio config loaded from AVStream.\n");
            }
        }
        
        // 【关键修复】如果流中没有 Extradata (Raw AAC)，强制注入配置
        // 0x14 0x08 对应 AAC LC, 16000Hz, Mono
        if (asc_sz == 0) {
            asc_cache[0] = 0x14; 
            asc_cache[1] = 0x08;
            asc_sz = 2;
            LOG_INFO(TAG, "FIX: Hardcoded AAC ASC to 16k Mono (14 08) for Raw AAC Stream\n");
        }
    }

    ResetTsState(ctx);
    av_init_packet(&pkt);

    while (1) {
        if (ctx->Rtsp->running == 0) {
            if (packet_queue_get(&ctx->Rtsp->RecordQueue, &pkt, 1) <= 0) break;
        } else {
            ret = packet_queue_get(&ctx->Rtsp->RecordQueue, &pkt, 1);
            if (ret < 0) break;
            if (ret == 0) continue;
        }

        int is_video = (pkt.stream_index == video_idx);
        int is_audio = (audio_idx >= 0 && pkt.stream_index == audio_idx);

        if (!is_video && !is_audio) {
            av_packet_unref(&pkt);
            continue;
        }

        // --- 关键信息采集 ---
        // (虽然我们已经硬编码了 ASC，但保留此逻辑以兼容其他标准流，只要不覆盖非零值即可)
        if (is_audio && pkt.data && pkt.size > 7 && asc_sz == 0) {
            if (AacCollectAsc(pkt.data, pkt.size, asc_cache, &asc_sz) == 0) {
                LOG_INFO(TAG, "Captured AAC ASC: %02X %02X\n", asc_cache[0], asc_cache[1]);
            }
        }
        if (is_video && pkt.data && pkt.size > 0) {
            H264CollectSpsPps(pkt.data, pkt.size, sps_cache, &sps_sz, 512, pps_cache, &pps_sz, 256);
            if (H264IsIframeAnnexb(pkt.data, pkt.size)) pkt.flags |= AV_PKT_FLAG_KEY;
        }

        // --- 切片逻辑 ---
        if (file_opened && is_video) {
            int64_t now = NowMsMonotonic();
            int slice_due = (seg_start_ms != 0 && (now - seg_start_ms) >= SLICE_MS);
            int is_i = ((pkt.flags & AV_PKT_FLAG_KEY) != 0);

            if (slice_due && is_i) {
                CloseMp4Segment(&oc, 1);
                oc = NULL;
                file_opened = 0;
                ResetTsState(ctx);
                PrebufClear(&pb);
                PrebufPush(&pb, &pkt);
                av_packet_unref(&pkt);
                wait_audio_cnt = 0;
                continue;
            }
        }

        // --- 打开文件逻辑 ---
        if (!file_opened) {
            PrebufPush(&pb, &pkt);

            if (NowMsMonotonic() - ctx->last_io_error_ms < IO_ERROR_COOLDOWN_MS) {
                usleep(20 * 1000);
                av_packet_unref(&pkt);
                continue;
            }

            int w = ctx->v_width_cache;
            int h = ctx->v_height_cache;

            // [FIX] 如果缓存的宽高是 0 (因为RTSP握手时没拿到)，尝试从捕获到的 SPS 中解析
            if (w <= 0 || h <= 0) {
                if (sps_sz > 0) {
                     int tw=0, th=0;
                     if (H264ParseSpsWh(sps_cache, sps_sz, &tw, &th) == 0) {
                         w = tw; h = th;
                         ctx->v_width_cache = w;
                         ctx->v_height_cache = h;
                         ctx->v_codecpar_cache->width = w;
                         ctx->v_codecpar_cache->height = h;
                         LOG_INFO(TAG, "Updated Video Params from collected SPS: %dx%d\n", w, h);
                     }
                }
            }

            int have_spspps = (sps_sz > 0 && pps_sz > 0) || (ctx->v_codecpar_cache->extradata_size > 0);
            int iframe_off = PrebufFindFirstIframe(&pb, video_idx);

            // [修复] 增加对音频配置的等待 (定义已移至顶部)
            int audio_ready = (audio_idx < 0) || (asc_sz > 0);
            
            if (!audio_ready && wait_audio_cnt < 100) { 
                wait_audio_cnt++;
                if (pb.count < PREBUF_MAX_PKTS - 20) {
                    av_packet_unref(&pkt);
                    continue; 
                }
            }

            if (w > 0 && h > 0 && have_spspps && iframe_off >= 0) {
                MakeSegmentFilename(ctx, cam_index, seg_no);
                if (OpenMp4Segment(ctx, &oc, stream_in_audio, sps_cache, sps_sz, pps_cache, pps_sz, asc_cache, asc_sz) == 0) {
                    file_opened = 1;
                    seg_start_ms = NowMsMonotonic();
                    seg_no++;

                    // 写入 Prebuf
                    for (k = iframe_off; k < pb.count; k++) {
                        int idx = (pb.head + k) % PREBUF_MAX_PKTS;
                        AVPacket *bp = &pb.pkts[idx];
                        int bv = (bp->stream_index == video_idx);
                        int ba = (audio_idx >= 0 && bp->stream_index == audio_idx);

                        if (bv) {
                            if (H264IsIframeAnnexb(bp->data, bp->size)) bp->flags |= AV_PKT_FLAG_KEY;
                            NormalizeAndRescaleTs(ctx, bp, 1);
                            bp->stream_index = ctx->v_st->index;
                        } else if (ba && ctx->a_st) {
                            if (ctx->a_st->codecpar->codec_id == AV_CODEC_ID_AAC) {
                                if (bp->size > 7 && bp->data[0] == 0xFF && (bp->data[1] & 0xF0) == 0xF0) {
                                    int hlen = (bp->data[1] & 0x01) ? 7 : 9;
                                    bp->data += hlen;
                                    bp->size -= hlen;
                                }
                            }
                            NormalizeAndRescaleTs(ctx, bp, 0);
                            bp->stream_index = ctx->a_st->index;
                        } else {
                            continue;
                        }
                        bp->pos = -1;
                        av_interleaved_write_frame(oc, bp);
                    }
                    PrebufClear(&pb);
                } else {
                    ctx->last_io_error_ms = NowMsMonotonic();
                }
            }
            av_packet_unref(&pkt);
            continue;
        }

        // --- 正常写入逻辑 ---
        if (is_video) {
            if (H264IsIframeAnnexb(pkt.data, pkt.size)) pkt.flags |= AV_PKT_FLAG_KEY;
            NormalizeAndRescaleTs(ctx, &pkt, 1);
            pkt.stream_index = ctx->v_st->index;
        } else {
            if (!ctx->a_st) {
                av_packet_unref(&pkt);
                continue;
            }
            if (ctx->a_st->codecpar->codec_id == AV_CODEC_ID_AAC) {
                if (pkt.size > 7 && pkt.data[0] == 0xFF && (pkt.data[1] & 0xF0) == 0xF0) {
                    int header_len = (pkt.data[1] & 0x01) ? 7 : 9;
                    pkt.data += header_len;
                    pkt.size -= header_len;
                }
            }
            NormalizeAndRescaleTs(ctx, &pkt, 0);
            pkt.stream_index = ctx->a_st->index;
        }
        pkt.pos = -1;
        av_interleaved_write_frame(oc, &pkt);
        av_packet_unref(&pkt);
    }

Record_Thread_Exit:
    PrebufClear(&pb);
    if (oc) CloseMp4Segment(&oc, file_opened);
    if (ctx->v_codecpar_cache) avcodec_parameters_free(&ctx->v_codecpar_cache);
    LOG_DEBUG(TAG, "Thread exit\n");
    pthread_exit(NULL);
}

int32_t Record_Init(StationHandle *Station) {
    RecordHandle *Record = calloc(1, sizeof(RecordHandle));
    if (!Record) return -1;
    Station->Record = Record;
    Record->Station = Station;
    pthread_mutex_init(&Record->Mutex, NULL);
    return 0;
}
void Record_Deinit(StationHandle *Station) {
    if (Station->Record) {
        pthread_mutex_destroy(&Station->Record->Mutex);
        free(Station->Record);
        Station->Record = NULL;
    }
}
int32_t Record_Start(StationHandle *Station, int32_t Index) {
    RecordHandle *Record = Station->Record;
    if (!Record) return -1;
    Record_Stop(Station, Index);
    memset(&Record->Ctx[Index], 0, sizeof(RecordCtx));
    Record->Ctx[Index].Record = Record;
    Record->Ctx[Index].Rtsp   = &Station->Stream->Rtsp[Index];
    if (pthread_create(&Record->Ctx[Index].Thread, NULL, Record_Thread, &Record->Ctx[Index]) != 0) return -1;
    Record->Ctx[Index].thread_created = 1;
    return 0;
}
int32_t Record_Stop(StationHandle *Station, int32_t Index) {
    RecordCtx *rc;
    RtspCtx *rt;
    if (!Station || !Station->Record || !Station->Stream) return 0;
    rc = &Station->Record->Ctx[Index];
    rt = &Station->Stream->Rtsp[Index];
    if (!rc->thread_created) return 0;
    packet_queue_abort(&rt->RecordQueue);
    pthread_join(rc->Thread, NULL);
    rc->thread_created = 0;
    return 0;
}

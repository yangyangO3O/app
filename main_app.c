/* main_app.c */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <stdarg.h>

// FFmpeg
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/log.h>
#include <libavutil/error.h>

#include "packet_queue.h"
#include "rtsp_puller.h"
#include "rtsp_forwarder.h"
#include "ffmpeg_muxer.h"

#define MAX_CHANNELS    4
#define MAX_URL_LEN     256

#define MAIN_LOG_PREFIX "[MAIN] "

// Custom log callback to ensure we see FFmpeg output in terminal
static void ffmpeg_log_callback(void *ptr, int level, const char *fmt, va_list vl)
{
    // You can adjust this cutoff. AV_LOG_ERROR or AV_LOG_WARNING is usually enough.
    // Use AV_LOG_INFO to see "non-existing PPS" errors clearly.
    if (level > AV_LOG_INFO) return; 

    fprintf(stderr, "[FFMPEG] ");
    vfprintf(stderr, fmt, vl);
    // fprintf(stderr, "\n"); // FFmpeg usually includes newline
}

typedef struct {
    int   id;

    char  in_rtsp_url[MAX_URL_LEN];
    char  out_rtsp_url[MAX_URL_LEN];
    char  record_file[MAX_URL_LEN];

    int   enable_record;
    int   enable_forward;
	// 0 = 正常转发, 1 = 暂停转发 (录像不受影响)
	volatile int p2p_paused;

    RtspPuller    *puller;
    RtspForwarder *forwarder;
    FFmpegMuxer   *rec_muxer;

    int           video_index;
    int           audio_index;
    AVRational    v_tb;
    AVRational    a_tb;

    // Pointer to the codec parameters owned by the Puller/Demuxer
    AVCodecParameters *v_par_ref;
    AVCodecParameters *a_par_ref;

    // ✅ SAFE: deep-copied codecpar (owned by ChannelCtx)
    AVCodecParameters *v_par_copy;
    AVCodecParameters *a_par_copy;

    int muxer_pending;
    int muxer_retry_cnt;
    int64_t muxer_last_try_ms;

    PacketQueue queue_disk;
    PacketQueue queue_net;

    pthread_t   tid_disk;
    pthread_t   tid_net;

    volatile int writer_running;
    
    // NEW: Wait for keyframe flag
    int has_seen_keyframe;
} ChannelCtx;

static ChannelCtx   g_ch[MAX_CHANNELS];
static int          g_channel_count = 0;
static volatile int g_running       = 1;

// “first frame + 15s”
static const int64_t kRunMsFromFirstFrame = 15000;
static volatile int  g_first_frame_set = 0;
static int64_t       g_first_frame_ms  = 0;
static int           g_first_frame_ch  = -1;
static pthread_mutex_t g_first_frame_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_ms_monotonic(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_first_frame_if_needed(ChannelCtx *ch, int is_video)
{
    if (!ch) return;
    if (!is_video) return;
    if (g_first_frame_set) return;

    pthread_mutex_lock(&g_first_frame_lock);
    if (!g_first_frame_set) {
        g_first_frame_ms  = now_ms_monotonic();
        g_first_frame_ch  = ch->id;
        g_first_frame_set = 1;
        printf("[Ch%d] First video packet seen -> start timer (15s)\n", ch->id);
    }
    pthread_mutex_unlock(&g_first_frame_lock);
}

// pause 控制接口
void mainapp_set_pause(int ch_id, int pause) {
    if (ch_id < 0 || ch_id >= g_channel_count) 
		return;
    g_ch[ch_id].p2p_paused = pause;
    printf(MAIN_LOG_PREFIX "Channel %d P2P Paused State set to: %d\n", ch_id, pause);
}

// for SIGINT
void main_app_request_exit(void)
{
    g_running = 0;
}

static int try_create_muxer(ChannelCtx *ch)
{
    if (!ch) return -1;
    if (!ch->enable_record) return 0;
    if (ch->rec_muxer) return 0;
    if (!ch->muxer_pending) return 0;
    if (!ch->v_par_ref) return 0;

    // rate limit retry
    int64_t now = now_ms_monotonic();
    if (ch->muxer_last_try_ms != 0 && (now - ch->muxer_last_try_ms) < 500) {
        return 0;
    }
    ch->muxer_last_try_ms = now;

    // must have dimensions for mp4+h264
    if (ch->v_par_ref->width <= 0 || ch->v_par_ref->height <= 0) {
        printf("[Ch%d] muxer pending: video w/h still not ready (%dx%d)\n",
               ch->id, ch->v_par_ref->width, ch->v_par_ref->height);
        ch->muxer_retry_cnt++;
        return 0;
    }

    printf("[Ch%d] try create muxer: file=%s w=%d h=%d extra=%d\n",
           ch->id, ch->record_file,
           ch->v_par_ref->width, ch->v_par_ref->height, ch->v_par_ref->extradata_size);

    const AVCodecParameters *use_apar = NULL;
    AVRational use_atb = (AVRational){0,1};

    if (ch->a_par_ref && ch->audio_index >= 0) {
        if (ch->a_par_ref->sample_rate > 0 && ch->a_par_ref->extradata_size > 0) {
            use_apar = ch->a_par_ref;
            use_atb  = ch->a_tb;
        } else {
            printf("[Ch%d] audio not ready (sr=%d extra=%d) -> record video only\n",
                   ch->id, ch->a_par_ref->sample_rate, ch->a_par_ref->extradata_size);
        }
    }

    int ret = FFmpegMuxer_Create(&ch->rec_muxer,
                                 ch->record_file,
                                 ch->v_par_ref, ch->v_tb,
                                 use_apar, use_atb);
    if (ret < 0 || !ch->rec_muxer) {
        char errbuf[64];
        av_strerror(ret, errbuf, sizeof(errbuf));
        printf("[Ch%d] muxer create failed ret=%d (%s), will keep pending and retry.\n",
               ch->id, ret, errbuf);

        ch->muxer_retry_cnt++;
        if (ch->muxer_retry_cnt > 200) {
            printf("[Ch%d] muxer retry too many times -> disable recording.\n", ch->id);
            ch->enable_record = 0;
            ch->muxer_pending = 0;
        }
        return ret;
    }

    printf("[Ch%d] Muxer create OK\n", ch->id);
    ch->muxer_pending = 0;
    return 0;
}

// disk writer thread
static void* disk_writer_thread(void* arg)
{
    ChannelCtx *ch = (ChannelCtx*)arg;
    AVPacket pkt;
    int pkt_cnt = 0;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    printf("[Ch%d] Disk writer thread started. rec=%d file=%s\n",
           ch->id, ch->enable_record, ch->record_file);

    while (ch->writer_running) {

        if (ch->enable_record && !ch->rec_muxer) {
            try_create_muxer(ch);
            usleep(10 * 1000);
            continue;
        }

        int r = packet_queue_get(&ch->queue_disk, &pkt, 1);
        if (r < 0) {
            printf("[Ch%d] Disk writer: queue aborted, exit loop.\n", ch->id);
            break;
        } else if (r == 0) {
            continue;
        }

        // --- KEYFRAME WAIT LOGIC ---
        // If we haven't seen a keyframe yet, check this packet
        if (ch->enable_record && !ch->has_seen_keyframe) {
            int is_video = (pkt.stream_index == ch->video_index);
            if (is_video) {
                if (pkt.flags & AV_PKT_FLAG_KEY) {
                    printf("[Ch%d] >>> FOUND FIRST KEYFRAME (size=%d) <<< Starting write.\n", ch->id, pkt.size);
                    ch->has_seen_keyframe = 1;
                } else {
                    // DROP until keyframe
                    static int drop_cnt = 0;
                    drop_cnt++;
                    if (drop_cnt % 30 == 0) printf("[Ch%d] Waiting for keyframe... dropped %d packets\n", ch->id, drop_cnt);
                    av_packet_unref(&pkt);
                    continue; 
                }
            } else {
                // Drop audio until video starts
                av_packet_unref(&pkt);
                continue;
            }
        }

        pkt_cnt++;
        if ((pkt_cnt % 50) == 0) {
             printf("[Ch%d] Disk writer proc pkt #%d (sz=%d idx=%d key=%d)\n", 
                  ch->id, pkt_cnt, pkt.size, pkt.stream_index, (pkt.flags & AV_PKT_FLAG_KEY));
        }

        if (ch->enable_record && ch->rec_muxer) {
            int is_video = (pkt.stream_index == ch->video_index);
            int is_audio = (pkt.stream_index == ch->audio_index);
            if (is_video || is_audio) {
                int ret = FFmpegMuxer_Write(ch->rec_muxer, &pkt, is_video, is_audio);
                if (ret < 0) {
                    char errbuf[64];
                    av_strerror(ret, errbuf, sizeof(errbuf));
                    printf("[Ch%d] Disk writer: FFmpegMuxer_Write failed ret=%d (%s)\n", ch->id, ret, errbuf);
                }
            }
        }

        av_packet_unref(&pkt);
    }

    printf("[Ch%d] Disk writer thread exit. Total processed=%d\n", ch->id, pkt_cnt);
    return NULL;
}

// net writer thread (P2P/Forward 线程 - 受暂停影响)
static void* net_writer_thread(void* arg)
{
    ChannelCtx *ch = (ChannelCtx*)arg;
    AVPacket pkt;

    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;

    while (ch->writer_running) {
        // 如果这里没有数据(因为被 paused 拦截了)，这个线程会阻塞在 get 上
        // 或者如果 get 是非阻塞的，它会空转。
        // 假设 packet_queue_get 是阻塞的 (block=1)
        int ret = packet_queue_get(&ch->queue_net, &pkt, 1);
        if (ret < 0) break;
        else if (ret == 0) continue;

        if (ch->enable_forward && ch->forwarder) {
            RtspForwarder_Write(ch->forwarder, &pkt);
        }
        av_packet_unref(&pkt);
    }
    return NULL;
}


// packet callback (分流)
static void on_rtsp_packet_cb(AVPacket* pkt, void* user_data)
{
    ChannelCtx *ch = (ChannelCtx*)user_data;
    if (!pkt || !ch) return;

    int is_video = (pkt->stream_index == ch->video_index);
    set_first_frame_if_needed(ch, is_video);

    if (ch->enable_record && !ch->rec_muxer && ch->muxer_pending) {
        try_create_muxer(ch);
    }

    PacketType type = PKT_TYPE_OTHER;
    if (pkt->stream_index == ch->video_index) type = PKT_TYPE_VIDEO;
    else if (pkt->stream_index == ch->audio_index) type = PKT_TYPE_AUDIO;

    // 1. 录像队列：无条件写入
    if (ch->enable_record) {
        packet_queue_put(&ch->queue_disk, pkt, type);
    }

    // 2. P2P 队列：受控写入
    if (ch->enable_forward) {
        if (!ch->p2p_paused) {
            packet_queue_put(&ch->queue_net, pkt, type);
        } else {
            // Drop for P2P, save bandwidth
            printf("Drop P2P packet\n");
        }
    }
}


// info callback
static void on_rtsp_info_cb(int v_index,
                            AVCodecParameters* v_par, AVRational v_tb,
                            int a_index,
                            AVCodecParameters* a_par, AVRational a_tb,
                            void* user_data)
{
    ChannelCtx *ch = (ChannelCtx*)user_data;
    if (!ch) return;

    ch->video_index = v_index;
    ch->audio_index = a_index;
    ch->v_tb        = v_tb;
    ch->a_tb        = a_tb;

    ch->v_par_ref   = v_par;
    ch->a_par_ref   = a_par;
    
    // DIRECTLY SET 1920x1080 if not detected
    if (ch->v_par_ref) {
        if (ch->v_par_ref->width <= 0)  ch->v_par_ref->width = 1920;
        if (ch->v_par_ref->height <= 0) ch->v_par_ref->height = 1080;
    }

    printf("[Ch%d] RTSP Info: v_idx=%d a_idx=%d v_tb=%d/%d a_tb=%d/%d\n",
           ch->id, v_index, a_index, v_tb.num, v_tb.den, a_tb.num, a_tb.den);

    if (v_par) {
        printf("[Ch%d] video par: codec=%d w=%d h=%d extra=%d\n",
               ch->id, v_par->codec_id, v_par->width, v_par->height, v_par->extradata_size);
    }
    if (a_par) {
        printf("[Ch%d] audio par: codec=%d sr=%d ch=%d extra=%d\n",
               ch->id, a_par->codec_id, a_par->sample_rate, a_par->channels, a_par->extradata_size);
    }

    if (ch->enable_forward && ch->out_rtsp_url[0] != '\0' && !ch->forwarder) {
        ch->forwarder = RtspForwarder_Create(ch->out_rtsp_url,
                                             ch->video_index, v_par, ch->v_tb,
                                             ch->audio_index, a_par, ch->a_tb);
        if (!ch->forwarder) {
            printf("[Ch%d] Forwarder create failed, disable forwarding.\n", ch->id);
            ch->enable_forward = 0;
        } else {
            printf("[Ch%d] Forwarder create OK\n", ch->id);
        }
    }

    if (ch->enable_record) {
        ch->muxer_pending = 1;
        ch->muxer_retry_cnt = 0;
        ch->muxer_last_try_ms = 0;
        try_create_muxer(ch);
    }
}

int main_app(int argc, char *argv[])
{
    g_running = 1;

    // Set callback to see FFmpeg logs
    av_log_set_callback(ffmpeg_log_callback);
    av_log_set_level(AV_LOG_INFO); // See useful info

    pthread_mutex_lock(&g_first_frame_lock);
    g_first_frame_set = 0;
    g_first_frame_ms  = 0;
    g_first_frame_ch  = -1;
    pthread_mutex_unlock(&g_first_frame_lock);

    if (!argv || argc <= 0) return -1;

    if ((argc - 1) <= 0 || ((argc - 1) % 3) != 0) {
        printf(MAIN_LOG_PREFIX "Usage: %s <in> <out|-> <rec|->...\n", argv[0]);
        return -1;
    }

    g_channel_count = (argc - 1) / 3;
    if (g_channel_count > MAX_CHANNELS) g_channel_count = MAX_CHANNELS;

    printf(MAIN_LOG_PREFIX "argc=%d, channels=%d\n", argc, g_channel_count);

    av_register_all();
    avformat_network_init();

    for (int i = 0; i < g_channel_count; ++i) {
        ChannelCtx *ch = &g_ch[i];
        memset(ch, 0, sizeof(*ch));
        ch->id = i;

        const char* in_rtsp  = argv[1 + i*3 + 0];
        const char* out_rtsp = argv[1 + i*3 + 1];
        const char* rec_path = argv[1 + i*3 + 2];

        if (!in_rtsp)  in_rtsp  = "";
        if (!out_rtsp) out_rtsp = "-";
        if (!rec_path) rec_path = "-";

        strncpy(ch->in_rtsp_url, in_rtsp, sizeof(ch->in_rtsp_url)-1);
        if (strcmp(out_rtsp, "-") == 0) ch->enable_forward = 0;
        else {
            ch->enable_forward = 1;
            strncpy(ch->out_rtsp_url, out_rtsp, sizeof(ch->out_rtsp_url)-1);
        }

        if (strcmp(rec_path, "-") == 0) ch->enable_record = 0;
        else {
            ch->enable_record = 1;
            strncpy(ch->record_file, rec_path, sizeof(ch->record_file)-1);
        }

        printf("[Ch%d] Config:\n", ch->id);
        printf("       in_rtsp = %s\n", ch->in_rtsp_url);
        printf("       out_rtsp= %s (%s)\n", ch->out_rtsp_url, ch->enable_forward ? "forward ON" : "forward OFF");
        printf("       rec_file= %s (%s)\n", ch->record_file, ch->enable_record ? "record ON" : "record OFF");

        // [FIXED] Init queues with split memory pool sizes
        // queue_disk: Video=2048, Audio=1024 (Robust buffer)
        packet_queue_init(&ch->queue_disk, 2048, 1024); 
        // queue_net: Video=1024, Audio=256 (Faster buffer)
        packet_queue_init(&ch->queue_net, 1024, 256);  

        ch->writer_running = 1;
        ch->has_seen_keyframe = 0; // Reset keyframe flag

        if (ch->enable_record) pthread_create(&ch->tid_disk, NULL, disk_writer_thread, ch);
        if (ch->enable_forward) pthread_create(&ch->tid_net, NULL, net_writer_thread, ch);

        if (ch->v_par_copy) avcodec_parameters_free(&ch->v_par_copy);
        if (ch->a_par_copy) avcodec_parameters_free(&ch->a_par_copy);

        ch->puller = RtspPuller_Create(ch->in_rtsp_url, on_rtsp_info_cb, on_rtsp_packet_cb, ch);
        if (!ch->puller) printf("[Ch%d] Puller create failed\n", ch->id);
        else printf("[Ch%d] Puller create OK\n", ch->id);
    }

    printf(MAIN_LOG_PREFIX "Running main loop... (will exit 15s after first video packet)\n");

    while (g_running) {
        usleep(200 * 1000);

        if (g_first_frame_set) {
            int64_t start_ms;
            int start_ch;

            pthread_mutex_lock(&g_first_frame_lock);
            start_ms = g_first_frame_ms;
            start_ch = g_first_frame_ch;
            pthread_mutex_unlock(&g_first_frame_lock);

            int64_t elapsed = now_ms_monotonic() - start_ms;
            if (elapsed >= kRunMsFromFirstFrame) {
                printf(MAIN_LOG_PREFIX "Reached %lld ms since first video packet (Ch%d). Exit now.\n",
                       (long long)elapsed, start_ch);
                g_running = 0;
                break;
            }
        }
    }

    printf(MAIN_LOG_PREFIX "Exiting main loop, start cleanup...\n");

    for (int i = 0; i < g_channel_count; ++i) {
        ChannelCtx *ch = &g_ch[i];

        if (ch->puller) {
            RtspPuller_Stop(ch->puller);
            RtspPuller_Destroy(ch->puller);
            ch->puller = NULL;
        }

        ch->writer_running = 0;
        packet_queue_abort(&ch->queue_disk);
        packet_queue_abort(&ch->queue_net);

        if (ch->enable_record) pthread_join(ch->tid_disk, NULL);
        if (ch->enable_forward) pthread_join(ch->tid_net, NULL);

        packet_queue_destroy(&ch->queue_disk);
        packet_queue_destroy(&ch->queue_net);

        if (ch->forwarder) {
            RtspForwarder_Destroy(ch->forwarder);
            ch->forwarder = NULL;
        }
        if (ch->rec_muxer) {
            FFmpegMuxer_Close(ch->rec_muxer);
            ch->rec_muxer = NULL;
        }
    }

    avformat_network_deinit();
    printf(MAIN_LOG_PREFIX "Exit.\n");
    return 0;
}

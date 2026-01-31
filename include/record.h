#ifndef __RECORD_H__
#define __RECORD_H__

#include <pthread.h>
#include <libavformat/avformat.h>

#include "common.h"        // StationHandle definition
#include "camera_manage.h" // CAM_MAX_CNT
#include "stream.h"        // RtspCtx
#include "packet_queue.h"  // PacketQueue

#ifndef RECORD_SLICE_MIN
#define RECORD_SLICE_MIN 1
#endif
#define SLICE_SECONDS    (RECORD_SLICE_MIN * 60)
#define SLICE_MS         ((int64_t)SLICE_SECONDS * 1000)

// PREBUF macro
#define PREBUF_MAX_PKTS   1200
#define PREBUF_MAX_BYTES  (12 * 1024 * 1024)   // 12MB

// --- Structure Definitions ---

/* Pre-recording buffer */
typedef struct {
    AVPacket pkts[PREBUF_MAX_PKTS];
    int head;
    int count;
    int64_t bytes;
} PreBuf;

/* Recording channel context */
typedef struct {
    struct RecordHandle *Record;   // Pointer to parent handle
    RtspCtx *Rtsp;                 // Pointer to source stream context

    pthread_t Thread;
    int thread_created;

    char FileName[128];

    // Codec Params Cache
    AVCodecParameters *v_codecpar_cache;
    int v_extradata_cached;
    int v_width_cache;
    int v_height_cache;

    // Output Streams
    AVStream *v_st;
    AVStream *a_st;
    AVRational v_src_tb;
    AVRational a_src_tb;
    // Timestamp Normalization
    int64_t v_start_dts;
    int v_start_dts_inited;
    int64_t v_last_dts;
    int64_t v_last_pts;

    int64_t a_start_dts;
    int a_start_dts_inited;
    int64_t a_last_dts;
    int64_t a_last_pts;

    // State
    int64_t last_io_error_ms;
    int64_t t_thread_start_ms;
} RecordCtx;

/* Record module main handle */
typedef struct RecordHandle {
    StationHandle   *Station;
    pthread_mutex_t Mutex;
    RecordCtx        Ctx[CAM_MAX_CNT];
    void            *Priv[0];
} RecordHandle;

// --- Function Prototypes ---
int32_t Record_Init(StationHandle *Station);
void Record_Deinit(StationHandle *Station);
int32_t Record_Start(StationHandle *Station, int32_t Index);
int32_t Record_Stop(StationHandle *Station, int32_t Index);

#endif

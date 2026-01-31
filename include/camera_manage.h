#ifndef __CAMERA_MANAGE_H__
#define __CAMERA_MANAGE_H__

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>  // [新增] 必须包含，用于 pthread_mutex_t

#include "common.h"

#define CAM_MAX_CNT 4

#define CAM_INFO                "/config/cam_info"


#define MSG_FLAG_CAMERA         (0<<31)
#define MSG_FLAG_STATION        (1<<31)
#define MSG_FLAG_MASK           (1<<31)
#define MSG_ID_MASK                      (0x7fffffff)

#define MSG_TYPE_CAM2STA(Id)    (Id | MSG_FLAG_STATION)
#define MSG_TYPE_STA2CAM(Id)    (Id | MSG_FLAG_CAMERA)

#define MSG_IS_CAM2STA(Type)    (((Type) & MSG_FLAG_MASK) == MSG_FLAG_STATION)
#define MSG_IS_STA2CAM(Type)    (((Type) & MSG_FLAG_MASK) == MSG_FLAG_CAMERA)

enum {
        MSG_CAM_INFO,
        MSG_STREAM_INFO,
        MSG_WIFI_HALOW_INFO,
        MSG_STREAM_READY,
        MSG_KEEP_ALIVE,

        MSG_CHANGE_RESOLUTION,
        MSG_REQ_STREAM,
        MSG_CONFIG_WIFI_HALOW,
        MSG_REQ_IFRAME,
        MSG_UPGRADE,
        MSG_SEND_FILE,

        MSG_SYNC_DATE_TIME,
};

typedef struct stream_info {
        int32_t VideoWidth;
        int32_t VideoHeight;
        int32_t VideoFrameRate;
        int32_t AudioSampleRate;
} StreamInfo;

typedef struct wifi_halow_info {
        int32_t Rssi;
        int32_t Evm;
} WifiHalowInfo;

typedef struct msg_packet {
        int32_t Type;
        int32_t Len;
        int32_t CheckSum;
        char Data[0];
} MsgPacket;

typedef struct camera_info {
        int32_t DevIndex;
        int32_t Sock;
        char DevId[32];
        char Addr[24];
        char FwVersion[16];
        int32_t IsAlive;
        int32_t DisconCnt;
} CameraInfo;

struct CamManageHandle {
        StationHandle  *Station;

    // --- [新增] 流控相关成员 ---
        // 记录当前用户选择的通道 ID
        // -1: 代表四分屏（全看）
        // 0~3: 代表只看某一路，其他的暂停
        int32_t FocusIndex;
        pthread_mutex_t FocusMutex; // 保护 FocusIndex 的读写
        int64_t LastActionTime;     // 上一次启动通道的时间 (ms)
        int32_t ActionCount;        // 短时间内的连续动作计数

        pthread_cond_t FlowCond;
        pthread_t ConnThread;
        pthread_t FlowThread;       // [新增] 流控线程句柄
        int32_t ListenSock;
        CameraInfo Camera[CAM_MAX_CNT];
        int32_t CameraConnectedCnt;
        int32_t CameraBindCnt;
        pthread_mutex_t CamInfoMutex;
};

int32_t CamManage_Init(StationHandle *Station);
void CamManage_Deinit(StationHandle *Station);
int32_t CamManage_Send(StationHandle *Station, int32_t Index, int32_t Type, char *Data, int32_t Len);
int64_t CamManage_GetCamDevIdByIndex(StationHandle *Station, int32_t Index);
int32_t CamManage_RemoveCamInfo(StationHandle *Station);
void CamManage_SetFocusChannel(StationHandle *Station, int32_t Index);

#endif

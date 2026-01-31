#ifndef __P2P_H__
#define __P2P_H__

#include "IOTCAPIs.h"
#include "AVAPIs.h"
#include "P2PCam/AVFRAMEINFO.h"
#include "P2PCam/AVIOCTRLDEFs.h"
#include "IOTCAPIs.h"
#include "IOTCWakeUp.h"

#include "common.h"
#include "camera_manage.h"
#include "stream.h"

#define STATE_LOGINING          0
#define STATE_LOGIN_DONE        1
#define STATE_EXIT                      2

#define USAGERATE_CTRL          0

/////////////////////////////////////////////////////////////////////////////////
/////////////////// Message Type Defined By LONGZY////////////////////////////////
/////////////////////////////////////////////////////////////////////////////////

// AVIOCTRL Message Type
enum
{
        IOTYPE_USER_IPCAM_SET_ZOOM_REQ                                  = 0x30000000,
        IOTYPE_USER_IPCAM_SET_ZOOM_RESP                                 = 0x30000001,
        IOTYPE_USER_IPCAM_GET_INFO_REQ                                  = 0x30000002,
        IOTYPE_USER_IPCAM_GET_INFO_RESP                                 = 0x30000003,
        IOTYPE_USER_IPCAM_QUERY_EVENT_REQ                               = 0x30000004,
        IOTYPE_USER_IPCAM_QUERY_EVENT_RESP                              = 0x30000005,
        IOTYPE_USER_IPCAM_PTZ_COMMAND_LZY                               = 0x30000006,
};

/*
IOTYPE_USER_IPCAM_SET_ZOOM_REQ                          = 0x30000000,
** @struct SMsgAVIoctrlSetZoomReq
*/
typedef struct
{
        unsigned int channel; // Camera Index
        unsigned char ratio;               // 1x~16x
        unsigned char reserved[3];
}SMsgAVIoctrlSetZoomReq;

/*
IOTYPE_USER_IPCAM_SET_ZOOM_RESP                         = 0x30000001,
** @struct SMsgAVIoctrlSetZoomResp
*/
typedef struct
{
        int result;     // 0: success; otherwise: failed.
        unsigned char reserved[4];
}SMsgAVIoctrlSetZoomResp;

/*
IOTYPE_USER_IPCAM_GET_INFO_REQ                          = 0x30000002,
** @struct SMsgAVIoctrlGetInfoReq
*/
typedef struct
{
        unsigned int channel; // Camera Index
        unsigned char reserved[4];
}SMsgAVIoctrlGetInfoReq;

/*
IOTYPE_USER_IPCAM_GET_INFO_RESP                         = 0x30000003,
** @struct SMsgAVIoctrlGetInfoResp
*/
typedef struct
{
        unsigned int channel; // Camera Index
        unsigned char rssi;                // 0~5
        unsigned char battery_level;               // 0~100
        unsigned char reserved[2];
}SMsgAVIoctrlGetInfoResp;

/*
IOTYPE_USER_IPCAM_QUERY_EVENT_REQ                       = 0x30000004,
** @struct SMsgAVIoctrlQueryEventReq
*/
typedef struct
{
        unsigned int channel; // Camera Index
        unsigned short year;
        unsigned char month;
        unsigned char reserved;
}SMsgAVIoctrlQueryEventReq;

/*
IOTYPE_USER_IPCAM_QUERY_EVENT_RESP                      = 0x30000005,
** @struct SMsgAVIoctrlQueryEventResp
*/
typedef struct
{
        unsigned int channel; // Camera Index
        unsigned char result[32];
}SMsgAVIoctrlQueryEventResp;


typedef enum {
        VIDEO_FRAME_TYPE_PBFRAME = 0, // P Frame
        VIDEO_FRAME_TYPE_IFRAME  = 1, // I Frame
} VideoFrameType;


#define CLIENT_MAX_CNT          5

typedef struct {
        int32_t         avIndex;
        uint8_t         bEnableAudio;
        uint8_t         bEnableVideo;
        uint8_t         bEnableVideoReqIFrame;
        uint8_t         bEnableSpeaker;
        uint8_t         bStopPlayBack;
        uint8_t         bPausePlayBack;
        uint8_t         bTwoWayStream;
        int32_t         speakerCh;
        int32_t         playBackCh;
        int32_t         playBackProgress;
        float           BufUsageRate;
        SMsgAVIoctrlPlayRecord  playRecord;
        pthread_rwlock_t                sLock;
} ClientInfo;

typedef struct {
        P2pHandle *P2p;
        RtspCtx *Ctx;
        ClientInfo      Client[CLIENT_MAX_CNT];
        pthread_t SendThread;
} CameraStream;

struct P2pHandle {
        StationHandle  *Station;
        uint8_t         IsExit;
        uint8_t         HeartBeatTimeout;
        int32_t         State;
        int32_t         OnlineNum;
        int32_t         ListenTimeout;
        uint32_t        MaxClientNum;
        char            User[18];
        char            Passwd[18];
        char            UID[24];
        char            Authkey[12];
        int32_t         SessionId;
        CameraStream  CamStream[CAM_MAX_CNT];
        pthread_t   ListenThd;
        pthread_t       LoginThd;
        void            *Priv[0];
};


int32_t P2P_Init(StationHandle *Station);
void P2P_Deinit(StationHandle *Station);
int32_t P2P_Start(StationHandle *Station, int32_t Index);
int32_t P2P_Stop(StationHandle *Station, int32_t Index);

#endif //__P2P_H__

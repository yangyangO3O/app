#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <pthread.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "profile.h"
#include "system.h"
#include "network.h"
#include "p2p.h"
#include "stream.h"          // [Added] for Stream_SetPause
#include "camera_manage.h"   // [Added] for CamManage_SetFocusChannel
#include "packet_queue.h"

#define TAG                     "P2P"
#define LISTEN_TIMEOUT          100
#define LICENSE_KEY             "/config/license"
#define ENABLE_TOKEN_AUTH       0
#define ENABLE_DTLS             0
#define ENABLE_RESEND           1
#define SERVTYPE_STREAM_SERVER  0
#define MAX_SIZE_IOCTRL_BUF     512
#define AUDIO_BUF_SIZE          512
#define MAX_HEART_CHANNEL       2
#define RECORD_PATH             "/mnt/sdcard" 

// 定义可能缺失的宏，防止原始注释代码报错
#ifndef LED_CAM_1
#define LED_CAM_1 0
#define LED_CAM_2 0
#define LED_CAM_3 0
#define LED_CAM_4 0
#endif

static P2pHandle *gP2p = NULL;

static void P2P_LoginCallback(uint32_t nLoginInfo)
{
    LOG_INFO(TAG, "I can be connected via Internet: %x\n", nLoginInfo);
    if((nLoginInfo & 0x04)) {
        LOG_INFO(TAG, "I can be connected via Internet\n");
    }
    else if((nLoginInfo & 0x08)) {
        LOG_INFO(TAG, "I am be banned by IOTC Server because UID multi-login\n");
    }
}

static int32_t P2P_PasswordAuthCallBack(const char *account, char *pwd, unsigned int pwd_buf_size)
{
    LOG_INFO(TAG, "account: %s, pwd: %s\n", account, pwd);
    if (strcmp(account, gP2p->User) != 0){
        return -1;
    }
    if (pwd_buf_size <= strlen(gP2p->Passwd)){
        return -1;
    }

    strcpy(pwd, gP2p->Passwd);
    return 0;
}

static int32_t P2P_ChangePasswordCallBack(
        int av_index,
        const char *account,
        const char *old_password,
        const char *new_password,
        const char *new_iotc_authkey){

    if (strcmp(account, gP2p->User) != 0) {
        return -1;
    }
    if (strcmp(old_password, gP2p->Passwd) != 0) {
        return -1;
    }

    // please make sure the maximum password size is enough
    if (strlen(new_password) > NEW_MAXSIZE_VIEWPWD) {
        return -1;
    }
    strcpy(gP2p->User, new_password);

    //auth key has been changed inside the SDK, please save it
    memcpy(gP2p->Authkey, new_iotc_authkey, IOTC_AUTH_KEY_LENGTH);

    IOTC_Device_Update_Authkey(gP2p->Authkey);
    LOG_INFO(TAG, "Account:%s\n", gP2p->User);
    LOG_INFO(TAG, "Password:%s\n", gP2p->Passwd);
    LOG_INFO(TAG, "Authkey:%s\n", gP2p->Authkey);

    return 0; //success
}

static void P2P_AbilityRequest(int av_index, avServSendAbility send_ability)
{
    send_ability(av_index, (const unsigned char*)"NULL", 4);
}

static void P2P_PrintErrHandling(int32_t ErrorCode)
{
    //LOG_ERROR(TAG, "[Error code : %d]\n", ErrorCode );
    switch (ErrorCode)
    {
    case IOTC_ER_SERVER_NOT_RESPONSE :
        LOG_ERROR(TAG, "Master doesn't respond.\n");
        LOG_ERROR(TAG, "Please check the network wheather it could connect to the Internet.\n");
        break;
    case IOTC_ER_FAIL_RESOLVE_HOSTNAME :
        LOG_ERROR(TAG, "Can't resolve hostname.\n");
        break;
    case IOTC_ER_ALREADY_INITIALIZED :
        LOG_ERROR(TAG, "Already initialized.\n");
        break;
    case IOTC_ER_FAIL_CREATE_MUTEX :
        LOG_ERROR(TAG, "Can't create mutex.\n");
        break;
    case IOTC_ER_FAIL_CREATE_THREAD :
        LOG_ERROR(TAG, "Can't create thread.\n");
        break;
    case IOTC_ER_UNLICENSE :
        LOG_ERROR(TAG, "This UID is unlicense.\n");
        LOG_ERROR(TAG, "Check your UID.\n");
        break;
    case IOTC_ER_NOT_INITIALIZED :
        LOG_ERROR(TAG, "Please initialize the IOTCAPI first.\n");
        break;
    case IOTC_ER_TIMEOUT :
        //-13 IOTC_ER_TIMEOUT
        break;
    case IOTC_ER_INVALID_SID :
        LOG_ERROR(TAG, "This SessionId is invalid.\n");
        LOG_ERROR(TAG, "Please check it again.\n");
        break;
    case IOTC_ER_EXCEED_MAX_SESSION :
        LOG_ERROR(TAG, "[Warning] The amount of session reach to the maximum.\n");
        LOG_ERROR(TAG, "It cannot be connected unless the session is released.\n");
        break;
    case IOTC_ER_CAN_NOT_FIND_DEVICE :
        LOG_ERROR(TAG, "Device didn't register on server, so we can't find device.\n");
        LOG_ERROR(TAG, "Please check the device again.\n");
        LOG_ERROR(TAG, "Retry...\n");
        break;
    case IOTC_ER_SESSION_CLOSE_BY_REMOTE :
        LOG_ERROR(TAG, "Session is closed by remote so we can't access.\n");
        LOG_ERROR(TAG, "Please close it or establish session again.\n");
        break;
    case IOTC_ER_REMOTE_TIMEOUT_DISCONNECT :
        LOG_ERROR(TAG, "We can't receive an acknowledgement character within a TIMEOUT.\n");
        LOG_ERROR(TAG, "It might that the session is disconnected by remote.\n");
        LOG_ERROR(TAG, "Please check the network wheather it is busy or not.\n");
        LOG_ERROR(TAG, "And check the device and user equipment work well.\n");
        break;
    case IOTC_ER_DEVICE_NOT_LISTENING :
        LOG_ERROR(TAG, "Device doesn't listen or the sessions of device reach to maximum.\n");
        LOG_ERROR(TAG, "Please release the session and check the device wheather it listen or not.\n");
        break;
    case IOTC_ER_CH_NOT_ON :
        LOG_ERROR(TAG, "Channel isn't on.\n");
        LOG_ERROR(TAG, "Please open it by IOTC_Session_Channel_ON() or IOTC_Session_Get_Free_Channel()\n");
        LOG_ERROR(TAG, "Retry...\n");
        break;
    case IOTC_ER_SESSION_NO_FREE_CHANNEL :
        LOG_ERROR(TAG, "All channels are occupied.\n");
        LOG_ERROR(TAG, "Please release some channel.\n");
        break;
    case IOTC_ER_TCP_TRAVEL_FAILED :
        LOG_ERROR(TAG, "Device can't connect to Master.\n");
        LOG_ERROR(TAG, "Don't let device use proxy.\n");
        LOG_ERROR(TAG, "Close firewall of device.\n");
        LOG_ERROR(TAG, "Or open device's TCP port 80, 443, 8080, 8000, 21047.\n");
        break;
    case IOTC_ER_TCP_CONNECT_TO_SERVER_FAILED :
        LOG_ERROR(TAG, "Device can't connect to server by TCP.\n");
        LOG_ERROR(TAG, "Don't let server use proxy.\n");
        LOG_ERROR(TAG, "Close firewall of server.\n");
        LOG_ERROR(TAG, "Or open server's TCP port 80, 443, 8080, 8000, 21047.\n");
        LOG_ERROR(TAG, "Retry...\n");
        break;
    case IOTC_ER_EXIT_LISTEN:
        LOG_ERROR(TAG, "Stop listening for connections from clients.\n");
        break;
    case IOTC_ER_NO_PERMISSION :
        LOG_ERROR(TAG, "This UID's license doesn't support TCP.\n");
        break;
    case IOTC_ER_NETWORK_UNREACHABLE :
        LOG_ERROR(TAG, "Network is unreachable.\n");
        LOG_ERROR(TAG, "Please check your network.\n");
        LOG_ERROR(TAG, "Retry...\n");
        break;
    case IOTC_ER_FAIL_SETUP_RELAY :
        LOG_ERROR(TAG, "Client can't connect to a device via Lan, P2P, and Relay mode\n");
        break;
    case IOTC_ER_NOT_SUPPORT_RELAY :
        LOG_ERROR(TAG, "Server doesn't support UDP relay mode.\n");
        LOG_ERROR(TAG, "So client can't use UDP relay to connect to a device.\n");
        break;
    default :
        break;
    }
}

static void P2P_RegeditClientToVideo(ClientInfo *Client, int32_t avIndex)
{
    Client->avIndex = avIndex;
    Client->bEnableVideo = 1;
    Client->bEnableVideoReqIFrame = 1;
}

static void P2P_UnRegeditClientFromVideo(ClientInfo *Client)
{
    Client->bEnableVideo = 0;
    Client->bEnableVideoReqIFrame = 0;
}

static void P2P_RegeditClientToAudio(ClientInfo *Client, int32_t avIndex)
{
    Client->bEnableAudio = 1;
}

static void P2P_UnRegeditClientFromAudio(ClientInfo *Client)
{
    Client->bEnableAudio = 0;
}

static void P2P_HandleIOCtrlCmd(P2pHandle *P2p, int32_t SessionId, int32_t avIndex, char *Data, int32_t type)
{
    int32_t Chn;
    int32_t LockRet;
    ClientInfo *Client;

    Chn = *((int32_t *)Data);
    if(Chn < 0 || Chn >= CAM_MAX_CNT) {
        LOG_ERROR(TAG, "Invalid channel %d\n", Chn);
        return ;
    }
    Client = &P2p->CamStream[Chn].Client[SessionId];

    if (type == IOTYPE_USER_IPCAM_START || type == IOTYPE_USER_IPCAM_STOP) {
        LOG_INFO(TAG, "Handle CMD: %x (Ch:%d)\n", type, Chn);
    }
    
    switch(type)
    {
        case IOTYPE_USER_IPCAM_START:
        {
            SMsgAVIoctrlAVStream *p = (SMsgAVIoctrlAVStream *)Data;
            LOG_INFO(TAG, "IOTYPE_USER_IPCAM_START, ch:%d, avIndex:%d\n\n", p->channel, avIndex);
            
            //get writer lock
            LockRet = pthread_rwlock_wrlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
            }
            P2P_RegeditClientToVideo(Client, avIndex);
            //release lock
            LockRet = pthread_rwlock_unlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
            }

            // 【核心联动】: 告诉 CameraManage 当前用户正在看哪个通道
            if (P2p->Station) {
                // 1. 设置焦点通道
                CamManage_SetFocusChannel(P2p->Station, Chn);
                
                // 2. 强制立即恢复当前通道
                Stream_SetPause(P2p->Station, Chn, 0);
                
                // 3. 请求关键帧 (确保画面秒开)
                Stream_RequestIFrame(P2p->Station, Chn);
            }

            //if(P2p->Callback.RequestIDR) {
            //  P2p->Callback.RequestIDR();
            //}
            LOG_INFO(TAG, "P2P_RegeditClientToVideo OK\n");
            
            int32_t LedNum = 0;
            if (Chn == 0) {
                LedNum = LED_CAM_1;
            }
            else if (Chn == 1) {
                LedNum = LED_CAM_2;
            }
            else if (Chn == 2) {
                LedNum = LED_CAM_3;
            }
            else if (Chn == 3) {
                LedNum = LED_CAM_4;
            }
            if (LedNum && P2p->CamStream[Chn].Ctx && P2p->CamStream[Chn].Ctx->running == 2) {
                System_LedBlink(P2p->Station, LedNum, 500);
            }
            break;
        }
        // -----------------------------------------------------------
        // 场景 2: App 请求停止观看某一路视频 (STOP)
        // -----------------------------------------------------------
        case IOTYPE_USER_IPCAM_STOP:
        {
            SMsgAVIoctrlAVStream *p = (SMsgAVIoctrlAVStream *)Data;
            LOG_INFO(TAG, "IOTYPE_USER_IPCAM_STOP, ch:%d, avIndex:%d\n", p->channel, avIndex);
            
            // 1. 注销客户端状态 (P2P内部逻辑)
            LockRet = pthread_rwlock_wrlock(&Client->sLock);
            if(LockRet) LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
            P2P_UnRegeditClientFromVideo(Client);
            LockRet = pthread_rwlock_unlock(&Client->sLock);
            if(LockRet) LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);

            // 【核心联动】: 用户停止观看
            if (P2p->Station) {
                // 1. 立即暂停该通道的 RTSP 拉流，节省带宽并清空缓存
                Stream_SetPause(P2p->Station, Chn, 1);
            }

            LOG_INFO(TAG, "P2P_UnRegeditClientFromVideo OK\n");
            
            int32_t LedNum = 0;
            if (Chn == 0) {
                LedNum = LED_CAM_1;
            }
            else if (Chn == 1) {
                LedNum = LED_CAM_2;
            }
            else if (Chn == 2) {
                LedNum = LED_CAM_3;
            }
            else if (Chn == 3) {
                LedNum = LED_CAM_4;
            }
            if (LedNum && P2p->CamStream[Chn].Ctx && P2p->CamStream[Chn].Ctx->running == 2) {
                System_LedSet(P2p->Station, LedNum, 1);
            }
            break;
        }
        case IOTYPE_USER_IPCAM_AUDIOSTART:
        {
            SMsgAVIoctrlAVStream *p = (SMsgAVIoctrlAVStream *)Data;
            LOG_INFO(TAG, "IOTYPE_USER_IPCAM_AUDIOSTART, ch:%d, avIndex:%d\n\n", p->channel, avIndex);
            //get writer lock
            LockRet = pthread_rwlock_wrlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
            }
            P2P_RegeditClientToAudio(Client, avIndex);
            //release lock
            LockRet = pthread_rwlock_unlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
            }
            LOG_INFO(TAG, "P2P_RegeditClientToAudio OK\n");
            break;
        }
        case IOTYPE_USER_IPCAM_AUDIOSTOP:
        {
            SMsgAVIoctrlAVStream *p = (SMsgAVIoctrlAVStream *)Data;
            LOG_INFO(TAG, "IOTYPE_USER_IPCAM_AUDIOSTOP, ch:%d, avIndex:%d\n\n", p->channel, avIndex);
            //get writer lock
            LockRet = pthread_rwlock_wrlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
            }
            P2P_UnRegeditClientFromAudio(Client);
            //release lock
            LockRet = pthread_rwlock_unlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
            }
            LOG_INFO(TAG, "P2P_UnRegeditClientFromAudio OK\n");
            break;
        }
        case IOTYPE_USER_IPCAM_SPEAKERSTART:
        {
        #if 0
            pthread_t ReceiveAudioThd;
            SMsgAVIoctrlAVStream *p = (SMsgAVIoctrlAVStream *)Data;

            LOG_INFO(TAG,"IOTYPE_USER_IPCAM_SPEAKERSTART, ch:%d, avIndex:%d\n\n", p->channel, avIndex);
            //get writer lock
            LockRet = pthread_rwlock_wrlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
            }
            Client->speakerCh = p->channel;
            Client->bEnableSpeaker = 1;
            //release lock
            LockRet = pthread_rwlock_unlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
            }
            // use which channel decided by client
            ParamBuf = (int32_t *)malloc(2 * sizeof(int32_t));
            ParamBuf[0] = (int32_t)Tutk;
            ParamBuf[1] = SessionId;
            Ret = pthread_create(&ReceiveAudioThd, NULL, &ReceiveAudioThread, (void *)ParamBuf);
            if(Ret < 0)
            {
                LOG_ERROR(TAG,"pthread_create ReceiveAudioThread failed\n");
            }
        #endif
            break;
        }
        case IOTYPE_USER_IPCAM_SPEAKERSTOP:
        {
            LOG_INFO(TAG, "IOTYPE_USER_IPCAM_SPEAKERSTOP\n\n");
            //get writer lock
            LockRet = pthread_rwlock_wrlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
            }
            Client->bEnableSpeaker = 0;
            //release lock
            LockRet = pthread_rwlock_unlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
            }
            break;
        }
        case IOTYPE_USER_IPCAM_LISTEVENT_REQ:
        {
        #if 0
            char *p, path[64];
            char mp4_path[260];
            int32_t duraion;
            int32_t n, i, year, month, day, hour, minute, second;
            struct dirent **namelist;

            SMsgAVIoctrlListEventReq *eventReq = (SMsgAVIoctrlListEventReq *)Data;
            SMsgAVIoctrlListEventResp *eventResp = (SMsgAVIoctrlListEventResp *)malloc(sizeof(SMsgAVIoctrlListEventResp)+sizeof(SAvEvent)*50);
            memset(eventResp, 0, sizeof(SMsgAVIoctrlListEventResp)+sizeof(SAvEvent)*50);

            LOG_INFO(TAG, "IOTYPE_USER_IPCAM_LISTEVENT_REQ\n\n");
            //eventReq->stStartTime.day += 1; /******************@@@@@@@@@@@@@@@@@@@@@@@@**********************/
            LOG_INFO(TAG, "List event req: %04d%02d%02d %02d%02d%02d\n", eventReq->stStartTime.year, eventReq->stStartTime.month, eventReq->stStartTime.day, eventReq->stStartTime.hour, eventReq->stStartTime.minute, eventReq->stStartTime.second);

            if(!Storage_IsReady()){
                LOG_ERROR(TAG, "have not sdcard\n");
                eventResp->endflag = 1;
                avSendIOCtrl(avIndex, IOTYPE_USER_IPCAM_LISTEVENT_RESP, (char *)eventResp, sizeof(SMsgAVIoctrlListEventResp));
                free(eventResp);
                break;
            }

            sprintf(path, "%s/video/%04d%02d%02d", RECORD_PATH, eventReq->stStartTime.year, eventReq->stStartTime.month, eventReq->stStartTime.day);
            n = scandir(path, &namelist, EventFilter, alphasort);
            if (n < 0) {
                LOG_ERROR(TAG, "scandir error: %s\n", path);
                eventResp->endflag = 1;
                avSendIOCtrl(avIndex, IOTYPE_USER_IPCAM_LISTEVENT_RESP, (char *)eventResp, sizeof(SMsgAVIoctrlListEventResp));
                free(eventResp);
                break;
            }

            eventResp->total = n;
            LOG_INFO(TAG, "eventResp: %s %d\n", path, eventResp->total);
            for(i = (n - 1); i >= 0; i--) {
                if (!strncasecmp(namelist[i]->d_name, "person", strlen("person"))) {
                    p = &namelist[i]->d_name[strlen("person_")];
                    eventResp->stEvent[eventResp->count].event = AVIOCTRL_EVENT_HUMANOID_DETECTION;
                }
                else if (!strncasecmp(namelist[i]->d_name, "car", strlen("car"))) {
                    p = &namelist[i]->d_name[strlen("car_")];
                    eventResp->stEvent[eventResp->count].event = AVIOCTRL_EVENT_MOTIONPASS;
                }
                else if (!strncasecmp(namelist[i]->d_name, "pet", strlen("pet"))) {
                    p = &namelist[i]->d_name[strlen("pet_")];
                    eventResp->stEvent[eventResp->count].event = AVIOCTRL_EVENT_PIR;
                }
                else if (!strncasecmp(namelist[i]->d_name, "motion", strlen("motion"))) {
                    p = &namelist[i]->d_name[strlen("motion_")];
                    eventResp->stEvent[eventResp->count].event = AVIOCTRL_EVENT_MOTIONDECT;
                }
                else if (!strncasecmp(namelist[i]->d_name, "remote", strlen("remote"))) {
                    p = &namelist[i]->d_name[strlen("remote_")];
                    eventResp->stEvent[eventResp->count].event = AVIOCTRL_EVENT_RINGBELL;
                }
                else {
                    p = NULL;
                }
                
                eventResp->stEvent[eventResp->count].status = 0;
                if(p && sscanf(p, "%04d%02d%02d-%02d%02d%02d.mp4", &year, &month, &day, &hour, &minute, &second) == 6) {
                    eventResp->stEvent[eventResp->count].stTime.year = year;
                    eventResp->stEvent[eventResp->count].stTime.month = month;
                    eventResp->stEvent[eventResp->count].stTime.day = day;
                    eventResp->stEvent[eventResp->count].stTime.hour = hour;
                    eventResp->stEvent[eventResp->count].stTime.minute = minute;
                    eventResp->stEvent[eventResp->count].stTime.second = second;
                    
                    memset(mp4_path, 0x0, sizeof(mp4_path));
                    sprintf(mp4_path, "%s/%s", path, namelist[i]->d_name);
                    if (CheckFileSize(mp4_path) < 0) {
                        eventResp->total--;
                        free(namelist[i]);
                        unlink(mp4_path);
                        continue;
                    }
                    duraion = mp4_read_duration(mp4_path)/1000;
                    if (duraion <= 5) {
                        eventResp->total--;
                        free(namelist[i]);
                        unlink(mp4_path);
                        continue;
                    }
                    *((unsigned short *)eventResp->stEvent[eventResp->count].reserved) = duraion;
                    eventResp->count++;

                    if (eventResp->count == 50) {
                        if (i == 0) {
                            eventResp->endflag = 1;
                        }
                        LOG_INFO(TAG, "eventResp: %d %d %d\n", eventResp->index, eventResp->count, eventResp->endflag);
                        avSendIOCtrl(avIndex, IOTYPE_USER_IPCAM_LISTEVENT_RESP, (char *)eventResp, sizeof(SMsgAVIoctrlListEventResp) + sizeof(SAvEvent) * (eventResp->count - 1));
                        eventResp->count = 0;
                        eventResp->index++;
                        
                        if((eventResp->index%5) == 4 && P2p->Callback.MsgProcess) {
                            char Resp[256] = "";
                            Ret = P2p->Callback.MsgProcess(Data, IOTYPE_USER_IPCAM_GET_INFO_REQ, Resp, sizeof(Resp));
                            if(Ret < 0) {
                                LOG_ERROR(TAG, "%s failed: avIndex[%d] type[%X]\n", __func__, avIndex, type);
                            }
                        }
                    }
                }
                free(namelist[i]);
            }
            free(namelist);

            eventResp->endflag = 1;
            LOG_INFO(TAG, "eventResp: %d %d %d\n", eventResp->index, eventResp->count, eventResp->endflag);
            avSendIOCtrl(avIndex, IOTYPE_USER_IPCAM_LISTEVENT_RESP, (char *)eventResp, sizeof(SMsgAVIoctrlListEventResp)+sizeof(SAvEvent)*(eventResp->count - 1));

            free(eventResp);
        #endif
            break;
        }
        case IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL:
        {
            SMsgAVIoctrlPlayRecord *p = (SMsgAVIoctrlPlayRecord *)Data;
            SMsgAVIoctrlPlayRecordResp resp;
            
            LOG_INFO(TAG, "IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL cmd[%d]\n\n", p->command);
            if(p->command == AVIOCTRL_RECORD_PLAY_START)
            {
                if (Client->playBackCh > 0 && Client->bPausePlayBack == 0) {
                    int32_t timeout = 0;
                    
                    Client->bStopPlayBack = 1;
                    while(Client->playBackCh > 0 && timeout < 30) {
                        usleep(200000);
                        timeout++;
                    }
                    sleep(1);
                }
                
                memcpy(&Client->playRecord, p, sizeof(SMsgAVIoctrlPlayRecord));
                
                //get writer lock
                LockRet = pthread_rwlock_wrlock(&Client->sLock);
                if(LockRet) {
                    LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
                }
                resp.command = AVIOCTRL_RECORD_PLAY_START;
                LOG_INFO(TAG, "playback now %d\n",Client->playBackCh);
                if( Client->playBackCh < 0)
                {
                    Client->bPausePlayBack = 0;
                    Client->bStopPlayBack = 0;
                    Client->playBackCh = IOTC_Session_Get_Free_Channel(SessionId);
                    //resp.result = Client->playBackCh;
                }
                else {
                    //resp.result = -1;
                    LOG_INFO(TAG, "Continue to playback %d\n",Client->playBackCh);
                }
                resp.result = Client->playBackCh;
                //release lock
                LockRet = pthread_rwlock_unlock(&Client->sLock);
                if(LockRet) {
                    LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
                }
#if 0
                if(Client->bPausePlayBack == 0)
                {
                    pthread_t PlaybackThd;
                    
                    // TODO:
                    LOG_INFO(TAG, "%d start playback\n", resp.result);
                    ParamBuf = (int32_t *)malloc(3 * sizeof(int32_t));
                    ParamBuf[0] = (int32_t)Tutk;
                    ParamBuf[1] = SessionId;
                    ParamBuf[2] = avIndex;
                    Ret = pthread_create(&PlaybackThd, NULL, &PlaybackThread, (void *)ParamBuf);
                    if(Ret < 0)
                    {
                        LOG_INFO(TAG, "pthread_create PlaybackThread failed\n");
                    }
                    Client->bPausePlayBack = 0;
                }
                else {
                    LOG_INFO(TAG, "Playback on SessionId %d is still functioning\n", SessionId);
                    Client->bPausePlayBack = 2;
                }
#endif
                //LOG_INFO(TAG, "Sending res [%d]\n",resp.result);
                if(avSendIOCtrl(avIndex, IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL_RESP, (char *)&resp, sizeof(SMsgAVIoctrlPlayRecordResp)) < 0) {
                    break;
                }
            }
            else if(p->command == AVIOCTRL_RECORD_PLAY_PAUSE)
            {
                resp.command = AVIOCTRL_RECORD_PLAY_PAUSE;
                resp.result = 0;
                if(avSendIOCtrl(avIndex, IOTYPE_USER_IPCAM_RECORD_PLAYCONTROL_RESP, (char *)&resp, sizeof(SMsgAVIoctrlPlayRecordResp)) < 0) {
                    LOG_ERROR(TAG, "SessionId[%d] AVIOCTRL_RECORD_PLAY_PAUSE response failed\n", SessionId);
                    break;
                }
                //get writer lock
                LockRet = pthread_rwlock_wrlock(&Client->sLock);
                if(LockRet) {
                    LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
                }
                Client->bPausePlayBack = 1;
                //release lock
                LockRet = pthread_rwlock_unlock(&Client->sLock);
                if(LockRet) {
                    LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
                }
            }
            else if(p->command == AVIOCTRL_RECORD_PLAY_STOP)
            {
                //get writer lock
                LockRet = pthread_rwlock_wrlock(&Client->sLock);
                if(LockRet) {
                    LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
                }
                Client->bStopPlayBack = 1;
                //release lock
                LockRet = pthread_rwlock_unlock(&Client->sLock);
                if(LockRet) {
                    LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
                }
            }
            break;
        }
        case IOTYPE_USER_IPCAM_SET_RECORD_PROGRESS_REQ:
        {
            SMsgAVIoctrlSetRecordProgessReq *p = (SMsgAVIoctrlSetRecordProgessReq *)Data;
            SMsgAVIoctrlSeRecordProgressResp resp;
            
            memset(&resp, 0, sizeof(resp));
            //get writer lock
            LockRet = pthread_rwlock_wrlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire SessionId %d rwlock failed\n", SessionId);
            }
            if(Client->playBackCh > 0) {
                Client->playBackProgress = p->progressTime | (1<<31);
                resp.result = 0;
            }
            else {
                resp.result = 1;
                avSendIOCtrl(avIndex, IOTYPE_USER_IPCAM_SET_RECORD_PROGRESS_RESP, (char *)&resp, sizeof(SMsgAVIoctrlSeRecordProgressResp));
            }
            //release lock
            LockRet = pthread_rwlock_unlock(&Client->sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Release SessionId %d rwlock failed\n", SessionId, LockRet);
            }
            break;
        }
        default:
        {
            /*char Resp[256] = "";

            if(P2p->Callback.MsgProcess) {
                Ret = P2p->Callback.MsgProcess(Data, type, Resp, sizeof(Resp));
                if(Ret < 0) {
                    LOG_ERROR(TAG, "%s failed: avIndex[%d] type[%X]\n", __func__, avIndex, type);
                    break;
                }
            }

            if (Ret > 0) {
                Ret = avSendIOCtrl(avIndex, type + 1, Resp, Ret);
                if(Ret < 0) {
                    LOG_ERROR(TAG, "avSendIOCtrl failed avIndex[%d]: [%X]\n", avIndex, Ret);
                }
                LOG_INFO(TAG, "%s: avIndex[%d] type[%X] done\n", __func__, avIndex, type);
            }*/
            break;
        }
    }
}

static void *P2P_AVServerStartThread(void *Arg)
{
    int32_t Ret, SessionId;
    int32_t AVServerIndex;
    int32_t avIndex;
    uint32_t CtrlType;
    char CtrlBuf[MAX_SIZE_IOCTRL_BUF];
    P2pHandle *P2p;

    int32_t *ArgTmp = (int32_t *)Arg;
    P2p = (P2pHandle *)ArgTmp[0];
    AVServerIndex = ArgTmp[1];
    SessionId = P2p->SessionId;
    free(ArgTmp);

    {
        char threadName[32];
        sprintf(threadName, "AVServer-%d-%d", SessionId, AVServerIndex);
        prctl(PR_SET_NAME, (unsigned long)threadName);
        pthread_detach(pthread_self());
    }

    {
        AVServStartInConfig avStartInConfig;
        AVServStartOutConfig avStartOutConfig;
        memset(&avStartInConfig, 0, sizeof(AVServStartInConfig));
        avStartInConfig.cb               = sizeof(AVServStartInConfig);
        avStartInConfig.iotc_session_id  = SessionId;
        avStartInConfig.iotc_channel_id  = AVServerIndex == 0 ? 0 : IOTC_Session_Get_Free_Channel(SessionId);
        avStartInConfig.timeout_sec      = 30;
        avStartInConfig.password_auth    = P2P_PasswordAuthCallBack;
        avStartInConfig.server_type      = SERVTYPE_STREAM_SERVER;
        avStartInConfig.resend           = ENABLE_RESEND;
        avStartInConfig.change_password_request = P2P_ChangePasswordCallBack;
        avStartInConfig.ability_request  = P2P_AbilityRequest;
#if ENABLE_TOKEN_AUTH
        // Advance use of authentication.
        // Users can enable or disable these function depends on the actual situation.
        avStartInConfig.token_auth       = ExTokenAuthCallBackFn;
        avStartInConfig.token_delete     = ExTokenDeleteCallBackFn;
        avStartInConfig.token_request    = ExTokenRequestCallBackFn;
        avStartInConfig.identity_array_request = ExGetIdentityArrayCallBackFn;
#endif
        
#if ENABLE_DTLS
        // Enable DTLS encryption of AV data, otherwise use AV_SECURITY_SIMPLE
        avStartInConfig.security_mode = AV_SECURITY_DTLS; 
#else
        avStartInConfig.security_mode = AV_SECURITY_SIMPLE;
#endif

        avStartOutConfig.cb              = sizeof(AVServStartOutConfig);
        
        avIndex = avServStartEx(&avStartInConfig, &avStartOutConfig);
        if(avIndex < 0) {
            LOG_ERROR(TAG, "avServStartEx failed!! AVServerIndex[%d], SessionId[%d] code[%d]\n", AVServerIndex, SessionId, avIndex);
            goto EXIT_AVServerStartThread;
        }
        else {
            LOG_ERROR(TAG, "avServStartEx successful!! AVServerIndex[%d], SessionId[%d] code[%d]\n", AVServerIndex, SessionId, avIndex);
            if (AVServerIndex == 0) {
                P2p->OnlineNum++;
                LOG_INFO(TAG, "Online num = %d\n", P2p->OnlineNum);
            }
        }
    }

    {
        struct st_SInfoEx SeInfo;
        if(IOTC_Session_Check_Ex(SessionId, &SeInfo) == IOTC_ER_NoERROR) {
            char *mode[3] = {"P2P", "RLY", "LAN"};
            // prIBT_INT session information(not a must)
            if(isdigit(SeInfo.RemoteIP[0])) {
                LOG_INFO(TAG, "Client is from[IP:%s, Port:%d] Mode[%s] VPG[%d:%d:%d] VER[%X] NAT[%d] AES[%d]\n", SeInfo.RemoteIP, SeInfo.RemotePort, mode[(int32_t)SeInfo.Mode], SeInfo.VID, SeInfo.PID, SeInfo.GID, SeInfo.IOTCVersion, SeInfo.LocalNatType, SeInfo.isSecure);
            }
        }

        avServSetResendSize(avIndex, 1*1024*1024);
    }

    while(1) {
        Ret = avRecvIOCtrl(avIndex, &CtrlType, (char *)CtrlBuf, MAX_SIZE_IOCTRL_BUF, 3000);
        if(Ret >= 0)
        {
            P2P_HandleIOCtrlCmd(P2p, SessionId, avIndex, CtrlBuf, CtrlType);
        }
        else if(Ret == AV_ER_TIMEOUT)
        {
            /*float UsageRate = avResendBufUsageRate(avIndex);
            LOG_INFO(TAG, "UsageRate[%d--%d]: %f\n", SessionId, avIndex, UsageRate);
            if ((int)(UsageRate*10) > 7) {
                LOG_INFO(TAG, "ResendBuf is going to be overflow\n");
            }*/
        }
        else if(Ret != AV_ER_TIMEOUT)
        {
            LOG_ERROR(TAG, "SessionId[%d], avIndex[%d], avRecvIOCtrl error[%d]\n", SessionId, avIndex, Ret);
            break;
        }
    }


    avServStop(avIndex);
    LOG_INFO(TAG, "AVServerIndex[%d], SessionId[%d], avIndex[%d], AVServerStartThread exit!!\n", AVServerIndex, SessionId, avIndex);

    if(AVServerIndex == 0) {
        P2p->OnlineNum--;
        LOG_INFO(TAG, "Online num = %d\n", P2p->OnlineNum);
    }

EXIT_AVServerStartThread:
    if(AVServerIndex == 0) {
        IOTC_Session_Close(SessionId);
    }
    
    pthread_exit(NULL);
}

static int32_t P2P_SendVideoFrame(CameraStream  *CamStream, char *FrameData, int32_t FrameSize, int32_t IsKeyFrame, int32_t FrameSeq, int64_t timestamp)
{
    int32_t Ret = 0;
    int32_t i;
    FRAMEINFO_t FrameInfo;
    P2pHandle *P2p = CamStream->P2p;

    memset(&FrameInfo, 0, sizeof(FRAMEINFO_t));
    FrameInfo.codec_id = MEDIA_CODEC_VIDEO_H264;
    FrameInfo.reserve2 = FrameSeq;
    FrameInfo.onlineNum = P2p->OnlineNum;
    FrameInfo.timestamp = timestamp;

    if(IsKeyFrame) {
        FrameInfo.flags = IPC_FRAME_FLAG_IFRAME;
    }
    else {
        FrameInfo.flags = IPC_FRAME_FLAG_PBFRAME;
    }

    for(i = 0 ; i < CLIENT_MAX_CNT; i++)
    {
        //get reader lock
        int32_t LockRet = pthread_rwlock_rdlock(&CamStream->Client[i].sLock);
        if(LockRet) {
            LOG_ERROR(TAG, "Acquire Session %d rdlock error, Ret = %d\n", i, LockRet);
        }
        if(CamStream->Client[i].avIndex < 0 || CamStream->Client[i].bEnableVideo == 0)
        {
            //release reader lock
            LockRet = pthread_rwlock_unlock(&CamStream->Client[i].sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire Session %d rdlock error, Ret = %d\n", i, LockRet);
            }
            continue;
        }

#if USAGERATE_CTRL
        if(FrameInfo.flags == IPC_FRAME_FLAG_IFRAME){
            CamStream->Client[i].BufUsageRate = avResendBufUsageRate(CamStream->Client[i].avIndex);
            if (CamStream->Client[i].BufUsageRate - Rate > 0.00001f) {
                Rate = CamStream->Client[i].BufUsageRate;
            }
        }

        if (CamStream->Client[i].BufUsageRate - 0.9f > 0.00001f) {
            Ret = AV_ER_EXCEED_MAX_SIZE;
        }
        else {
            // Send Video Frame to av-idx and know how many time it takes
            Ret = avSendFrameData(CamStream->Client[i].avIndex, FrameData, FrameSize, &FrameInfo, sizeof(FRAMEINFO_t));
        }
#else
        // Send Video Frame to av-idx and know how many time it takes
        Ret = avSendFrameData(CamStream->Client[i].avIndex, FrameData, FrameSize, &FrameInfo, sizeof(FRAMEINFO_t));
#endif

        //release reader lock
        LockRet = pthread_rwlock_unlock(&CamStream->Client[i].sLock);
        if(LockRet) {
            LOG_ERROR(TAG, "Acquire Session %d rdlock error, Ret = %d\n", i, LockRet);
        }

        if(Ret == AV_ER_EXCEED_MAX_SIZE) // means data not write to queue, send too slow, I want to skip it
        {
            LOG_WARN(TAG, "data not write to queue, send too slow, I want to skip it\n");
            usleep(50000);
        }
        else if(Ret == AV_ER_SESSION_CLOSE_BY_REMOTE)
        {
            LOG_WARN(TAG, "thread_VideoFrameData AV_ER_SESSION_CLOSE_BY_REMOTE Session[%d]\n", i);
            P2P_UnRegeditClientFromVideo(&CamStream->Client[i]);
        }
        else if(Ret == AV_ER_REMOTE_TIMEOUT_DISCONNECT)
        {
            LOG_WARN(TAG, "thread_VideoFrameData AV_ER_REMOTE_TIMEOUT_DISCONNECT Session[%d]\n", i);
            P2P_UnRegeditClientFromVideo(&CamStream->Client[i]);
        }
        else if(Ret == IOTC_ER_INVALID_SID)
        {
            LOG_WARN(TAG, "Session cant be used anymore\n");
            P2P_UnRegeditClientFromVideo(&CamStream->Client[i]);
        }
        else if(Ret < 0)
        {
            LOG_INFO(TAG, "avSendFrameData: %d\n", Ret);
        }
    }
    
    return Ret;
}

static int32_t P2P_SendAudioFrame(CameraStream  *CamStream, char *FrameData, int32_t FrameSize, int64_t timestamp)
{
    int32_t LockRet;
    int32_t Ret;
    int32_t i;
    FRAMEINFO_t FrameInfo;
    //P2pHandle *P2p;

    //P2p = container_of(CamStream, P2pHandle, CamStream);
    FrameInfo.codec_id = MEDIA_CODEC_AUDIO_AAC_RAW;//MEDIA_CODEC_AUDIO_AAC_ADTS;//MEDIA_CODEC_AUDIO_PCM;//
    FrameInfo.flags = (AUDIO_SAMPLE_16K << 2) | (AUDIO_DATABITS_16 << 1) | AUDIO_CHANNEL_MONO;
    //FrameInfo.flags = (AUDIO_SAMPLE_8K << 2) | (AUDIO_DATABITS_16 << 1) | AUDIO_CHANNEL_MONO;
    FrameInfo.timestamp = timestamp;

    for(i = 0 ; i < CLIENT_MAX_CNT; i++)
    {
        //get reader lock
        LockRet = pthread_rwlock_rdlock(&CamStream->Client[i].sLock);
        if(LockRet) {
            LOG_ERROR(TAG, "Acquire Session %d rdlock error: %d\n", i, LockRet);
        }
        
        if(CamStream->Client[i].avIndex < 0 || CamStream->Client[i].bEnableAudio == 0)
        {
            //release reader lock
            LockRet = pthread_rwlock_unlock(&CamStream->Client[i].sLock);
            if(LockRet) {
                LOG_ERROR(TAG, "Acquire Session %d rdlock error: %d\n", i, LockRet);
            }
            continue;
        }

#if USAGERATE_CTRL
        if (CamStream->Client[i].BufUsageRate - 0.8f > 0.00001f) {
            Ret = AV_ER_EXCEED_MAX_SIZE;
        }
        else {
            // send audio data to av-idx
            Ret = avSendAudioData(CamStream->Client[i].avIndex, FrameData, FrameSize, &FrameInfo, sizeof(FRAMEINFO_t));
        }
#else
        // send audio data to av-idx
        Ret = avSendAudioData(CamStream->Client[i].avIndex, FrameData, FrameSize, &FrameInfo, sizeof(FRAMEINFO_t));
#endif

        //release reader lock
        LockRet = pthread_rwlock_unlock(&CamStream->Client[i].sLock);
        if(LockRet) {
            LOG_ERROR(TAG, "Acquire Session %d rdlock error: %d\n", i, LockRet);
        }

        //LOG_INFO(TAG, "avIndex[%d] size[%d]\n", Client[i].avIndex, size);
        if(Ret == AV_ER_SESSION_CLOSE_BY_REMOTE)
        {
            LOG_WARN(TAG, "thread_AudioFrameData: AV_ER_SESSION_CLOSE_BY_REMOTE\n");
            P2P_UnRegeditClientFromAudio(&CamStream->Client[i]);
        }
        else if(Ret == AV_ER_REMOTE_TIMEOUT_DISCONNECT)
        {
            LOG_WARN(TAG, "thread_AudioFrameData: AV_ER_REMOTE_TIMEOUT_DISCONNECT\n");
            P2P_UnRegeditClientFromAudio(&CamStream->Client[i]);
        }
        else if(Ret == IOTC_ER_INVALID_SID)
        {
            LOG_WARN(TAG, "Session cant be used anymore\n");
            P2P_UnRegeditClientFromAudio(&CamStream->Client[i]);
        }
        else if(Ret < 0)
        {
            LOG_WARN(TAG, "avSendAudioData error[%d]\n", Ret);
            P2P_UnRegeditClientFromAudio(&CamStream->Client[i]);
        }
    }

    return 0;
}

static void* P2p_SendThread(void *Arg)
{
    int32_t ret, Seq;
    int32_t HasKeyFrame;
    AVPacket pkt;
    CameraStream  *CamStream = (CameraStream *)Arg;
    RtspCtx *ctx;

//#define SAVE_VIDEO_STREAM
#ifdef SAVE_VIDEO_STREAM
    FILE *File;
    File = fopen("/mnt/P2p.h264", "w");
    if (File == NULL) {
        pthread_exit(NULL);
    }
#endif

    prctl(PR_SET_NAME, "P2P_Send");
    pthread_detach(pthread_self());
    
    // Bind context
    ctx = CamStream->Ctx;

    Seq = 0;
    HasKeyFrame = 0;
    av_init_packet(&pkt);
    pkt.data = NULL;
    pkt.size = 0;
    while (1) {
        pthread_testcancel();
        
        // Wait for RTSP stream to be active
        // Use Ctx directly as per new stream.c logic
        if (!ctx || ctx->running != 2) {
             HasKeyFrame = 0;
             usleep(100000); 
             continue;
        }
        
        // Get from P2P Queue (Small Buffer, Low Latency)
        ret = packet_queue_get(&ctx->P2pQueue, &pkt, 1);
        if (ret < 0) {
            LOG_ERROR(TAG, "%s: queue aborted, exit loop.\n", ctx->url);
            break;
        }
        else if (ret == 0) {
            HasKeyFrame = 0;
            continue;
        }
        
        // --- KEYFRAME WAIT LOGIC ---
        // If we haven't seen a keyframe yet, check this packet
        if (!HasKeyFrame) {
            if (pkt.stream_index == ctx->VdIndex) {
                if (pkt.flags & AV_PKT_FLAG_KEY) {
                    LOG_DEBUG(TAG, "%s >>> FOUND FIRST KEYFRAME (size=%d) <<< Starting write.\n", ctx->url, pkt.size);
                    HasKeyFrame = 1;
                }
                else {
                    // DROP until keyframe
                    static int drop_cnt = 0;
                    drop_cnt++;
                    if (drop_cnt % 30 == 0) LOG_DEBUG(TAG, "%s Waiting for keyframe... dropped %d packets\n", ctx->url, drop_cnt);
                    av_packet_unref(&pkt);
                    continue; 
                }
            }
            else {
                // Drop audio until video starts
                av_packet_unref(&pkt);
                continue;
            }
        }

        //Send...
        if (pkt.stream_index == ctx->VdIndex) {
#ifdef SAVE_VIDEO_STREAM
            if (Seq < 300 && File) {
                fwrite((char *)pkt.data, 1, pkt.size, File);
                if (Seq >= 300) {
                    fclose(File);
                    File = NULL;
                }
            }
#endif
            P2P_SendVideoFrame(CamStream, (char *)pkt.data, pkt.size, pkt.flags & AV_PKT_FLAG_KEY, Seq++, pkt.dts);
        }
        else {
            P2P_SendAudioFrame(CamStream, (char *)pkt.data, pkt.size, pkt.dts);
        }
        
        av_packet_unref(&pkt);
    }

    pthread_exit(NULL);
}

static void *P2P_ListenThread(void *Args)
{
    P2pHandle *P2p;

    P2p = (P2pHandle *)Args;
    prctl(PR_SET_NAME, "P2P_Listen");
    pthread_detach(pthread_self());

    while(P2p->State != STATE_EXIT) {
        int32_t SessionId = IOTC_Listen(10000);
        if(SessionId < 0) {
            P2P_PrintErrHandling(SessionId);
            if (SessionId == IOTC_ER_EXIT_LISTEN) {
                break;
            }
            else if (SessionId == IOTC_ER_EXCEED_MAX_SESSION) {
                sleep(5);
            }
            continue;
        }
        P2p->SessionId = SessionId;

        for (int32_t i = 0; i < CAM_MAX_CNT; i++)
        {
            pthread_t AVServerThid;
            int32_t *Param = (int *)malloc(2*sizeof(int32_t));

            Param[0] = (int32_t)P2p;
            Param[1] = i;
            if(pthread_create(&AVServerThid, NULL, &P2P_AVServerStartThread, Param) < 0) {
                LOG_ERROR(TAG, "Create AVServerStartThread failed\n");
            }
            usleep(300*1000);
        }
    }
    P2p->ListenThd = 0;

    pthread_exit(NULL);
}

static void *P2P_LoginThread(void *Args)
{
    int32_t Ret;
    int32_t Timeout;
    P2pHandle *P2p;

    prctl(PR_SET_NAME, "P2P_Login");
    pthread_detach(pthread_self());

    Timeout = 5;//20;
    P2p = (P2pHandle *)Args;
    while(!P2p->IsExit) {
        if (strlen(P2p->Authkey) > 0) {
            DeviceLoginInput DevLogin;
            
            memset(&DevLogin, 0, sizeof(DevLogin));
            DevLogin.cb = sizeof(DeviceLoginInput);
            DevLogin.authentication_type = AUTHENTICATE_BY_KEY;
            memcpy(DevLogin.auth_key, P2p->Authkey, IOTC_AUTH_KEY_LENGTH);
        
            LOG_INFO(TAG, "Login with auth key\n");
            Ret = IOTC_Device_LoginEx(P2p->UID, &DevLogin);
        }
        else {
            LOG_INFO(TAG, "Login without auth key\n");
            // Ignore deprecated warning via pragma
            #pragma GCC diagnostic push
            #pragma GCC diagnostic ignored "-Wdeprecated-declarations"
            Ret = IOTC_Device_Login(P2p->UID, NULL, NULL);
            #pragma GCC diagnostic pop
        }
        
        if (Ret != IOTC_ER_NoERROR) {
            P2P_PrintErrHandling(Ret);
        }
        else {
            break;
        }

        sleep(1);
    }

    if(Timeout <= 0) {
        LOG_WARN(TAG, "Devices login failed\n");
    }
    else
    {
        P2p->State = STATE_LOGIN_DONE;
        IOTC_Get_Login_Info((uint32_t *)&Ret);
        LOG_INFO(TAG, "Devices login successful: %x\n", Ret);
        //Power_SetLedState(P2p->Station,LED_P2PLOGIN_STATE);
    }
    
    LOG_INFO(TAG, "%s exit\n", __func__);

    P2p->LoginThd = 0;
    
    pthread_exit(NULL);
}

static void *P2P_InitThread(void *Args)
{
    int32_t i, j, Ret, Timeout;
    P2pHandle *P2p;

    prctl(PR_SET_NAME, "P2P_Init");
    pthread_detach(pthread_self());
    
    P2p = (P2pHandle *)Args;
    Timeout = 6;

    while (Profile_IsReady(P2p->Station) == 0) {
        if (Timeout-- <= 0) {
            LOG_ERROR(TAG, "Profiles is not ready\n");
            goto TUTK_InitThread_Exit;
        }
        usleep(500*1000);
    }
    
    P2p->MaxClientNum = CLIENT_MAX_CNT;
    for(i = 0; i < CAM_MAX_CNT; i++) {
        ClientInfo *Client = P2p->CamStream[i].Client;
        
        memset(Client, 0, sizeof(P2p->CamStream[i].Client));
        for (j = 0; j < P2p->MaxClientNum; j++) {
            Client[i].avIndex = -1;
            Client[i].playBackCh = -1;
            pthread_rwlock_init(&Client[i].sLock, NULL);
        }
    }
    
    Ret = Profile_Read(P2p->Station, "P2P", "uid", P2p->UID);
    if(Ret != 0) {
        LOG_ERROR(TAG, "Profile_Read UID failed\n");
        goto TUTK_InitThread_Exit;
    }
    
    Ret = Profile_Read(P2p->Station, "P2P", "user", P2p->User);
    if(Ret != 0) {
        LOG_WARN(TAG, "Profile_Read User failed, default users is set\n");
        strcpy(P2p->User, "admin");
    }

    Ret = Profile_Read(P2p->Station, "P2P", "passwd", P2p->Passwd);
    if(Ret != 0) {
        LOG_WARN(TAG, "Profile_Read Passwd failed, default passwd is set\n");
        strcpy(P2p->Passwd, "888888");
    }
    
    Ret = Profile_Read(P2p->Station, "P2P", "authKey", P2p->Authkey);
    if(Ret != 0) {
        LOG_WARN(TAG, "Profile_Read AuthKey failed, default auth key is set\n");
        strcpy(P2p->Authkey, "00000000");
    }
    
    Timeout = 30;
    while (!Network_IsReady(P2p->Station)) {
        if (Timeout-- <= 0) {
            LOG_ERROR(TAG, "Network is not ready\n");
            goto TUTK_InitThread_Exit;
        }
        sleep(1);
    }
    
    LOG_INFO(TAG, "Devices UID : %s\n", P2p->UID);
    LOG_INFO(TAG, "Devices User: %s\n", P2p->User);
    LOG_INFO(TAG, "Devices Passwd: %s\n", P2p->Passwd);
    LOG_INFO(TAG, "Devices AuthKey: %s\n", P2p->Authkey);

    {
        const char LiscenseKey[256] = "AQAAAJmxp86k5V8E2OwWKAtveVfy5vjiibSbaQyf/N475ZtNeAg2ylwLsCs4CiQVULz+8IVYe2HaWVcJmeE7jH6ERWZm36DORik5lwdNugUdnQ2l47dFAZk4uY5G8gQJQqlRZOEkAr4AdNmh20adHKM1fTSJBXBYehrO+vazLHk2Ik4Lahqqo2lGJRr7e5eb8U4u7zxKmHWVxEyAM8z35jZiIuwb";

        Ret = TUTK_SDK_Set_License_Key(LiscenseKey);
        if (Ret != TUTK_ER_NoERROR) {
            LOG_ERROR(TAG, "TUTK_SDK_Set_License_Key failed: %d\n", Ret);
            return NULL;
        }
    }
    
    Ret = IOTC_Initialize2(0);
    if(Ret != IOTC_ER_NoERROR)
    {
        LOG_ERROR(TAG, "IOTC_Initialize2 failed: %d\n", Ret);
        P2P_PrintErrHandling (Ret);
        goto TUTK_InitThread_Exit;
    }

    LOG_INFO(TAG, "IOTCAPI version: %s\n", IOTC_Get_Version_String());
    LOG_INFO(TAG, "AVAPI version: %s\n", avGetAVApiVersionString());

    IOTC_Set_Max_Session_Number(P2p->MaxClientNum);
    IOTC_Get_Login_Info_ByCallBackFn(P2P_LoginCallback);

    // alloc CLIENT_MAX_CNT*3 for every session av data/speaker/play back
    Ret = avInitialize(P2p->MaxClientNum * 3);
    if (Ret != P2p->MaxClientNum * 3) {
        LOG_ERROR(TAG, "initialize failed: %d\n", Ret);
        goto TUTK_InitThread_Exit;
    }

    Ret = pthread_create(&P2p->ListenThd, NULL, P2P_ListenThread, P2p);
    if(Ret < 0)
    {
        LOG_ERROR(TAG,"Create listen thread failed\n");
        goto TUTK_InitThread_Exit;
    }

    Ret = pthread_create(&P2p->LoginThd, NULL, P2P_LoginThread, P2p);
    if(Ret < 0) {
        LOG_ERROR(TAG,"Login thread create fail\n", Ret);
        goto TUTK_InitThread_Exit;
    }

TUTK_InitThread_Exit:
    pthread_exit(NULL);
}

int32_t P2P_Init(StationHandle *Station)
{
    int32_t Ret;
    P2pHandle *P2p;
    pthread_t   InitThd;

    P2p = calloc(1, sizeof(P2pHandle));
    if (P2p == NULL) {
        LOG_ERROR(TAG, "calloc P2pHandle failed\n");
        return -1;
    }

    Station->P2p = P2p;
    P2p->Station = Station;
    Ret = pthread_create(&InitThd, NULL, P2P_InitThread, P2p);
    if(Ret != 0) {
        LOG_ERROR(TAG, "pthread_create p2p init failed\n");
        goto P2P_Init_Error;
    }
    gP2p = P2p;
    
    return 0;

P2P_Init_Error:
    free(P2p);
    Station->P2p = NULL;
    
    return -1;
}

void P2P_Deinit(StationHandle *Station)
{
    P2pHandle *P2p = Station->P2p;

    if (P2p) {
        int32_t i, j;
    
        if (P2p->LoginThd) {
            pthread_cancel(P2p->LoginThd);
        }
        if (P2p->ListenThd) {
            pthread_cancel(P2p->ListenThd);
        }
        P2p->IsExit =1;
        for (i = 0; i < CAM_MAX_CNT; i++) {
            P2P_Stop(Station, i);
        }
        
        P2p->OnlineNum = 0;

        for(i = 0; i < CAM_MAX_CNT; i++) {
            ClientInfo *Client = P2p->CamStream[i].Client;
            
            memset(Client, 0, sizeof(P2p->CamStream[i].Client));
            for (j = 0; j < P2p->MaxClientNum; j++) {
                Client[i].avIndex = -1;
                Client[i].playBackCh = -1;
                pthread_rwlock_destroy(&Client[i].sLock);
            }
        }

        usleep(500000);
        free(P2p);
        Station->P2p = NULL;
    }
}

int32_t P2P_Start(StationHandle *Station, int32_t Index)
{
    int32_t Ret;
    P2pHandle *P2p = Station->P2p;
    //CamManageHandle *CamManage = Station->CameraMag;
    //CameraInfo *CamInfo = &CamManage->Camera[Index];

    if (P2p == NULL) {
        LOG_ERROR(TAG, "P2p is not ready\n");
        return -1;
    }

    P2p->CamStream[Index].P2p = P2p;
    // Bind the RTSP Context from the Stream module
    P2p->CamStream[Index].Ctx = &Station->Stream->Rtsp[Index];
    
    Ret = pthread_create(&P2p->CamStream[Index].SendThread, NULL, P2p_SendThread, &P2p->CamStream[Index]);
    if (Ret < 0) {
        LOG_ERROR(TAG, "pthread_create P2p_SendThread failed\n");
        return -1;
    }
    
    return 0;
}

int32_t P2P_Stop(StationHandle *Station, int32_t Index)
{
    P2pHandle *P2p = Station->P2p;
    
    if (P2p && P2p->CamStream[Index].SendThread) {
        pthread_cancel(P2p->CamStream[Index].SendThread);
        pthread_join(P2p->CamStream[Index].SendThread, NULL); 
        P2p->CamStream[Index].SendThread = 0;
    }
    
    return 0;
}

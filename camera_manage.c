#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

#include "log.h"
#include "system.h"
#include "network.h"
#include "camera_manage.h"
#include "stream.h"

#define TAG 	"CAM_MANAGE"

#define CAM_MANAGE_SRV_PORT		4321
#define FAC_TEST_RTSP_PORT      1234
#define MSG_PAYLOAD_LEN			512

#ifndef HALOW_IF
#define HALOW_IF "wlan0"
#endif

// ==========================================
// Flow Control Configuration
// ==========================================
#define STREAM_SWITCH_INTERVAL  10
// 1 = Auto Switch Demo (自动轮播在线通道); 0 = App Focus Mode
#define ENABLE_AUTO_SWITCH_DEMO 1

// [核心修改] 设为 0 以允许后台播放 (支持4分屏)
// 设为 1 则启用“单通道聚焦”，自动暂停非焦点通道以省流
#define ENABLE_BACKGROUND_PAUSE 0

/* ========================================================================== */
/* External Interface: Set Current Focus Channel                              */
/* Index: -1 for Overview (All On), 0~3 for Single View (Focus On, Others Off)*/
/* ========================================================================== */
void CamManage_SetFocusChannel(StationHandle *Station, int32_t Index)
{
    CamManageHandle *CamManage = Station->CameraMag;
    if (!CamManage) return;

    pthread_mutex_lock(&CamManage->FocusMutex);
    
    if (CamManage->FocusIndex != Index) {
        LOG_INFO(TAG, "User changed focus: Old=%d -> New=%d\n", CamManage->FocusIndex, Index);
        CamManage->FocusIndex = Index;
    }
    
    pthread_mutex_unlock(&CamManage->FocusMutex);
}

/* ========================================================================== */
/* Flow Control Thread                                                        */
/* Manages Stream Pause/Resume based on Focus or Timer                        */
/* ========================================================================== */
static void *CamManage_FlowControlThread(void *Args)
{
    StationHandle *Station = (StationHandle *)Args;

#if ENABLE_AUTO_SWITCH_DEMO
    int32_t AutoEnableIdx = 0; 
#else
    CamManageHandle *CamManage = Station->CameraMag;
    int32_t CurrentFocus = -1;
    int32_t i;
#endif

    prctl(PR_SET_NAME, "FlowCtrl");
    pthread_detach(pthread_self());

#if ENABLE_AUTO_SWITCH_DEMO
    LOG_INFO(TAG, "Flow Control: [AUTO SWITCH MODE] Interval: %ds\n", STREAM_SWITCH_INTERVAL);
#else
    LOG_INFO(TAG, "Flow Control: [APP FOCUS MODE] BG_PAUSE=%d\n", ENABLE_BACKGROUND_PAUSE);
#endif

    // Wait for Stream Module Init
    while (!Network_IsReady(Station) || !Station->Stream) {
        sleep(1);
    }
    sleep(5); // Buffer time for RTSP to stabilize

    while (1) {
#if ENABLE_AUTO_SWITCH_DEMO
        // ============================================================
        // [Logic A] Timer-based Auto Switch (Smart Loop)
        // ============================================================
        
        // 1. 寻找下一个在线的通道 (跳过离线通道)
        int found = 0;
        int startIdx = AutoEnableIdx;
        
        for (int k = 0; k < CAM_MAX_CNT; k++) {
            int idx = (startIdx + k) % CAM_MAX_CNT;
            // 检查该通道是否正在运行
            if (Station->Stream->Rtsp[idx].running && Station->Stream->Rtsp[idx].thread_created) {
                AutoEnableIdx = idx;
                found = 1;
                break;
            }
        }

        // 2. 如果找到了在线通道，执行切换逻辑
        if (found) {
            for (int i = 0; i < CAM_MAX_CNT; i++) {
                if (!Station->Stream->Rtsp[i].running) continue;

                if (i == AutoEnableIdx) {
                    // 恢复目标通道
                    if (Stream_SetPause(Station, i, 0) == 0) {
                         LOG_DEBUG(TAG, "[FlowCtrl] Auto: Resume Ch%d\n", i);
                    }
                } else {
                    // 暂停其他通道
                    if (Stream_SetPause(Station, i, 1) == 0) {
                         LOG_DEBUG(TAG, "[FlowCtrl] Auto: Pause Ch%d\n", i);
                    }
                }
            }
            // 准备下一次轮询的索引 (下一次循环会再次检查它是否在线)
            AutoEnableIdx = (AutoEnableIdx + 1) % CAM_MAX_CNT;
        } else {
            // 没有通道在线，不做任何操作，防止空转过快
            LOG_DEBUG(TAG, "[FlowCtrl] No active streams found\n");
        }

        sleep(STREAM_SWITCH_INTERVAL); 

#else
        // ============================================================
        // [Logic B] App Focus Based Control
        // ============================================================
        pthread_mutex_lock(&CamManage->FocusMutex);
        CurrentFocus = CamManage->FocusIndex;
        pthread_mutex_unlock(&CamManage->FocusMutex);

        for (i = 0; i < CAM_MAX_CNT; i++) {
            RtspCtx *ctx = &Station->Stream->Rtsp[i];
            
            if (!ctx->running || !ctx->thread_created) continue;

            if (CurrentFocus == -1) {
                // Overview Mode: Resume All
                if (ctx->paused) {
                    Stream_SetPause(Station, i, 0); 
                    LOG_INFO(TAG, "[FlowCtrl] Overview: Resume Ch%d\n", i);
                }
            }
            else {
                // Focus Mode Logic
                if (i == CurrentFocus) {
                    if (ctx->paused) {
                        Stream_SetPause(Station, i, 0);
                        Stream_RequestIFrame(Station, i);
                        LOG_INFO(TAG, "[FlowCtrl] Focus: Resume Target Ch%d\n", i);
                    }
                } 
                else {
                    #if ENABLE_BACKGROUND_PAUSE
                    if (!ctx->paused) {
                        Stream_SetPause(Station, i, 1);
                        LOG_INFO(TAG, "[FlowCtrl] Focus: Pause BG Ch%d\n", i);
                    }
                    #endif
                }
            }
        }
        usleep(500 * 1000); 
#endif
    } 
    pthread_exit(NULL);
}

int32_t CamManage_ReadCamInfo(CamManageHandle *CamManage)
{
	int32_t i;
	FILE *File;

	pthread_mutex_lock(&CamManage->CamInfoMutex);
	if (access(CAM_INFO, F_OK) == 0) {
		File = fopen(CAM_INFO, "r");
		if (File) {
			fread(CamManage->Camera, 1, sizeof(CamManage->Camera), File);
			fclose(File);
			for (i = 0; i < CAM_MAX_CNT; i++) {
				CamManage->Camera[i].Sock = -1;
				CamManage->Camera[i].IsAlive = 0;
				CamManage->Camera[i].DisconCnt = 0;
				memset(CamManage->Camera[i].Addr, 0, sizeof(CamManage->Camera[i].Addr));
			}
		}
	}
	else {
		memset(CamManage->Camera, 0, sizeof(CamManage->Camera));
		for (i = 0; i < CAM_MAX_CNT; i++) {
			CamManage->Camera[i].DevIndex = -1;
			CamManage->Camera[i].Sock = -1;
		}
		File = fopen(CAM_INFO, "w");
		if (File) {
			fwrite(CamManage->Camera, 1, sizeof(CamManage->Camera), File);
			fclose(File);
		}
	}
	pthread_mutex_unlock(&CamManage->CamInfoMutex);

	return 0;
}

int32_t CamManage_AddCamInfo(CamManageHandle *CamManage, int32_t Sock, char *Addr, CameraInfo *CamIno)
{
	int32_t i, Flag = 0;
	FILE *File;
	
    // 1. Search for existing device ID (Reconnect logic)
	for (i = 0; i < CAM_MAX_CNT; i++) {
		if (CamManage->Camera[i].DevIndex >= 0 && strcmp(CamManage->Camera[i].DevId, CamIno->DevId) == 0) {
			Flag = 1;
            
            // Force close old socket if it's different (Handle zombie connections)
			if (CamManage->Camera[i].Sock > 0 && CamManage->Camera[i].Sock != Sock) {
                LOG_WARN(TAG, "Camera[%d] %s Kick-off Zombie Socket %d -> New %d\n", 
                         i, Addr, CamManage->Camera[i].Sock, Sock);
				close(CamManage->Camera[i].Sock);
                
                // Stop old stream to allow clean restart
                Stream_Stop(CamManage->Station, i);
			} else {
                LOG_INFO(TAG, "Camera[%d] %s[%s] Re-Connected (Index Kept)\n", i, Addr, CamIno->DevId);
            }
			break;
		}
	}

    // 2. If not found, find a free slot (New Connection)
	if (Flag == 0) {
		if (CamManage->CameraBindCnt >= CAM_MAX_CNT) {
            LOG_ERROR(TAG, "Max Cameras Reached (%d)\n", CamManage->CameraBindCnt);
			return -1;
		}
		for (i = 0; i < CAM_MAX_CNT; i++) {
			if (CamManage->Camera[i].DevIndex < 0) {
				Flag = 2;
				CamManage->CameraBindCnt++;
				LOG_INFO(TAG, "Camera[%d] %s[%s] New Connection\n", i, Addr, CamIno->DevId);
				break;
			}
		}
	}

	if (Flag) {
		pthread_mutex_lock(&CamManage->CamInfoMutex);
		File = fopen(CAM_INFO, "w");

        // Update State
		CamManage->CameraConnectedCnt++;
		CamManage->Camera[i].IsAlive = 1;
		CamManage->Camera[i].DevIndex = i;
		CamManage->Camera[i].Sock = Sock;
        CamManage->Camera[i].DisconCnt = 0; 
		strcpy(CamManage->Camera[i].Addr, Addr);
		strcpy(CamManage->Camera[i].DevId, CamIno->DevId);
		strcpy(CamManage->Camera[i].FwVersion, CamIno->FwVersion);
		
		fwrite(CamManage->Camera, 1, sizeof(CamManage->Camera), File);
		fclose(File);
		pthread_mutex_unlock(&CamManage->CamInfoMutex);
        
        // Log Bitrate for diagnosis
        int rssi=0, evm=0, rate=0;
        Network_GetHalowState(CamManage->Station, &rssi, &evm, &rate);
        if (rate < 500 && rate > 0) {
            LOG_WARN(TAG, "Camera[%d] connected with LOW BITRATE: %d Kbps\n", i, rate);
        }

        return CamManage->Camera[i].DevIndex;
	}

	return -1;
}

int32_t CamManage_DisconnectCamInfo(CamManageHandle *CamManage, int32_t Sock)
{
	int32_t i;
	FILE *File;
	
	for (i = 0; i < CAM_MAX_CNT; i++) {
		if (CamManage->Camera[i].Sock == Sock) {
			LOG_INFO(TAG, "Camera[%s] Disconnected\n", CamManage->Camera[i].Addr);
			if (CamManage->CameraConnectedCnt > 0) {
				CamManage->CameraConnectedCnt--;
			}
			CamManage->Camera[i].Sock = -1;
			CamManage->Camera[i].IsAlive = 0;
			CamManage->Camera[i].DisconCnt = 0;
			
			pthread_mutex_lock(&CamManage->CamInfoMutex);
			File = fopen(CAM_INFO, "w");
			fwrite(CamManage->Camera, 1, sizeof(CamManage->Camera), File);
			fclose(File);
			pthread_mutex_unlock(&CamManage->CamInfoMutex);
			
			return CamManage->Camera[i].DevIndex;
		}
	}
	
	return -1;
}

int32_t CamManage_GetCamIndex(CamManageHandle *CamManage, int32_t Sock)
{
	int32_t i;
	for (i = 0; i < CAM_MAX_CNT; i++) {
		if (Sock == CamManage->Camera[i].Sock) {
			return CamManage->Camera[i].DevIndex;
		}
	}
	return -1;
}

char* CamManage_GetCamAddr(CamManageHandle *CamManage, int32_t Sock)
{
	int i;
	for (i = 0; i < CAM_MAX_CNT; i++) {
		if (Sock == CamManage->Camera[i].Sock) {
			return CamManage->Camera[i].Addr;
		}
	}
	return NULL;
}

int64_t CamManage_GetCamDevId(CamManageHandle *CamManage, int32_t Sock)
{
	int i;
	for (i = 0; i < CAM_MAX_CNT; i++) {
		if (Sock == CamManage->Camera[i].Sock) {
			return *((int64_t *)CamManage->Camera[i].DevId);
		}
	}
	return -1;
}

int32_t CamManage_SetCamKeepAlive(CamManageHandle *CamManage, int32_t Sock)
{
	int32_t i;
	for (i = 0; i < CAM_MAX_CNT; i++) {
		if (Sock == CamManage->Camera[i].Sock) {
			CamManage->Camera[i].DisconCnt = 0;
		}
	}
	return -1;
}

int32_t CamManage_SetCamIndicator(CamManageHandle *CamManage, int32_t Index)
{
	int32_t LedNum = 0;
	switch (Index) {
		case 0: LedNum = LED_CAM_1; break;
		case 1: LedNum = LED_CAM_2; break;
		case 2: LedNum = LED_CAM_3; break;
		case 3: LedNum = LED_CAM_4; break;
		default: break;
	}
	if (LedNum) Hardware_GpioSet(LedNum, 1);
	return !LedNum;
}

static void* CamManage_ConnThread(void *Args)
{
	int32_t Ret;
    int32_t Opt = 1;
	struct ifreq Ifr;
    struct sockaddr_in ClientAddr;
    struct sockaddr_in ServAddr;
    socklen_t SockLen = sizeof(ClientAddr);
	CamManageHandle *CamManage = (CamManageHandle *)Args;
	fd_set Rdfds, RdSet;
	int32_t MaxFd;
	MsgPacket *Packet = malloc(sizeof(MsgPacket) + MSG_PAYLOAD_LEN);
	
	prctl(PR_SET_NAME, "CamConn");
	pthread_detach(pthread_self());

    LOG_INFO(TAG, "ConnThread waiting for Network...\n");
	while (!Network_IsReady(CamManage->Station)) sleep(1);
    
    LOG_INFO(TAG, "ConnThread waiting for Halow...\n");
	while (!Network_IsHalowReady(CamManage->Station)) sleep(1);
	
OPEN_SOCKET:
    if ((CamManage->ListenSock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        LOG_ERROR(TAG, "Socket creation error\n");
		sleep(2);
        goto OPEN_SOCKET;
    }
    
    if (setsockopt(CamManage->ListenSock, SOL_SOCKET, SO_REUSEADDR, &Opt, sizeof(int32_t)) < 0) {
        LOG_ERROR(TAG, "Socket setsockopt error: %d\n", errno);
		close(CamManage->ListenSock);
		sleep(2);
        goto OPEN_SOCKET;
    }

    if (setsockopt(CamManage->ListenSock, IPPROTO_TCP, TCP_NODELAY, &Opt, sizeof(int32_t)) < 0) {
        LOG_WARN(TAG, "Set TCP_NODELAY failed\n");
    }

    strncpy(Ifr.ifr_name, HALOW_IF, IF_NAMESIZE);
    Ifr.ifr_name[IFNAMSIZ-1] = '\0';

    if (ioctl(CamManage->ListenSock, SIOCGIFADDR, &Ifr) < 0) {
        LOG_ERROR(TAG, "ioctl error on %s: %d\n", HALOW_IF, errno);
		close(CamManage->ListenSock);
		sleep(2);
        goto OPEN_SOCKET;
    }

    memcpy(&ServAddr, &Ifr.ifr_addr, sizeof(ServAddr));
    ServAddr.sin_family = AF_INET;
    ServAddr.sin_port = htons(CAM_MANAGE_SRV_PORT);
	LOG_INFO(TAG, "%s bound to %s:%d\n", HALOW_IF, inet_ntoa(ServAddr.sin_addr), CAM_MANAGE_SRV_PORT);

    if (bind(CamManage->ListenSock, (struct sockaddr *)&ServAddr, sizeof(ServAddr)) < 0) {
        LOG_ERROR(TAG, "Socket bind failed: %d\n", errno);
		close(CamManage->ListenSock);
		sleep(2);
        goto OPEN_SOCKET;
    }
    
    if (listen(CamManage->ListenSock, CAM_MAX_CNT) < 0) {
        LOG_ERROR(TAG, "Socket listen failed\n");
		close(CamManage->ListenSock);
		sleep(2);
        goto OPEN_SOCKET;
    }

    LOG_INFO(TAG, "TCP Server Listening on Port %d\n", CAM_MANAGE_SRV_PORT);

	FD_ZERO(&Rdfds);
	FD_SET(CamManage->ListenSock, &Rdfds);
	MaxFd = CamManage->ListenSock;

    while (1) {
		int32_t i;
		int32_t ConnSock, SelectRet;
		struct timeval TimeVal;

		TimeVal.tv_sec = 3;
		TimeVal.tv_usec = 0;
		RdSet = Rdfds;
		SelectRet = select(MaxFd + 1, &RdSet, NULL, NULL, &TimeVal);
		
		if (SelectRet == 0) {
            // Heartbeat/Timeout check logic
			for (i = CamManage->ListenSock + 1;  i <= MaxFd; i ++) {
				for (int32_t j = 0; j < CAM_MAX_CNT; j++) {
					if (i == CamManage->Camera[j].Sock) {
						CamManage->Camera[j].DisconCnt++;
						if (CamManage->Camera[j].IsAlive == 1 && CamManage->Camera[j].DisconCnt > 3) {
							Ret = CamManage_DisconnectCamInfo(CamManage, i);
							Stream_Stop(CamManage->Station, Ret);
							FD_CLR(i, &Rdfds);
							FD_CLR(i, &RdSet);
							close(i);
						}
					}
				}
			}
			continue;
		}
		
		if (FD_ISSET(CamManage->ListenSock, &RdSet)) {
			if ((ConnSock = accept(CamManage->ListenSock, (struct sockaddr *)&ClientAddr, &SockLen)) < 0) {
				LOG_ERROR(TAG, "Accept failed\n");
				continue;
			}
            
            int rssi=0, evm=0, rate=0;
            Network_GetHalowState(CamManage->Station, &rssi, &evm, &rate);
			LOG_INFO(TAG, "New Connection from %s (RSSI:%d EVM:%d Rate:%dKbps)\n", 
                     inet_ntoa(ClientAddr.sin_addr), rssi, evm, rate);
            
            setsockopt(ConnSock, IPPROTO_TCP, TCP_NODELAY, &Opt, sizeof(int32_t));
			
			FD_SET(ConnSock, &Rdfds);
			if (ConnSock > MaxFd) MaxFd = ConnSock;
			if (--SelectRet == 0) continue;
		}

		for (i = CamManage->ListenSock + 1;  i <= MaxFd; i ++) {
			if (FD_ISSET(i, &RdSet)) {
				Ret = read(i, Packet, sizeof(MsgPacket) + MSG_PAYLOAD_LEN);
				if (Ret <= 0) {
					Ret = CamManage_DisconnectCamInfo(CamManage, i);
					if (Ret >= 0) {
						Stream_Stop(CamManage->Station, Ret);
						FD_CLR(i, &Rdfds);
						close(i);
					}
				}
				else if (MSG_IS_CAM2STA(Packet->Type)) {
					switch (Packet->Type & MSG_ID_MASK) {
						case MSG_CAM_INFO:
						{
                            LOG_INFO(TAG, "Recv MSG_CAM_INFO from sock %d\n", i);
							CameraInfo CamInfo = {0};
							Ret = CamManage_AddCamInfo(CamManage, i, inet_ntoa(ClientAddr.sin_addr), (CameraInfo *)Packet->Data);
							Packet->Type = MSG_TYPE_STA2CAM(MSG_CAM_INFO);
							Packet->Len = sizeof(CameraInfo);
							Packet->CheckSum = 0;
							CamInfo.DevIndex = Ret;
							memcpy(Packet->Data, &CamInfo, Packet->Len);
							send(i, Packet, sizeof(MsgPacket), 0);
							if (Ret >= 0) {
								CamManage_SetCamIndicator(CamManage, Ret);
							} else {
								close(i);
                                FD_CLR(i, &Rdfds);
							}
							break;
						}
						case MSG_STREAM_READY:
						{
							int32_t Index = CamManage_GetCamIndex(CamManage, i);
                            LOG_INFO(TAG, "Recv MSG_STREAM_READY from Ch%d\n", Index);
							if (Index >= 0) {
								Packet->Type = MSG_TYPE_STA2CAM(MSG_REQ_STREAM);
								Packet->Len = 0;
								Packet->CheckSum = 0;
								send(i, Packet, sizeof(MsgPacket), 0);
								usleep(500*1000);
								Stream_Start(CamManage->Station, Index);
							}
							break;
						}
						case MSG_SYNC_DATE_TIME:
						{
							time_t TimeStamp;
							struct tm *DateTime;
							Packet->Type = MSG_TYPE_STA2CAM(MSG_SYNC_DATE_TIME);
							Packet->Len = sizeof(struct tm);
							Packet->CheckSum = 0;
							time(&TimeStamp);
							DateTime = localtime(&TimeStamp);
							memcpy(Packet->Data, DateTime, Packet->Len);
							send(i, Packet, sizeof(MsgPacket) + Packet->Len, 0);
							break;
						}
						case MSG_KEEP_ALIVE:
							CamManage_SetCamKeepAlive(CamManage, i);
							Packet->Type = MSG_TYPE_STA2CAM(MSG_KEEP_ALIVE);
							Packet->Len = 0;
							Packet->CheckSum = 0;
							send(i, Packet, sizeof(MsgPacket) + Packet->Len, 0);
							break;
						default: break;
					}
				}
				if (--SelectRet == 0) break;
			}
		}
    }

	free(Packet);
	if (CamManage->ListenSock > 0) close(CamManage->ListenSock);
	pthread_exit(NULL);
}

int32_t CamManage_Init(StationHandle *Station)
{
	int32_t Ret;
	CamManageHandle *CamManage;

	CamManage = malloc(sizeof(CamManageHandle));
	if (CamManage == NULL) return -1;
	
	memset(CamManage, 0, sizeof(CamManageHandle));
	Station->CameraMag = CamManage;
	CamManage->Station = Station;
	
	Ret = pthread_mutex_init(&CamManage->CamInfoMutex, NULL);
    pthread_mutex_init(&CamManage->FocusMutex, NULL); 

	if (Ret != 0) goto CamManage_Init_Error;

	CamManage_ReadCamInfo(CamManage);
	Ret = pthread_create(&CamManage->ConnThread, NULL, CamManage_ConnThread, CamManage);
	if (Ret < 0) goto CamManage_Init_Error;

    // Start Flow Control
    Ret = pthread_create(&CamManage->FlowThread, NULL, CamManage_FlowControlThread, Station);
	
    return 0;

CamManage_Init_Error:
	if (CamManage->ConnThread > 0) pthread_cancel(CamManage->ConnThread);
    if (CamManage->FlowThread > 0) pthread_cancel(CamManage->FlowThread);
	free(CamManage);
	Station->CameraMag = NULL;
	return -1;
}

void CamManage_Deinit(StationHandle *Station)
{
	CamManageHandle *CamManage = Station->CameraMag;
	if (CamManage) {
		if (CamManage->ListenSock) {
			for (int i = 0; i < CamManage->CameraConnectedCnt; i++) {
				if (CamManage->Camera[i].Sock > 0) {
					close(CamManage->Camera[i].Sock);
					CamManage->Camera[i].Sock = 0;
				}
			}
			close(CamManage->ListenSock);
		}

        if (CamManage->FlowThread) {
            pthread_cancel(CamManage->FlowThread);
            pthread_join(CamManage->FlowThread, NULL);
        }
		if (CamManage->ConnThread) {
            pthread_cancel(CamManage->ConnThread);
            pthread_join(CamManage->ConnThread, NULL);
        }

		pthread_mutex_destroy(&CamManage->CamInfoMutex);
        pthread_mutex_destroy(&CamManage->FocusMutex);
		free(CamManage);
		Station->CameraMag = NULL;
	}
}

int32_t CamManage_Send(StationHandle *Station, int32_t Index, int32_t Type, char *Data, int32_t Len)
{
	int32_t Ret;
	char Buf[256] = {0};
	MsgPacket *Packet = (MsgPacket *)Buf;
	CamManageHandle *CamManage = Station->CameraMag;
	
	Packet->Type = MSG_TYPE_STA2CAM(Type);
	Packet->Len = Len;
	Packet->CheckSum = 0;
    if (Len > 0 && Data) memcpy(Packet->Data, Data, Len);
    
	if (CamManage->Camera[Index].Sock > 0) {
		Ret = send(CamManage->Camera[Index].Sock, Packet, sizeof(MsgPacket) + Len, 0);
	} else {
		return -1;
	}
	return Ret;
}

int64_t CamManage_GetCamDevIdByIndex(StationHandle *Station, int32_t Index)
{
	CamManageHandle *CamManage = Station->CameraMag;
	return *((int64_t *)CamManage->Camera[Index].DevId);
}

int32_t CamManage_RemoveCamInfo(StationHandle *Station)
{
	CamManageHandle *CamManage = Station->CameraMag;
	pthread_mutex_lock(&CamManage->CamInfoMutex);
	unlink(CAM_INFO);
	pthread_mutex_unlock(&CamManage->CamInfoMutex);
	return 0;
}

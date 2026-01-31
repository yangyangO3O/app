#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>
#include <linux/input.h>

#include "system_call.h"
#include "system.h"
#include "storage.h"
#include "profile.h"
#include "network.h"
#include "camera_manage.h"
#include "stream.h"
#include "record.h"
#include "p2p.h"
#include "device_manage.h"
#include "cJSON.h"


#define TAG "SYSTEM"
#define DEV_KEY_EVENT			"/dev/input/event0"

#define MTD_UBOOT				"/dev/mtd0"
#define MTD_KERNEL				"/dev/mtd1"
#define MTD_ROOTFS				"/dev/mtd2"
#define MTD_SYSTEM				"/dev/mtd3"
#define MTD_ALL					"/dev/mtd4"

#define FW_UBOOT		STORAGE_CARD "/u-boot.bin"
#define FW_KERNEL		STORAGE_CARD "/uImage"
#define FW_ROOTFS		STORAGE_CARD "/rootfs.bin"
#define FW_SYSTEM		STORAGE_CARD "/system.bin"
#define FW_ALL			STORAGE_CARD "/PRJ007NQ.bin"

#define FW_UPGRADE		"/config/.upgrade"

#define SYSTEM_SCRIPT 	STORAGE_CARD "/script.sh"
#define SYSTEM_CONFIG	STORAGE_CARD "/config.json"

#define NTP_SRV			"ntp.aliyun.com"

static void* System_CommonThread(void *Args)
{
	int32_t Mounted;
	SystemHandle *System;
	
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());

	Mounted = 0;
	System = (SystemHandle *)Args;
	
	while (1) {
		char Command[64];
		sleep(1);
		/*if (System->DateTimeSyncState != 2 && Network_IsReady(System->Station)) {
			struct tm *Tm;
			struct timeval TimeVal;
			
			Ret = gettimeofday(&TimeVal, NULL);
			if(Ret < 0) {
				LOG_DEBUG(TAG, "gettimeofday failed: {%ld, %ld}\n", TimeVal.tv_sec, TimeVal.tv_usec);
				continue;
			}
			
			Tm = localtime(&TimeVal.tv_sec);
			if (!System->DateTimeSyncState) {
				sprintf(Command, "ntpdate %s &", NTP_SRV);
				system_call(Command, 1000);
				System->DateTimeSyncState = 1;
			}
			else if (Tm->tm_year + 1900 >= 2026) {
				System->DateTimeSyncState = 2;
			}
		}*/
		
		if (Storage_IsReady(System->Station)) {
			if (Mounted == 0) {
				Mounted = 1;
				System_ImportConfig(System->Station);
				if (access(SYSTEM_SCRIPT, F_OK) == 0) {
					LOG_DEBUG(TAG, "Run the script\n");

					sprintf(Command, "chmod 0775 %s", SYSTEM_SCRIPT);
					system_call(Command, 1000);
					usleep(1000*200);
					system_call(SYSTEM_SCRIPT, 0);
					sleep(1);
					//unlink(SYSTEM_SCRIPT);
				}
			}
		}
		else {
			Mounted = 0;
		}
	}
	
	pthread_exit(NULL);
}

static void* System_KeyProcessThread(void *Args)
{
	SystemHandle *System;
	struct timespec TimeSpec;

	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());

	System = (SystemHandle *)Args;
	sem_init(&System->KeySem, 0, 0);
	
	if (clock_gettime(CLOCK_REALTIME, &TimeSpec) == -1) {
		LOG_ERROR(TAG, "clock_gettime error\n");
		sem_destroy(&System->KeySem);
		pthread_exit(NULL);
	}
	
	TimeSpec.tv_sec += 3;
	if (sem_timedwait(&System->KeySem, &TimeSpec) == -1) {
		if (errno == ETIMEDOUT) {
			CamManage_RemoveCamInfo(System->Station);
			System_Reboot(System->Station);
		}
		else {
			LOG_ERROR(TAG, "sem_timedwait error\n");
		}
	}

	sem_destroy(&System->KeySem);
	pthread_exit(NULL);
}

static void* System_KeyListenThread(void *Args)
{
	int32_t KeyEvtFd, Ret;
	uint32_t PressTime, LastReleaseTime, PressCount;
	SystemHandle *System;

	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());
	
	System = (SystemHandle *)Args;
	KeyEvtFd = open(DEV_KEY_EVENT, O_RDONLY);
	if (KeyEvtFd < 0) {
		LOG_ERROR(TAG, "open %s failed\n", DEV_KEY_EVENT);
		pthread_exit(NULL);
	}

	while(1) {
		struct input_event KeyEvent;
		
		Ret = read(KeyEvtFd, &KeyEvent, sizeof(struct input_event));
		if (Ret <= 0) {
			LOG_ERROR(TAG, "read key event error: %s\n", strerror(errno));
			Ret = -1;
			goto System_KeyListenThread_Exit;
		}

		if (KeyEvent.code == 0) {
			continue;
		}
		//LOG_INFO(TAG, "Key:%d type:%d event:%d\n", KeyEvent.code & 0xff, KeyEvent.type, KeyEvent.value);
		if (KeyEvent.code == KEY_HOME) {
			if (KeyEvent.value) {
				LOG_INFO(TAG, "RESET is pressed\n");
				PressTime = KeyEvent.time.tv_sec * 1000 + KeyEvent.time.tv_usec/1000;//IMP_System_GetTimeStamp()/1000;
				
				Ret = pthread_create(&System->KeyProcessThread, NULL, System_KeyProcessThread, System);
				if(Ret < 0) {
					LOG_ERROR(TAG, "pthread_create System_KeyProcessThread failed\n");
					continue;
				}
			}
			else {
				uint32_t ReleaseTime = KeyEvent.time.tv_sec * 1000 + KeyEvent.time.tv_usec/1000;//IMP_System_GetTimeStamp()/1000;
				
				LOG_INFO(TAG, "RESET is released\n");
				sem_post(&System->KeySem);
				if(ReleaseTime - LastReleaseTime < 500 && ReleaseTime - PressTime < 2000) {
					PressCount++;
					LOG_INFO(TAG, "RESET PressCount=%d\n",PressCount);
				}
				else {
					PressCount = 0;
				}

				LastReleaseTime = ReleaseTime;

				if (PressCount == 2) {
				}
			}
		}
	}
	
System_KeyListenThread_Exit:
	close(KeyEvtFd);

	pthread_exit(NULL);
}
#if 0
static void* System_FwUpgradeThread(void *Args)
{
	SystemHandle *System = (SystemHandle *)Args;
	char Buf[128];
	int32_t Ret;
	
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());
	
	while (1) {
		sleep(1);

		if (!Storage_IsReady(System->Station)) {
			continue;
		}

		if (access(FW_UPGRADE, F_OK) < 0) {
			if (access(FW_UBOOT, F_OK) == 0 || access(FW_KERNEL, F_OK) == 0 || access(FW_ROOTFS, F_OK) == 0 || access(FW_SYSTEM, F_OK) == 0) {
				//sprintf(Buf, "touch %s", FW_UPGRADE);
				//Ret = system_call(Buf, 1000);
				//sleep(2);
				//System_Reboot(System->Station);
				break;
			}
			else {
				continue;
			}
		}
		
		System_LedBlink(System->Station, LED_STA_RED, 300);
		if (access(FW_UBOOT, F_OK) == 0) {
			LOG_DEBUG(TAG, "Upgrade %s\n", MTD_UBOOT);
			System->UpgradeState = 2;
			sprintf(Buf, "flashcp -v %s %s", FW_UBOOT, MTD_UBOOT);
			Ret = system_call(Buf, 1000*30);
			if (Ret != 0) {
				break;
			}
			unlink(FW_UBOOT);
		}

		if (access(FW_KERNEL, F_OK) == 0) {
			LOG_DEBUG(TAG, "Upgrade %s\n", MTD_KERNEL);
			System->UpgradeState = 3;
			sprintf(Buf, "flashcp -v %s %s", FW_KERNEL, MTD_KERNEL);
			Ret = system_call(Buf, 1000*30);
			if (Ret != 0) {
				break;
			}
			unlink(FW_KERNEL);
		}
		
		if (access(FW_ROOTFS, F_OK) == 0) {
			LOG_DEBUG(TAG, "Upgrade %s\n", MTD_ROOTFS);
			System->UpgradeState = 4;
			sprintf(Buf, "flashcp -v %s %s", FW_ROOTFS, MTD_ROOTFS);
			Ret = system_call(Buf, 1000*30);
			if (Ret != 0) {
				break;
			}
			unlink(FW_ROOTFS);
		}
		
		if (access(FW_SYSTEM, F_OK) == 0) {
			LOG_DEBUG(TAG, "Upgrade %s\n", MTD_SYSTEM);
			System->UpgradeState = 5;
			//sprintf(Buf, "flash_eraseall -j %s", MTD_SYSTEM);
			//system_call(Buf, 1000*60);
			
			sprintf(Buf, "flashcp -v %s %s", FW_SYSTEM, MTD_SYSTEM);
			Ret = system_call(Buf, 1000*60);
			if (Ret != 0) {
				break;
			}
			unlink(FW_SYSTEM);
		}
		
		if (access(FW_ALL, F_OK) == 0) {
			LOG_DEBUG(TAG, "Upgrade %s\n", MTD_ALL);
			System->UpgradeState = 6;
			sprintf(Buf, "flashcp -v %s %s", FW_ALL, MTD_ALL);
			Ret = system_call(Buf, 1000*120);
			if (Ret != 0) {
				break;
			}
			unlink(FW_ALL);
		}
		
		System_LedSet(System->Station, LED_STA_RED, 1);
		if (System->UpgradeState == 1) {
			System->UpgradeState = 0;
			unlink(FW_UPGRADE);
		}
		else if (Ret == 0) {
			unlink(FW_UPGRADE);
			System_Reboot(System->Station);
			break;
		}
	}
	
	while (Ret != 0) {
		sleep(3);
	}
	
	System->FwUpgradeThread = 0;
	
	pthread_exit(NULL);
}
#else
int32_t System_FwUpgrade(StationHandle *Station)
{
	int32_t Ret;
	char Buf[128];
	SystemHandle *System = Station->System;
	
	DevManage_Deinit(Station);
	P2P_Deinit(Station);
	Record_Deinit(Station);
	Stream_Deinit(Station);
	CamManage_Deinit(Station);
	Network_Deinit(Station);
	
	System_LedBlink(Station, LED_STA_RED, 300);
	if (access(FW_UBOOT, F_OK) == 0) {
		LOG_DEBUG(TAG, "Upgrade %s\n", MTD_UBOOT);
		System->UpgradeState = 2;
		sprintf(Buf, "flashcp -v %s %s", FW_UBOOT, MTD_UBOOT);
		Ret = system_call(Buf, 1000*30);
		if (Ret != 0) {
			goto System_FwUpgrade_Exit;
		}
		unlink(FW_UBOOT);
	}
	
	if (access(FW_KERNEL, F_OK) == 0) {
		LOG_DEBUG(TAG, "Upgrade %s\n", MTD_KERNEL);
		System->UpgradeState = 3;
		sprintf(Buf, "flashcp -v %s %s", FW_KERNEL, MTD_KERNEL);
		Ret = system_call(Buf, 1000*30);
		if (Ret != 0) {
			goto System_FwUpgrade_Exit;
		}
		unlink(FW_KERNEL);
	}
	
	if (access(FW_ROOTFS, F_OK) == 0) {
		LOG_DEBUG(TAG, "Upgrade %s\n", MTD_ROOTFS);
		System->UpgradeState = 4;
		sprintf(Buf, "flashcp -v %s %s", FW_ROOTFS, MTD_ROOTFS);
		Ret = system_call(Buf, 1000*30);
		if (Ret != 0) {
			goto System_FwUpgrade_Exit;
		}
		unlink(FW_ROOTFS);
	}
	
	if (access(FW_SYSTEM, F_OK) == 0) {
		LOG_DEBUG(TAG, "Upgrade %s\n", MTD_SYSTEM);
		System->UpgradeState = 5;
		//sprintf(Buf, "flash_eraseall -j %s", MTD_SYSTEM);
		//system_call(Buf, 1000*60);
		
		sprintf(Buf, "flashcp -v %s %s", FW_SYSTEM, MTD_SYSTEM);
		Ret = system_call(Buf, 1000*60);
		if (Ret != 0) {
			goto System_FwUpgrade_Exit;
		}
		unlink(FW_SYSTEM);
	}
	
	if (access(FW_ALL, F_OK) == 0) {
		LOG_DEBUG(TAG, "Upgrade %s\n", MTD_ALL);
		System->UpgradeState = 6;
		sprintf(Buf, "flashcp -v %s %s", FW_ALL, MTD_ALL);
		Ret = system_call(Buf, 1000*120);
		if (Ret != 0) {
			goto System_FwUpgrade_Exit;
		}
		unlink(FW_ALL);
	}

System_FwUpgrade_Exit:

	System_LedSet(System->Station, LED_STA_RED, 1);

	return Ret;
}

static void* System_FwUpgradeThread(void *Args)
{
	SystemHandle *System = (SystemHandle *)Args;
	
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());
	
	while (1) {
		sleep(3);

		if (!Storage_IsReady(System->Station)) {
			continue;
		}

		if (access(FW_UPGRADE, F_OK) < 0) {
			if (access(FW_UBOOT, F_OK) == 0 || access(FW_KERNEL, F_OK) == 0 || access(FW_ROOTFS, F_OK) == 0 || access(FW_SYSTEM, F_OK) == 0) {
				System->UpgradeState = 1;
				break;
			}
		}
	}
	
	if (System->UpgradeState == 1) {
		System_FwUpgrade(System->Station);
		System_Reboot(System->Station);
	}
	
	System->FwUpgradeThread = 0;
	
	pthread_exit(NULL);
}

#endif

static int32_t System_LedInit(SystemHandle *System)
{
	System->Led[0].Pin = LED_CAM_1;
	System->Led[1].Pin = LED_CAM_2;
	System->Led[2].Pin = LED_CAM_3;
	System->Led[3].Pin = LED_CAM_4;
	System->Led[4].Pin = LED_STA_RED;
	System->Led[5].Pin = LED_STA_BLUE;
	
	return 0;
}

static int32_t System_LedToggle(void *UserData)
{
	LedObj *Led = (LedObj *)UserData;

	if (Led == NULL) {
		LOG_ERROR(TAG, "System_LedToggle failed\n");
		return -1;
	}
	Led->State = !Led->State;
	
	return GPIO_Set(Led->Pin, Led->State);
}

int32_t System_Init(StationHandle *Station)
{
	SystemHandle *System = NULL;
	int32_t Ret = 0;

	System = calloc(1, sizeof(SystemHandle));
	if (System == NULL) {
		LOG_ERROR(TAG, "calloc SystemHandle failed\n");
		return -1;
	}
	
	Station->System = System;
	System->Station = Station;
	System_LedInit(System);
	
	Ret = pthread_mutex_init(&System->Mutex, NULL);
	if (Ret != 0) {
		LOG_ERROR(TAG, "pthread_mutex_init failed\n");
		goto System_Init_Error;
	}

	Ret = pthread_create(&System->CommonThread, NULL, System_CommonThread, System);
	if (Ret < 0) {
		LOG_ERROR(TAG, "pthread_create System_CommonThread failed\n");
		goto System_Init_Error;
	}

	Ret = pthread_create(&System->KeyListenThread, NULL, System_KeyListenThread, System);
	if (Ret < 0) {
		LOG_ERROR(TAG, "pthread_create Stream_KeyListenThread failed\n");
		goto System_Init_Error;
	}
	
	Ret = pthread_create(&System->FwUpgradeThread, NULL, System_FwUpgradeThread, System);
	if (Ret < 0) {
		LOG_ERROR(TAG, "pthread_create System_FwUpgradeThread failed\n");
		goto System_Init_Error;
	}
	
	/*if (access(FW_UPGRADE, F_OK) == 0) {
		System->UpgradeState = 1;
		do {
			sleep(1);
		} while (System->UpgradeState == 1);
	}*/

	return 0;

System_Init_Error:
	if (System->CommonThread > 0) {
		pthread_cancel(System->CommonThread);
	}
	if (System->KeyListenThread > 0) {
		pthread_cancel(System->KeyListenThread);
	}
	if (System->FwUpgradeThread > 0) {
		pthread_cancel(System->FwUpgradeThread);
	}
	free(System);
	Station->System = NULL;
	
	return -1;
}
void System_Deinit(StationHandle *Station)
{
	SystemHandle *System = Station->System;
	
	if (System) {
		if (System->CommonThread) {
			pthread_cancel(System->CommonThread);
			pthread_join(System->CommonThread, NULL);
		}
		if (System->KeyListenThread) {
			pthread_cancel(System->KeyListenThread);
			pthread_join(System->KeyListenThread, NULL);
		}
		if (System->FwUpgradeThread) {
			pthread_cancel(System->FwUpgradeThread);
			pthread_join(System->FwUpgradeThread, NULL);
		}
		pthread_mutex_destroy(&System->Mutex);
		free(System);
		Station->System = NULL;
	}
}

void System_Reboot(StationHandle *Station)
{
	sync();
	sem_post(&Station->ExitSem);
}

int64_t System_GetTimeStamp(StationHandle *Station, char *DateTime, int32_t Fmt)
{
	int32_t Ret;
	struct tm *Tm;
	struct timeval TimeVal;
	
	Ret = gettimeofday(&TimeVal, NULL);
	if(Ret < 0) {
		LOG_ERROR(TAG, "gettimeofday failed: {%ld, %ld}\n", TimeVal.tv_sec, TimeVal.tv_usec);
		return -1;
	}
	
	if (DateTime) {
		Tm = localtime(&TimeVal.tv_sec);
		if (Fmt == 0) {
			//strftime(DateTime, 16, "%Y%m%d-%I%M%S", Tm);
			sprintf(DateTime, "%04d%02d%02d-%02d%02d%02d", Tm->tm_year+1900, Tm->tm_mon+1, Tm->tm_mday, Tm->tm_hour, Tm->tm_min, Tm->tm_sec);
		}
		else {
			//strftime(DateTime, 20, "%Y-%m-%d %I:%M:%S", Tm);
			sprintf(DateTime, "%04d-%02d-%02d %02d:%02d:%02d", Tm->tm_year+1900, Tm->tm_mon+1, Tm->tm_mday, Tm->tm_hour, Tm->tm_min, Tm->tm_sec);
		}
	}

	return ((int64_t)TimeVal.tv_sec*1000 + TimeVal.tv_usec/1000);
}

int32_t System_SetTime(StationHandle *Station, int32_t TimeStamp)
{
	int32_t Ret;
	struct timeval TimeVal;

	TimeVal.tv_usec = 0;
	TimeVal.tv_sec = (time_t)TimeStamp;
	Ret = settimeofday(&TimeVal, NULL);
	if(Ret < 0) {
		LOG_ERROR(TAG, "settimeofday failed\n");
		return -1;
	}
	
	return 0;
}

int32_t System_ImportConfig(StationHandle *Station)
{
	char Buf[2048];
	FILE *File;
	cJSON *RootObj, *SubObj;
	SystemHandle *System = Station->System;

	File = fopen(SYSTEM_CONFIG, "r");
	if (File == NULL) {
		LOG_WARN(TAG, "fopen %s failed\n", SYSTEM_CONFIG);
		return -1;
	}

	fread(Buf, 1, sizeof(Buf) - 1, File);
	fclose(File);
	
	RootObj = cJSON_Parse(Buf);
	if (RootObj == NULL) {
		LOG_ERROR(TAG, "cJSON_Parse failed\n");
		return -1;
	}

	SubObj = cJSON_GetObjectItem(RootObj, "P2P");
	if (SubObj) {
		cJSON *ItemObj;

		LOG_DEBUG(TAG, "P2P: \n");
		ItemObj = cJSON_GetObjectItem(SubObj, "uid");
		if (ItemObj) {
			LOG_DEBUG(TAG, "uid=%s\n", ItemObj->valuestring);
			Profile_Write(System->Station, "P2P", "uid", ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "user");
		if (ItemObj) {
			LOG_DEBUG(TAG, "user=%s\n", ItemObj->valuestring);
			Profile_Write(System->Station, "P2P", "user", ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "passwd");
		if (ItemObj) {
			LOG_DEBUG(TAG, "passwd=%s\n", ItemObj->valuestring);
			Profile_Write(System->Station, "P2P", "passwd", ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "authKey");
		if (ItemObj) {
			LOG_DEBUG(TAG, "authKey=%s\n", ItemObj->valuestring);
			Profile_Write(System->Station, "P2P", "authKey", ItemObj->valuestring);
		}
	}
	
	SubObj = cJSON_GetObjectItem(RootObj, "Halow");
	if (SubObj) {
		cJSON *ItemObj;
		NetWorkHalowConfig HalowConf = {0};
		char Value[128] = {0};
		char *Ptr = Value;

		LOG_DEBUG(TAG, "Halow: \n");
		ItemObj = cJSON_GetObjectItem(SubObj, "freq_range");
		if (ItemObj && cJSON_GetArraySize(ItemObj) == 3) {
			for (int32_t i = 0; i < 3; i++) {
				cJSON *ArrayItemObj = cJSON_GetArrayItem(ItemObj, i);
				HalowConf.freq_range[i] = ArrayItemObj->valueint;				
			}
			sprintf(Value, "%d,%d,%d", HalowConf.freq_range[0], HalowConf.freq_range[1], HalowConf.freq_range[2]);
			LOG_DEBUG(TAG, "freq_range=%s\n", Value);
			Profile_Write(System->Station, "Halow", "freq_range", Value);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "bss_bw");
		if (ItemObj) {
			LOG_DEBUG(TAG, "bss_bw=%d\n", ItemObj->valueint);
			HalowConf.bss_bw = ItemObj->valueint;
			sprintf(Value, "%d", ItemObj->valueint);
			Profile_Write(System->Station, "Halow", "bss_bw", Value);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "tx_mcs");
		if (ItemObj) {
			LOG_DEBUG(TAG, "tx_mcs=%d\n", ItemObj->valueint);
			HalowConf.tx_mcs = ItemObj->valueint;
			sprintf(Value, "%d", ItemObj->valueint);
			Profile_Write(System->Station, "Halow", "tx_mcs", Value);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "chan_list");
		if (ItemObj) {
			int32_t Size = cJSON_GetArraySize(ItemObj);

			if (Size > 8) {
				Size = 8;
			}
			for (int32_t i = 0; i < Size; i++) {
				cJSON *ArrayItemObj = cJSON_GetArrayItem(ItemObj, i);
				HalowConf.chan_list[i] = ArrayItemObj->valueint;				
				Ptr += sprintf(Ptr, (i == 0 ? "%d" : ",%d"), ArrayItemObj->valueint);
			}
			LOG_DEBUG(TAG, "chan_list=%s\n", Value);
			Profile_Write(System->Station, "Halow", "chan_list", Value);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "key_mgmt");
		if (ItemObj) {
			LOG_DEBUG(TAG, "key_mgmt=%s\n", ItemObj->valuestring);
			strcpy(HalowConf.key_mgmt, ItemObj->valuestring);
			Profile_Write(System->Station, "Halow", "key_mgmt", ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "wpa_psk");
		if (ItemObj) {
			LOG_DEBUG(TAG, "wpa_psk=%s\n", ItemObj->valuestring);
			strcpy(HalowConf.wpa_psk, ItemObj->valuestring);
			Profile_Write(System->Station, "Halow", "wpa_psk", ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "ssid");
		if (ItemObj) {
			LOG_DEBUG(TAG, "ssid=%s\n", ItemObj->valuestring);
			strcpy(HalowConf.ssid, ItemObj->valuestring);
			Profile_Write(System->Station, "Halow", "ssid", ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "mode");
		if (ItemObj) {
			LOG_DEBUG(TAG, "mode=%s\n", ItemObj->valuestring);
			strcpy(HalowConf.mode, ItemObj->valuestring);
			Profile_Write(System->Station, "Halow", "mode", ItemObj->valuestring);
		}

		Network_HalowConfig(System->Station, &HalowConf);
	}
	
	SubObj = cJSON_GetObjectItem(RootObj, "WiFi");
	if (SubObj) {
		cJSON *ItemObj;
		NetWorkWiFiConfig WiFiConf = {0};
		
		LOG_DEBUG(TAG, "WiFi: \n");
		ItemObj = cJSON_GetObjectItem(SubObj, "key_mgmt");
		if (ItemObj) {
			LOG_DEBUG(TAG, "key_mgmnt=%s\n", ItemObj->valuestring);
			strcpy(WiFiConf.key_mgmt, ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "psk");
		if (ItemObj) {
			LOG_DEBUG(TAG, "psk=%s\n", ItemObj->valuestring);
			strcpy(WiFiConf.psk, ItemObj->valuestring);
		}
		
		ItemObj = cJSON_GetObjectItem(SubObj, "ssid");
		if (ItemObj) {
			LOG_DEBUG(TAG, "ssid=%s\n", ItemObj->valuestring);
			strcpy(WiFiConf.ssid, ItemObj->valuestring);
		}

		ItemObj = cJSON_GetObjectItem(SubObj, "time_zone");
		if (ItemObj) {
			char TimeZone[8] = {0};
			char Cmd[128];
			
			LOG_DEBUG(TAG, "time_zone=%s\n", ItemObj->valuestring);
			strcpy(TimeZone, ItemObj->valuestring);
			if (TimeZone[3] == '-') {
				TimeZone[3] = '+';
			}
			else {
				TimeZone[3] = '-';
			}

			unlink("/config/localtime");
			sprintf(Cmd, "ln -s /usr/share/zoneinfo/%s /config/localtime", TimeZone);
			system_call(Cmd, 1000);
		}

		Network_WiFiConfig(System->Station, &WiFiConf, 1);
	}
	
	cJSON_Delete(RootObj);
	
	return 0;
}

int32_t System_LedBlink(StationHandle *Station, int32_t LedNum, int32_t IntervalMs)
{
	SystemHandle *System = Station->System;
	LedObj *Led = NULL;

	switch (LedNum) {
		case LED_CAM_1:
			Led = &System->Led[0];
			break;
		case LED_CAM_2:
			Led = &System->Led[1];
			break;
		case LED_CAM_3:
			Led = &System->Led[2];
			break;
		case LED_CAM_4:
			Led = &System->Led[3];
			break;
		case LED_STA_RED:
			Led = &System->Led[4];
			break;
		case LED_STA_BLUE:
			Led = &System->Led[5];
			break;
		default:
			break;
	}
	
	if (Led->Timer == NULL) {
		char Name[16];

		sprintf(Name, "LED%d", LedNum);
		Led->Timer = Timer_Start(Name, IntervalMs, 1, System_LedToggle, Led);
	}
	else {
		Timer_Set(Led->Timer, IntervalMs);
	}
	
	return 0;
}

int32_t System_LedSet(StationHandle *Station, int32_t LedNum, int32_t State)
{
	SystemHandle *System = Station->System;
	LedObj *Led = NULL;

	switch (LedNum) {
		case LED_CAM_1:
			Led = &System->Led[0];
			break;
		case LED_CAM_2:
			Led = &System->Led[1];
			break;
		case LED_CAM_3:
			Led = &System->Led[2];
			break;
		case LED_CAM_4:
			Led = &System->Led[3];
			break;
		case LED_STA_RED:
			Led = &System->Led[4];
			break;
		case LED_STA_BLUE:
			Led = &System->Led[5];
			break;
		default:
			break;
	}
	
	if (Led->Timer) {
		Timer_Stop(Led->Timer);
		Led->Timer = NULL;
	}
	
	Led->State = State;
	GPIO_Set(Led->Pin, Led->State);
	
	return 0;
}

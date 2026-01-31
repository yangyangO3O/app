#ifndef __SYSTEM_H__
#define __SYSTEM_H__

#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <semaphore.h>
#include "common.h"
#include "timer.h"
#include "camera_manage.h"
#include "hardware.h"

#define DEVICE_NAME_LEN         16
#define DEVICE_ID_LEN           16

#pragma pack(push) //保存对齐状态
#pragma pack(1)

typedef struct {
        char Name[DEVICE_NAME_LEN];
        char ProductId[48];
        char DeviceId[48];
        char Zone;
} DeviceInfo;

typedef struct {
        //uint8_t Indicator:1;
        uint8_t Indicator:4;
        uint8_t LightEnable:1;
        uint8_t LightLvl:3;

        uint8_t NightMode;

        uint8_t AntiFlicker;

        uint8_t FlipMirror:4;
        uint8_t Stamp:1;
        uint8_t StampFmt:3;

        uint8_t Microphone;
        uint8_t Speaker;
        uint8_t SpeakerVol;

        uint8_t VoiceMode;

        uint8_t PowerType;
        uint8_t PowerAlarmThred;
} CameraSetting;

typedef struct {
        uint8_t Enable:1;
        uint8_t Sense:7;
        //uint8_t Reserve0:3;

        uint8_t HumanAlarmMode:2;
        uint8_t VichleAlarmMode:2;
        uint8_t PetAlarmMode:2;
        uint8_t MotionAlarmMode:2;
        uint8_t AlarmEnable:1;
        uint8_t HumanDet:1;
        uint8_t VichleDet:1;
        uint8_t PetDet:1;
        uint8_t MotionDet:1;
        uint8_t Mark:1;
        uint8_t Reserve1:2;

        uint8_t RecordEnable:1;
        uint8_t RecordDuration:7;

        union {
                uint8_t Day;
                struct {
                        uint8_t WDayMon:1;
                        uint8_t WDayTue:1;
                        uint8_t WDayWed:1;
                        uint8_t WDayThu:1;
                        uint8_t WDayFri:1;
                        uint8_t WDaySat:1;
                        uint8_t WDaySun:1;
                        uint8_t Res:1;
                }DaySt;
        } WDay;
        uint8_t StartHour:5;
        uint8_t StartMin:6;
        uint8_t EndHour:5;
        uint8_t EndMin:6;
        uint8_t WholeDay:1;
} DetectSetting;

typedef struct {
        CameraSetting Camera;
        DetectSetting Detect;
} SysSetting;
#pragma pack(pop)//恢复对齐状态

typedef struct {
        int32_t Pin;
        int32_t State;
        int32_t IsBlink;
        TimerObj *Timer;
} LedObj;

struct SystemHandle {
        StationHandle  *Station;
        LedObj                  Led[6];
        int32_t                 DateTimeSyncState;
        int32_t                 UpgradeState;
        pthread_t       KeyListenThread;
        pthread_t       KeyProcessThread;
        pthread_t       CommonThread;
        pthread_t       FwUpgradeThread;
        sem_t           KeySem;
        pthread_mutex_t Mutex;
        SysSetting              Setting;
        void            *Priv[0];
};

int32_t System_Init(StationHandle *Station);
void System_Deinit(StationHandle *Station);
void System_Reboot(StationHandle *Station);
int64_t System_GetTimeStamp(StationHandle *Station, char *DateTime, int32_t Fmt);
int32_t System_SetTime(StationHandle *Station, int32_t TimeStamp);
int32_t System_ImportConfig(StationHandle *Station);

int32_t System_LedSet(StationHandle *Station, int32_t LedNum, int32_t State);
int32_t System_LedBlink(StationHandle *Station, int32_t LedNum, int32_t IntervalMs);


#endif

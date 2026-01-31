#ifndef __STORAGE_H__
#define __STORAGE_H__

#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <semaphore.h>
#include "common.h"

#define STORAGE_CARD            "/tmp/mnt/sdcard"

struct StorageHandle {
        StationHandle  *Station;
        pthread_t       DetectThread;
        int32_t                 CardInsert;
        int32_t                 CardReady;
        pthread_mutex_t Mutex;
        void            *Priv[0];
};

int32_t Storage_Init(StationHandle *Station);
void Storage_Deinit(StationHandle *Station);
int32_t Storage_IsReady(StationHandle *Station);
int32_t Storage_Format(StationHandle *Station);
int32_t Storage_GetCapacity(StationHandle *Station, uint32_t *Total, uint32_t *Free);

#endif

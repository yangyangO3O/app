#ifndef __COMMON_H__
#define __COMMON_H__

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>
#include <sys/prctl.h>

#include "log.h"


typedef struct ProfileHandle ProfileHandle;
typedef struct SystemHandle SystemHandle;
typedef struct StorageHandle StorageHandle;
typedef struct NetworkHandle  NetworkHandle;
typedef struct CamManageHandle CamManageHandle;
typedef struct StreamHandle StreamHandle;
typedef struct RecordHandle RecordHandle;
typedef struct P2pHandle P2pHandle;
typedef struct DevManageHandle DevManageHandle;

typedef struct {
        int32_t                         PowerState;
        sem_t                           ExitSem;
        ProfileHandle           *Profile;
        SystemHandle            *System;
        StorageHandle           *Storage;
        NetworkHandle           *Network;
        CamManageHandle         *CameraMag;
        StreamHandle            *Stream;
        RecordHandle            *Record;
        P2pHandle                       *P2p;
        DevManageHandle         *DevManage;
} StationHandle;

#endif

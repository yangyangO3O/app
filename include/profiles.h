#ifndef __PROFILES_H__
#define __PROFILES_H__

#include "common.h"

struct ProfileHandle {
        StationHandle  *Station;
        pthread_mutex_t Mutex;
        void            *Priv[0];
};

int32_t Profile_Init(StationHandle *Station);
void Profile_Deinit(StationHandle *Station);
int32_t Profile_IsReady(StationHandle *Station);
int32_t Profile_Read(StationHandle *Station, char *Token, char *Key, char *Result);
int32_t Profile_Write(StationHandle *Station, char *Token, char *Key, char *Result);


#endif

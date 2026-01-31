#ifndef __DEVICE_MANAGE_H__
#define __DEVICE_MANAGE_H__

#include "common.h"
#include "womqtt.h"
#include "wohttp.h"


#define REPORT_MAX_CNT                  3
#define REPORT_DELAY_US                 50 * 1000


struct DevManageHandle {
        StationHandle  *Station;
        WoMqtt          *Mqtt;
        int32_t         State;
        pthread_t       LoginThd;
        void            *Priv[0];
};

int32_t DevManage_Init(StationHandle *Station);
void DevManage_Deinit(StationHandle *Station);

#endif

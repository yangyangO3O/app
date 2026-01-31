#ifndef __NETWORK_H__
#define __NETWORK_H__

#include <stdio.h>
#include <signal.h>
#include <sys/time.h>
#include <semaphore.h>
#include "common.h"

#define HALOW_IF "hg0"
#define WIFI_IF  "wlan0"
#define ETH_IF   "eth0"


typedef struct network_wifi_config {
        char ssid[32];
        char key_mgmt[32];
        char psk[32];
} NetWorkWiFiConfig;

typedef struct network_halow_config {
        int32_t freq_range[3];
        int32_t bss_bw;
        int32_t tx_mcs;
        int32_t chan_list[8];
        char key_mgmt[16];
        char wpa_psk[72];
        char ssid[32];
        char mode[8];
} NetWorkHalowConfig;

struct NetworkHandle {
        StationHandle  *Station;
        pthread_t       HalowThread;
        pthread_t       WifiThread;
        pthread_t       EthernetThread;
        int32_t                 HalowReady;
        int32_t                 WifiReady;
        int32_t                 EthReady;
        int32_t                 WifiReconfig;
        int32_t                 WifiRssi;

    // [新增] HaLow 信号质量统计
    int32_t         HalowRssi;
    int32_t         HalowEvm;
    uint32_t        HalowBitrateKbps;

        pthread_mutex_t Mutex;
        void            *Priv[0];
};


int32_t Network_Init(StationHandle *Station);
void Network_Deinit(StationHandle *Station);
int32_t Network_IsHalowReady(StationHandle *Station);
int32_t Network_HalowConfig(StationHandle *Station, NetWorkHalowConfig *HalowConf);
int32_t Network_HalowPaire(StationHandle *Station);
int32_t Network_HalowWakeup(StationHandle *Station, char *Mac);
int32_t Network_IsReady(StationHandle *Station);
int32_t Network_WiFiConfig(StationHandle *Station, NetWorkWiFiConfig *WiFiConf, int32_t Enable);
// [新增] 获取 HaLow 信号质量接口
int32_t Network_GetHalowState(StationHandle *Station, int *rssi, int *evm, int *bitrate);

#endif

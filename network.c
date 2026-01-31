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
#include "network.h"
#include "hgic.h"

#define TAG 	"NETWORK"
#define HG_EVT			"/proc/hgicf/fwevnt"
#define HG_IW			"/proc/hgicf/iwpriv"

#define HALOW_CARRIER	"sys/class/net/hg0/carrier"
#define ETH_CARRIER		"sys/class/net/eth0/carrier"
#define WIFI_CARRIER	"sys/class/net/wlan0/carrier"

#define HALOW_CONFIG	"/config/hgicf.conf"
#define WIFI_CONFIG		"/config/wpa_supplicant.conf"

static int Network_HalowFwEventParse(NetworkHandle *Network, uint8_t *event_data, uint32_t event_len)
{
    int i = 0;
    uint32_t   data_len = 0;
    uint32_t   evt_id   = 0;
    char *data     = NULL;
    char buff[4096];
    struct hgic_exception_info *exp;    
    struct hgic_dhcp_result *dhcpc;
    struct hgic_ctrl_hdr *evt = (struct hgic_ctrl_hdr *)event_data;

    data     = (char *)(evt + 1);
    data_len = event_len - sizeof(struct hgic_ctrl_hdr);
    evt_id   = HDR_EVTID(evt);

    // printf("recv firmware event %d\r\n", evt_id); 
    switch (evt_id) {
        case HGIC_EVENT_SCANNING:
            printf("start scan ...\r\n");
            break;
        case HGIC_EVENT_SCAN_DONE:
            printf("scan done!\r\n");
            hgic_iwpriv_get_scan_list(HALOW_IF, buff, 4096);
            printf("%s\r\n", buff);
            break;
        case HGIC_EVENT_TX_BITRATE:
            pthread_mutex_lock(&Network->Mutex);
            Network->HalowBitrateKbps = *(unsigned int *)data;
            pthread_mutex_unlock(&Network->Mutex);
            printf("estimate tx bitrate:%dKbps\r\n", *(unsigned int *)data);
            break;
        case HGIC_EVENT_PAIR_START:
            printf("start pairing ...\r\n");
            break;
        case HGIC_EVENT_PAIR_SUCCESS:
            printf("pairing success! [" MACSTR "]\r\n", MACARG(data));
            hgic_iwpriv_set_pairing(HALOW_IF, 0); //stop pair
            break;
        case HGIC_EVENT_PAIR_DONE:
            printf("pairing done!\r\n");
            for(i=0; i*6 < data_len;i++){
                printf("  sta%d:"MACSTR"\r\n", i, MACARG(data+6*i));
            }
            break;
        case HGIC_EVENT_CONECT_START:
            printf("start connecting ...\r\n");
            break;
        case HGIC_EVENT_CONECTED:
            printf("new sta "MACSTR" connected!\r\n", MACARG(data));
            break;
        case HGIC_EVENT_ROAM_CONECTED:
            printf("roam success to "MACSTR"!\r\n", MACARG(data));
            break;
        case HGIC_EVENT_DISCONECTED:
            // [新增] 断开时重置状态，防止上层误判信号良好
            pthread_mutex_lock(&Network->Mutex);
            Network->HalowRssi = -127;
            Network->HalowBitrateKbps = 0;
            pthread_mutex_unlock(&Network->Mutex);
            printf("sta "MACSTR" disconnected, reason_code=%d\r\n", MACARG(data), get_unaligned_le16(data+6));
            break;
        case HGIC_EVENT_SIGNAL:
            pthread_mutex_lock(&Network->Mutex);
            Network->HalowRssi = (signed char)data[0];
            Network->HalowEvm  = (signed char)data[1];
            pthread_mutex_unlock(&Network->Mutex);
            printf("signal changed: rssi:%d, evm:%d\r\n", (signed char)data[0], (signed char)data[1]);
            break;
        case HGIC_EVENT_CUSTOMER_MGMT:
            printf("rx customer mgmt frame from "MACSTR", %d bytes \r\n", MACARG(data), data_len-6);
            break;
        case HGIC_EVENT_DHCPC_DONE:
            dhcpc = (struct hgic_dhcp_result *)data;
            printf("fw dhcpc result: ipaddr:"IPSTR", netmask:"IPSTR", svrip:"IPSTR", router:"IPSTR", dns:"IPSTR"/"IPSTR"\r\n",
                IPARG(dhcpc->ipaddr), IPARG(dhcpc->netmask), IPARG(dhcpc->svrip),
                IPARG(dhcpc->router), IPARG(dhcpc->dns1), IPARG(dhcpc->dns2));
            break;
        case HGIC_EVENT_CONNECT_FAIL:
            printf("connect fail, status_code=%d\r\n", get_unaligned_le16(data));
            break;
        case HGIC_EVENT_CUST_DRIVER_DATA:
            printf("rx customer driver data %d bytes\r\n", data_len);
            break;
        case HGIC_EVENT_UNPAIR_STA:
            printf("unpair sta:"MACSTR"\r\n", MACARG(data));
            break;
        case HGIC_EVENT_EXCEPTION_INFO:
            exp = (struct hgic_exception_info *)data;
            switch(exp->num){
                case HGIC_EXCEPTION_TX_BLOCKED:
                    printf("*wireless tx blocked, maybe need reset wifi module*\r\n");
                    break;
                case HGIC_EXCEPTION_TXDELAY_TOOLONG:
                    printf("*wireless txdelay too loog, %d:%d:%d *\r\n", 
                            exp->info.txdelay.max, exp->info.txdelay.min, exp->info.txdelay.avg);
                    break;
                case HGIC_EXCEPTION_STRONG_BGRSSI:
                    printf("*detect strong backgroud noise. %d:%d:%d *\r\n", 
                            exp->info.bgrssi.max, exp->info.bgrssi.min, exp->info.bgrssi.avg);
                    break;
                case HGIC_EXCEPTION_TEMPERATURE_OVERTOP:
                    printf("*chip temperature too overtop: %d *\r\n", exp->info.temperature.temp);
                    break;
                case HGIC_EXCEPTION_WRONG_PASSWORD:
                    printf("*password maybe is wrong *\r\n");
                    break;
            }
            break;
    }
    
	return 0;
}

static void* Network_HalowThread(void *Args)
{
	NetworkHandle *Network;
	int32_t EvtFd = -1;
	int32_t IwFd = -1;
	int32_t MaxFd;
	int32_t Ret;
	fd_set Rdfds, RdSet;
    uint8_t *Buff;
	
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());

	Network = (NetworkHandle *)Args;
	while (access(HALOW_CARRIER, F_OK) < 0) {
		LOG_ERROR(TAG, "%s is not found\n", HALOW_IF);
		sleep(1);
	}

	Buff = malloc(4096);
	if (Buff == NULL) {
		LOG_ERROR(TAG, "malloc fail\n");
		goto Network_HalowThread_Exit;
	}

	IwFd = open(HG_IW, O_RDONLY);
	if (IwFd < 0) {
		LOG_ERROR(TAG, "open %s fail\r\n", HG_IW);
		goto Network_HalowThread_Exit;
	}
	close(IwFd);
	
	Network->HalowReady = 1;

OPEN:
	EvtFd = open(HG_EVT, O_RDONLY);
	if (EvtFd == -1) {
		LOG_ERROR(TAG, "open %s fail\n", HG_EVT);
		sleep(1);
		goto OPEN;
	}
	
	FD_ZERO(&Rdfds);
	FD_SET(EvtFd, &Rdfds);
	MaxFd = EvtFd;

    while (1) {
		struct timeval TimeVal;

		TimeVal.tv_sec = 5;
		TimeVal.tv_usec = 0;
		
		RdSet = Rdfds;
		Ret = select(MaxFd + 1, &RdSet, NULL, NULL, &TimeVal);
		if (Ret < 0) {
			//Error
		}
		if (Ret == 0) {
			//Timeout
		}
		else if (FD_ISSET(EvtFd, &RdSet)) {
	        Ret = read(EvtFd, Buff, 4096);
	        if (Ret > 0) {
	            Network_HalowFwEventParse(Network, Buff, Ret);
	        }
			else if(Ret < 0){
	            close(EvtFd);
	            goto OPEN;
	        }
		}
    }

Network_HalowThread_Exit:
	if (EvtFd > 0) {
    	close(EvtFd);
	}
	
	if (Buff) {
		free(Buff);
	}

	pthread_exit(NULL);
}

static void* Network_WifiThread(void *Args)
{
	NetworkHandle *Network;
	int32_t Ret;
	int32_t CarrierFd = -1;
	char Buf[64];

	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());

	Network = (NetworkHandle *)Args;
	while (1) {
		sleep(1);
		
		if (access(WIFI_CARRIER, F_OK) < 0) {
			continue;
		}
		
		memset(Buf, 0, sizeof(Buf));
		CarrierFd = open(WIFI_CARRIER, O_RDONLY);
		if (CarrierFd < 0) {
			LOG_WARN(TAG, "open %s fail\r\n", WIFI_CARRIER);
		}
		else {
			read(CarrierFd, Buf, sizeof(Buf));
			close(CarrierFd);
		}
		
		if (Buf[0] == '1') {
			break;
		}
	}

	while (1) {
		sleep(1);
		
		sprintf(Buf, "udhcpc -i %s", WIFI_IF);
		Ret = system_call(Buf, 30*1000);
		if (Ret != 0) {
			LOG_ERROR(TAG, "%s is not ready\n", WIFI_IF);
			continue;
		}
		
		Network->WifiReconfig = 0;
		pthread_mutex_lock(&Network->Mutex);
		Network->WifiReady = 1;
		pthread_mutex_unlock(&Network->Mutex);
		System_LedBlink(Network->Station, LED_STA_RED, 500);
				
		while (!Network->WifiReconfig) {
			sleep(3);
		}
		
		pthread_mutex_lock(&Network->Mutex);
		Network->WifiReady = 0;
		pthread_mutex_unlock(&Network->Mutex);
		System_LedSet(Network->Station, LED_STA_RED, 0);
	}

	pthread_exit(NULL);
}

static void* Network_EthernetThread(void *Args)
{
	NetworkHandle *Network;
	int32_t Ret;
	int32_t CarrierFd = -1;
	char Buf[64];
	
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());

	Network = (NetworkHandle *)Args;
	while (1) {
		sleep(1);
		if (access(ETH_CARRIER, F_OK) < 0) {
			continue;
		}
		
		memset(Buf, 0, sizeof(Buf));
		CarrierFd = open(ETH_CARRIER, O_RDONLY);
		if (CarrierFd < 0) {
			LOG_WARN(TAG, "open %s fail\r\n", ETH_CARRIER);
		}
		else {
			read(CarrierFd, Buf, sizeof(Buf));
			close(CarrierFd);
		}
		
		if (Buf[0] == '1') {
			break;
		}
	}

	sprintf(Buf, "udhcpc -i %s", ETH_IF);
	Ret = system_call(Buf, 30*1000);
	if (Ret != 0) {
		LOG_ERROR(TAG, "%s is not ready\n", ETH_IF);
		goto NetNetwork_EthernetThread_Exit;
	}
	
	pthread_mutex_lock(&Network->Mutex);
	Network->EthReady = 1;
	pthread_mutex_unlock(&Network->Mutex);
	
	System_LedBlink(Network->Station, LED_STA_RED, 500);
	
	while (1) {
		sleep(1);
	}

NetNetwork_EthernetThread_Exit:
	pthread_exit(NULL);
}

int32_t Network_Init(StationHandle *Station)
{
	NetworkHandle *Network = NULL;
	int32_t Ret = 0;

	Network = calloc(1, sizeof(NetworkHandle));
	if (Network == NULL) {
		LOG_ERROR(TAG, "calloc NetworkHandle failed\n");
		return -1;
	}
	
	Station->Network = Network;
	Network->Station = Station;
	
	// Init Mutex before threads
	Ret = pthread_mutex_init(&Network->Mutex, NULL);
	if (Ret != 0) {
		LOG_ERROR(TAG, "pthread_mutex_init failed\n");
		goto Network_Init_Error;
	}

	Ret = pthread_create(&Network->HalowThread, NULL, Network_HalowThread, Network);
	if (Ret < 0) {
		LOG_ERROR(TAG, "pthread_create Network_HalowThread failed\n");
		goto Network_Init_Error;
	}

	Ret = pthread_create(&Network->WifiThread, NULL, Network_WifiThread, Network);
	if (Ret < 0) {
		LOG_ERROR(TAG, "pthread_create Stream_WifiThread failed\n");
		goto Network_Init_Error;
	}
	
	Ret = pthread_create(&Network->EthernetThread, NULL, Network_EthernetThread, Network);
	if (Ret < 0) {
		LOG_ERROR(TAG, "pthread_create Network_EthernetThread failed\n");
		goto Network_Init_Error;
	}

	return 0;
Network_Init_Error:
	if (Network->HalowThread > 0) pthread_cancel(Network->HalowThread);
	if (Network->WifiThread > 0) pthread_cancel(Network->WifiThread);
	if (Network->EthernetThread > 0) pthread_cancel(Network->EthernetThread);
	pthread_mutex_destroy(&Network->Mutex);
	free(Network);
	Station->Network = NULL;

	return -1;
}

void Network_Deinit(StationHandle *Station)
{
	NetworkHandle *Network = Station->Network;
	
	if (Network) {
		char Cmd[32];
		
		pthread_cancel(Network->HalowThread);
		pthread_cancel(Network->WifiThread);
		pthread_cancel(Network->EthernetThread);
		pthread_mutex_destroy(&Network->Mutex);
		free(Network);

		sprintf(Cmd, "kill_process");
		system_call(Cmd, 0);

		Station->Network = NULL;
	}
}

int32_t Network_IsHalowReady(StationHandle *Station)
{
	int32_t Ready;
	NetworkHandle *Network = Station->Network;

	if (Network == NULL) {
		return 0;
	}
	
	pthread_mutex_lock(&Network->Mutex);
	Ready = Network->HalowReady;
	pthread_mutex_unlock(&Network->Mutex);

	return Ready;
}

int32_t Network_GetHalowState(StationHandle *Station, int *rssi, int *evm, int *bitrate)
{
    NetworkHandle *Network = Station->Network;
    if (Network == NULL) return -1;

    pthread_mutex_lock(&Network->Mutex);
    if (rssi) *rssi = Network->HalowRssi;
    if (evm) *evm = Network->HalowEvm;
    if (bitrate) *bitrate = Network->HalowBitrateKbps;
    pthread_mutex_unlock(&Network->Mutex);
    
    return 0;
}

int32_t Network_HalowConfig(StationHandle *Station, NetWorkHalowConfig *HalowConf)
{
	FILE *File;
	char ChanList[128] = {0};
	char *Ptr = ChanList;
	int32_t i, ChanCnt;
	
	File = fopen(HALOW_CONFIG, "w+");
	if (File == NULL) {
		LOG_ERROR(TAG, "fopen %s failed\n", HALOW_CONFIG);
		return -1;
	}

	if (HalowConf->freq_range[0] == 0 || HalowConf->freq_range[1] == 0 || HalowConf->freq_range[2] == 0) {
		fprintf(File, "freq_range=\n");
	}
	else {
		fprintf(File, "freq_range=%d,%d,%d\n", HalowConf->freq_range[0], HalowConf->freq_range[1], HalowConf->freq_range[2]);
		hgic_iwpriv_set_freqrange(HALOW_IF, HalowConf->freq_range[0], HalowConf->freq_range[1], HalowConf->freq_range[2]);
	}
	fprintf(File, "bss_bw=%d\n", HalowConf->bss_bw);
	fprintf(File, "tx_mcs=%d\n", HalowConf->tx_mcs);
	for (i = 0, ChanCnt = 0; HalowConf->chan_list[i] != 0; i++) {
		ChanCnt++;
		Ptr += sprintf(Ptr, (i == 0 ? "%d" : ",%d"), HalowConf->chan_list[i]);
	}
	if (ChanCnt > 0) {
		fprintf(File, "chan_list=%s\n", ChanList);
		hgic_iwpriv_set_chan_list(HALOW_IF, HalowConf->chan_list, ChanCnt);
	}
	fprintf(File, "key_mgmt=%s\n", HalowConf->key_mgmt);
	fprintf(File, "wpa_psk=%s\n", HalowConf->wpa_psk);
	fprintf(File, "ssid=%s\n", HalowConf->ssid);
	fprintf(File, "mode=%s\n", HalowConf->mode);
	fclose(File);

	if (strlen(HalowConf->ssid) > 0) {
		hgic_iwpriv_set_ssid(HALOW_IF, HalowConf->ssid);
	}
	if (strcmp(HalowConf->key_mgmt, "NONE") && strlen(HalowConf->wpa_psk) > 0) {
		hgic_iwpriv_set_wpapsk(HALOW_IF, HalowConf->wpa_psk);
	}
	if (strlen(HalowConf->mode) > 0) {
		hgic_iwpriv_set_mode(HALOW_IF, HalowConf->mode);
	}
	
	return 0;
}

int32_t Network_HalowPaire(StationHandle *Station)
{
	return hgic_iwpriv_set_pairing(HALOW_IF, 1);
}

int32_t Network_HalowWakeup(StationHandle *Station, char *Mac)
{
	return hgic_iwpriv_wakeup_sta(HALOW_IF, Mac);
}

int32_t Network_IsReady(StationHandle *Station)
{
	int32_t Ready;
	NetworkHandle *Network = Station->Network;

	if (Network == NULL) {
		return 0;
	}
	
	pthread_mutex_lock(&Network->Mutex);
	Ready = (Network->WifiReady || Network->EthReady);
	pthread_mutex_unlock(&Network->Mutex);

	return Ready;
}

int32_t Network_WiFiConfig(StationHandle *Station, NetWorkWiFiConfig *WiFiConf, int32_t Enable)
{
	int32_t Ret, NetworkId;
	char Buf[1024] = {0};
	char Cmd[128];

	if (Station->Network == NULL) {
		return 0;
	}

	NetworkId = -1;
	sprintf(Cmd, "wpa_cli -i %s list_networks | grep '%s'", WIFI_IF, WiFiConf->ssid);
	Ret = popen_call(Cmd, Buf, sizeof(Buf) - 1, 0);
	if (Ret < 0) {
		LOG_ERROR(TAG, "popen_call %s failed\n", Cmd);
		return -1;
	}
	
	if (strlen(Buf) > 0) {
		printf("%s\n", Buf);
		sscanf(Buf, "%d", &NetworkId);
	}

	if (NetworkId < 0) {
		sprintf(Cmd, "wpa_cli -i %s add_network", WIFI_IF);
		Ret = popen_call(Cmd, Buf, sizeof(Buf) - 1, 0);
		if (Ret < 0) {
			LOG_ERROR(TAG, "system_call %s failed\n", Cmd);
			return -1;
		}
		sscanf(Buf, "%d", &NetworkId);
	}
	LOG_DEBUG(TAG, "NetworkId = %d\n", NetworkId);

	if (strlen(WiFiConf->ssid) > 0) {
		sprintf(Cmd, "wpa_cli -i %s set_network %d ssid '\"%s\"'", WIFI_IF, NetworkId, WiFiConf->ssid);
		Ret = system_call(Cmd, 0);
		if (Ret < 0) {
			LOG_ERROR(TAG, "system_call %s failed\n", Cmd);
			return -1;
		}
	}
	
	if (strlen(WiFiConf->key_mgmt) > 0) {
		sprintf(Cmd, "wpa_cli -i %s set_network %d key_mgmt %s", WIFI_IF, NetworkId, WiFiConf->key_mgmt);
		Ret = system_call(Cmd, 0);
		if (Ret < 0) {
			LOG_ERROR(TAG, "system_call %s failed\n", Cmd);
			return -1;
		}
	}

	if (strlen(WiFiConf->psk) > 0) {
		sprintf(Cmd, "wpa_cli -i %s set_network %d psk '\"%s\"'", WIFI_IF, NetworkId, WiFiConf->psk);
		Ret = system_call(Cmd, 0);
		if (Ret < 0) {
			LOG_ERROR(TAG, "system_call %s failed\n", Cmd);
			return -1;
		}
	}

	if (Enable) {
		sprintf(Cmd, "wpa_cli -i %s select_network %d", WIFI_IF, NetworkId);
		Ret = system_call(Cmd, 0);
		if (Ret < 0) {
			LOG_ERROR(TAG, "system_call %s failed\n", Cmd);
			return -1;
		}
		
		sprintf(Buf, "wpa_cli -i %s save_config", WIFI_IF);
		Ret = system_call(Buf, 0);
		if (Ret < 0) {
			LOG_ERROR(TAG, "system_call %s failed\n", Buf);
		}
		
		Station->Network->WifiReconfig = 1;
	}
	
	return 0;
}

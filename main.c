#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <semaphore.h>
#include "hardware.h"
#include "system_call.h"
#include "profile.h"
#include "system.h"
#include "storage.h"
#include "network.h"
#include "camera_manage.h"
#include "stream.h"
#include "record.h"
#include "p2p.h"
#include "device_manage.h"

#define TAG		"STATION"
#define WTD_TIMEOUT		5

#define POWER_STATE_SHUTDOWN	0
#define POWER_STATE_REBOOT		1
#define POWER_STATE_RUNNING		2

static StationHandle *StaHandle = NULL;

/*************************************************
 Function:       SigHandler
 Description:    Signal handler for the main application. Handles SIGINT to trigger graceful exit.
 Input:          SigNum - The signal number received.
 Output:         None
 Return:         None
*************************************************/
static void SigHandler(int32_t SigNum)
{
	LOG_WARN(TAG, "SigHandler: %d caught\n", SigNum);
    if (SigNum == SIGINT || SigNum == SIGTERM) {
		if (StaHandle && StaHandle->PowerState == POWER_STATE_RUNNING) {
			StaHandle->PowerState = POWER_STATE_REBOOT;
		}
    }
}

/*************************************************
 Function:       StationInit
 Description:    Allocates and initializes the main Station structure and all sub-modules.
 Input:          None
 Output:         None
 Return:         Pointer to initialized StationHandle, or NULL on failure.
*************************************************/
StationHandle* StationInit(void)
{
	int32_t Ret;
	StationHandle *Station = NULL;
	
	Station = calloc(1, sizeof(StationHandle));
	if (Station == NULL) {
		LOG_ERROR(TAG, "malloc StationHandle failed\n");
		return NULL;
	}

	Station->PowerState = POWER_STATE_RUNNING;
	sem_init(&Station->ExitSem, 0, 0);


	Ret = Profile_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "Profile_Init failed\n");
		goto Station_Init_Error;
	}
	
	Ret = Storage_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "Storage_Init failed\n");
		goto Station_Init_Error;
	}
	
	Ret = System_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "System_Init failed\n");
		goto Station_Init_Error;
	}
	/*else if (Station->System->UpgradeState >= 2) {
		return Station;
	}*/

	Ret = Network_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "Network_Init failed\n");
		goto Station_Init_Error;
	}
	
	Ret = CamManage_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "CamManage_Init failed\n");
		goto Station_Init_Error;
	}
	
	Ret = Stream_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "Stream_Init failed\n");
		goto Station_Init_Error;
	}
	
    // This is the function causing the error previously.
    // Ensure record.h is in the include path.
	Ret = Record_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "Record_Init failed\n");
		goto Station_Init_Error;
	}

	Ret = P2P_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "P2P_Init failed\n");
		goto Station_Init_Error;
	}

	Ret = DevManage_Init(Station);
	if (Ret < 0) {
		LOG_ERROR(TAG, "WoIot_Init failed\n");
		goto Station_Init_Error;
	}

	return Station;
	
Station_Init_Error:
	DevManage_Deinit(Station);
	P2P_Deinit(Station);
	Record_Deinit(Station);
	Stream_Deinit(Station);
	CamManage_Deinit(Station);
	Network_Deinit(Station);
	System_Deinit(Station);
	Storage_Deinit(Station);
	Profile_Deinit(Station);
	
	free(Station);
	
	return NULL;
}

void StationDeinit(StationHandle *Station)
{
	if (!Station) return;
	
	DevManage_Deinit(Station);
	P2P_Deinit(Station);
	Record_Deinit(Station);
	Stream_Deinit(Station);
	CamManage_Deinit(Station);
	Network_Deinit(Station);
	System_Deinit(Station);
	Storage_Deinit(Station);
	Profile_Deinit(Station);
	
	sem_destroy(&Station->ExitSem);
	free(Station);
}

int32_t StationLoop(StationHandle *Station)
{
	struct timespec TimeSpec;

	while(Station->PowerState == POWER_STATE_RUNNING) {
			if (clock_gettime(CLOCK_REALTIME, &TimeSpec) == -1) {
			LOG_ERROR(TAG, "clock_gettime error\n");
			sleep(1);
			continue;
		}
		
		TimeSpec.tv_sec += (WTD_TIMEOUT - 2);
		// Wait for exit signal or timeout (to feed watchdog)
		if (sem_timedwait(&Station->ExitSem, &TimeSpec) == -1) {
			if (errno == ETIMEDOUT) {
				//LOG_DEBUG(TAG, "Feed dog\n");
				//System_FeedWtd();
            }
			else if (errno == EINTR) {
                // Interrupted by signal (e.g., SIGINT via SigHandler)
                LOG_INFO(TAG, "StationLoop interrupted by signal\n");
			}
			else {
				LOG_ERROR(TAG, "sem_timedwait error\n");
			}
		}
		else {
			LOG_INFO(TAG, "StationLoop will exit\n");
			Station->PowerState = POWER_STATE_REBOOT;
			//System_DisableWtd();
			break;
		}
	}

	return Station->PowerState;
}

int main(int argc, const char *argv[])
{
	int32_t PowerState;
	StationHandle *Station = NULL;
	
	LOG_INFO(TAG, "Build time: %s %s\n", __DATE__, __TIME__);
	LOG_INFO(TAG, "Version: boot=%s, kernel=%s, rootfs=%s, system=%s\n", UBOOT_VERSION, KERNEL_VERSION, ROOTFS_VERSION, SYSTEM_VERSION);

	Hardware_GpioInit();

    signal(SIGINT, SigHandler);
    signal(SIGTERM, SigHandler);
	system_call_init();
	
	Station = StationInit();
	if (Station == NULL) {
		LOG_ERROR(TAG, "StationInit failed\n");
		return -1;
	}
	StaHandle = Station;

	PowerState = StationLoop(Station);

	StationDeinit(Station);
	Station = NULL;
	StaHandle = NULL;
	
	system_call_exit();
	Hardware_PowerHalow(0);
	Hardware_PowerWiFi(0);

	while (PowerState == POWER_STATE_REBOOT) {
		sleep(2);
		system("reboot &");
		sleep(5);
	}
	
    return 0;
}

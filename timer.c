#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <semaphore.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>

#include "log.h"
#include "timer.h"

#define TAG 	"TIMER"


static void *Timer_RunThread(void *Args)
{
	int32_t Ret;
	TimerObj *Timer;
	struct timespec TimeSpec;

	Timer = (TimerObj *)Args;
	//prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	prctl(PR_SET_NAME, (unsigned long)Timer->Name);
	pthread_detach(pthread_self());
	
	Ret = sem_init(&Timer->StopSem, 0, 0);
	if (Ret != 0) {
		LOG_ERROR(TAG, "pthread_mutex_init failed\n");
		pthread_exit(NULL);
	}
	
	Ret = pthread_mutex_init(&Timer->Mutex, NULL);
	if (Ret != 0) {
		LOG_ERROR(TAG, "pthread_mutex_init failed\n");
		sem_destroy(&Timer->StopSem);
		pthread_exit(NULL);
	}
	
	do {
		if (clock_gettime(CLOCK_REALTIME, &TimeSpec) == -1) {
			LOG_ERROR(TAG, "clock_gettime error\n");
			sleep(1);
			continue;
		}
		
		//TimeSpec.tv_sec += Timer->ExpireSec;
		
		pthread_mutex_lock(&Timer->Mutex);
		TimeSpec.tv_sec += (Timer->IntervalMs/1000);
		TimeSpec.tv_nsec += (Timer->IntervalMs%1000) * 1000000;
		pthread_mutex_unlock(&Timer->Mutex);
		TimeSpec.tv_sec += (TimeSpec.tv_nsec / 1000000000);
		TimeSpec.tv_nsec = TimeSpec.tv_nsec % 1000000000;
		if (sem_timedwait(&Timer->StopSem, &TimeSpec) == -1) {
			if (errno == ETIMEDOUT) {
				if (Timer->Callback) {
					Timer->Callback(Timer->UserData);
				}
			}
			else {
				LOG_ERROR(TAG, "sem_timedwait error\n");
			}
		}
		else
		{
			break;
		}
	} while(Timer->IsRepeated);
	
	sem_destroy(&Timer->StopSem);
	pthread_mutex_destroy(&Timer->Mutex);
	free(Timer);
	
	pthread_exit(NULL);
}

TimerObj *Timer_Start(char *Name, uint32_t IntervalMs, uint32_t IsRepeat, TimerCallback TimerCb, void *UserData)
{
	TimerObj *Timer;
	pthread_t TimerThd;
	int32_t Ret;

	Timer = (TimerObj *)malloc(sizeof(TimerObj));
	if(Timer == NULL) {
		LOG_ERROR(TAG, "malloc failed\n");
		return NULL;
	}

	snprintf(Timer->Name, sizeof(Timer->Name) - 1, "%s", Name);
	Timer->IntervalMs = IntervalMs;
	Timer->IsRepeated = IsRepeat;
	Timer->Callback = TimerCb;
	Timer->UserData = UserData;	
	Ret = pthread_create(&TimerThd, NULL, Timer_RunThread, Timer);
	if(Ret != 0) {
		LOG_ERROR(TAG, "pthread_create Timer_RunThread failed\n");
		sem_destroy(&Timer->StopSem);
		pthread_mutex_destroy(&Timer->Mutex);
		free(Timer);
		return NULL;
	}
	
	return Timer;
}

int32_t Timer_Set(TimerObj *Timer, uint32_t IntervalMs)
{
	if(Timer == NULL) {
		return -1;
	}
	pthread_mutex_lock(&Timer->Mutex);
	Timer->IntervalMs = IntervalMs;
	pthread_mutex_unlock(&Timer->Mutex);

	return 0;
}

void Timer_Stop(TimerObj *Timer)
{
	if(Timer == NULL) {
		return ;
	}
	
	if(Timer->IsRepeated) {
		sem_post(&Timer->StopSem);
	}
}

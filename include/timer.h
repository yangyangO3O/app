#ifndef __TIMER_H__
#define __TIMER_H__


#include "common.h"

typedef int32_t (*TimerCallback)(void *UserData);

typedef struct {
        char Name[8];
        int32_t IsRepeated;
        uint32_t IntervalMs;
        sem_t StopSem;
        pthread_mutex_t Mutex;
        TimerCallback Callback;
        void *UserData;
}TimerObj;


TimerObj *Timer_Start(char *Name, uint32_t IntervalMs, uint32_t IsRepeat, TimerCallback TimerCb, void *UserData);
void Timer_Stop(TimerObj *Timer);
int32_t Timer_Set(TimerObj *Timer, uint32_t IntervalMs);

#endif

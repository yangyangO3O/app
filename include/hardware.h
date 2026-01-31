#ifndef __HARDWARE_H__
#define __HARDWARE_H__

#define GPIO_PA(Num)    (Num)
#define GPIO_PB(Num)    (32+Num)
#define GPIO_PC(Num)    (32*2+Num)
#define GPIO_PD(Num)    (32*3+Num)

#define GPIO_DIR_IN             0
#define GPIO_DIR_OUT    1

#define LED_CAM_1               GPIO_PB(30)
#define LED_CAM_2               GPIO_PB(29)
#define LED_CAM_3               GPIO_PA(14)
#define LED_CAM_4               GPIO_PA(15)

#define LED_STA_RED             GPIO_PA(21)
#define LED_STA_BLUE    GPIO_PA(22)


#define HALOW_POWER             GPIO_PA(18)
#define WIFI_POWER              GPIO_PB(25)

int32_t GPIO_Init(int32_t Num, int32_t Direction);
int32_t GPIO_Set(int32_t Num, int32_t Value);


int32_t Hardware_GpioInit(void);
int32_t Hardware_GpioSet(int32_t Num, int32_t Value);
int32_t Hardware_PowerHalow(int32_t Enable);
int32_t Hardware_PowerWiFi(int32_t Enable);


#endif

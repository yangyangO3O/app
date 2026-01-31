#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>

#include "system_call.h"
#include "system.h"
#include "storage.h"
#include "camera_manage.h"
#include "profile.h"
#include "network.h"
#include "p2p.h"
#include "device_manage.h"
#include "cJSON.h"

#define TAG 	"DEV_MANAGE"

#if 0
static void DevManage_ReportValue(WoMqtt *Mqtt, char *pcCode, int32_t iValue)
{
    DEV_MSG_ST  stMsg;

    if (NULL == pcCode)
    {
        LOG_ERROR(TAG, "Input invalid args!!!\r\n");
        return;
    }
    
    memset(&stMsg, 0, sizeof(stMsg));
    strncpy(stMsg.stAttr.acCode, pcCode, sizeof(stMsg.stAttr.acCode) - 1);
    stMsg.stAttr.enType = MSG_TYPE_INTEGER;
    stMsg.stAttr.unValue.iValue = iValue;
    WoMqtt_AttrReport(Mqtt, &stMsg);
}

static void DevManage_ReportBool(WoMqtt *Mqtt, char *pcCode, int32_t bFlag)
{
    DEV_MSG_ST  stMsg;

    if (NULL == pcCode)
    {
        LOG_ERROR(TAG, "Input invalid args!!!\r\n");
        return;
    }
    
    memset(&stMsg, 0, sizeof(stMsg));
    strncpy(stMsg.stAttr.acCode, pcCode, sizeof(stMsg.stAttr.acCode) - 1);
    stMsg.stAttr.enType = MSG_TYPE_BOOLEAN;
    stMsg.stAttr.unValue.bBool = bFlag;
    WoMqtt_AttrReport(Mqtt, &stMsg);
}
#endif
static void DevManage_ReportEnum(WoMqtt	 *Mqtt, char *pcCode, int32_t iEnum)
{
    DEV_MSG_ST  stMsg;

    if (NULL == pcCode)
    {
        LOG_ERROR(TAG, "Input invalid args!!!\r\n");
        return;
    }
    
    memset(&stMsg, 0, sizeof(stMsg));
    strncpy(stMsg.stAttr.acCode, pcCode, sizeof(stMsg.stAttr.acCode) - 1);
    stMsg.stAttr.enType = MSG_TYPE_ENUM;
    stMsg.stAttr.unValue.uiEnum = iEnum;
    WoMqtt_AttrReport(Mqtt, &stMsg);
}

static void DevManage_ReportString(WoMqtt *Mqtt, char *pcCode, char *pcStr)
{
    DEV_MSG_ST  stMsg;

    if (NULL == pcCode)
    {
        LOG_ERROR(TAG, "Input invalid args!!!\r\n");
        return;
    }
    
    memset(&stMsg, 0, sizeof(stMsg));
    strncpy(stMsg.stAttr.acCode, pcCode, sizeof(stMsg.stAttr.acCode) - 1);
    stMsg.stAttr.enType = MSG_TYPE_STRING;
    strncpy(stMsg.stAttr.unValue.acStr, pcStr, sizeof(stMsg.stAttr.unValue.acStr) - 1);
    WoMqtt_AttrReport(Mqtt, &stMsg);
}

static void DevManage_MsgPrint(DEV_MSG_ST *pstMsg)
{
	switch(pstMsg->stAttr.enType) {
	case MSG_TYPE_BOOLEAN:
		LOG_INFO(TAG, "%s-%d\n", pstMsg->stAttr.acCode, pstMsg->stAttr.unValue.bBool);
		break;
	case MSG_TYPE_INTEGER:
		LOG_INFO(TAG, "%s-%d\n", pstMsg->stAttr.acCode, pstMsg->stAttr.unValue.iValue);
		break;
	case MSG_TYPE_ENUM:
		LOG_INFO(TAG, "%s-%d\n", pstMsg->stAttr.acCode, pstMsg->stAttr.unValue.uiEnum);
		break;
	case MSG_TYPE_JSON:
	case MSG_TYPE_STRING:
		LOG_INFO(TAG, "%s-%s\n", pstMsg->stAttr.acCode, pstMsg->stAttr.unValue.acStr);
		break;
	default:
		LOG_INFO(TAG, "%s\n", pstMsg->stAttr.acCode);
		break;
	}
}

static void *DevManage_DownLoadFwThread(void *Args)
{
	int32_t iRet = -1;
	StationHandle *Station = (StationHandle *)(*((int32_t *)Args));
	IpcUpgradeFirmReq *Req = (IpcUpgradeFirmReq *)(Args + sizeof(int32_t));
	IpcFirmwareStatusReq FirmwareStatusReq = {NULL};
	char ProductId[48] = {0};
	char UpdatePath[128] = {0};
	
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());

	sleep(1);
	FirmwareStatusReq.objDevId = Req->objDevId;
	FirmwareStatusReq.otaId = Req->otaId;
	FirmwareStatusReq.statusType = 1;
	FirmwareStatusReq.status = 7;

	if(0){
		FirmwareStatusReq.status = 5;
		FirmwareStatusReq.msg = "low power";
		WoMqtt_SendGeneralReq(Station->DevManage->Mqtt, "UpgradeState", &FirmwareStatusReq);
	}

	if (Storage_IsReady(Station) == 0) {
		FirmwareStatusReq.status = 4;
		FirmwareStatusReq.msg = "have not sdcard";
		WoMqtt_SendGeneralReq(Station->DevManage->Mqtt, "UpgradeState", &FirmwareStatusReq);
	}

	if (FirmwareStatusReq.status == 7) {
		int32_t FileLen = 0;
		
		FileLen = WoHttp_GetLength(Req->url);
		if(FileLen <= 0){
			FirmwareStatusReq.status = 6;
			FirmwareStatusReq.msg = "url error";
			WoMqtt_SendGeneralReq(Station->DevManage->Mqtt, "UpgradeState", &FirmwareStatusReq);
		}
		else
		{
			FirmwareStatusReq.status = 7;
			FirmwareStatusReq.msg = "download fw";
			WoMqtt_SendGeneralReq(Station->DevManage->Mqtt, "UpgradeState", &FirmwareStatusReq);

			Profile_Read(Station, "Iot", "ProductId", ProductId);
			sprintf(UpdatePath,"/tmp/mnt/sdcard/update_%s.zip",ProductId);
			LOG_INFO(TAG, "UpdatePath=%s\n",UpdatePath);

			if(0){
				iRet = -1;
				LOG_INFO(TAG, "Power_IsPwnDown donot upgrade!\n",UpdatePath);
			}
			else{
				iRet = WoHttp_Download(Req->url, UpdatePath, FileLen);
				if (iRet == 0) {
					Profile_Write(Station, "OTA", "OtaId", Req->otaId);
					FirmwareStatusReq.statusType = 1;
					FirmwareStatusReq.status = 1;
					FirmwareStatusReq.msg = "upgrade fw";
					WoMqtt_SendGeneralReq(Station->DevManage->Mqtt, "UpgradeState", &FirmwareStatusReq);
				}
				else{
					char Cmd[128] = {0};
					sprintf(Cmd, "rm -rf %s", UpdatePath);
					system_call(Cmd, 0);
					LOG_ERROR(TAG, "http_download failed, rm %s\n",UpdatePath);
				}
			}
		}
			
		if(iRet != 0){
			FirmwareStatusReq.statusType = 1;
			FirmwareStatusReq.status = 3;
			FirmwareStatusReq.msg = "download fw failed";
			WoMqtt_SendGeneralReq(Station->DevManage->Mqtt, "UpgradeState", &FirmwareStatusReq);
		}
		
	}
	free(Args);
	
	pthread_exit(NULL);
}

static void *DevManage_FormatThread(void *Args)
{
	uint32_t Ret;
	StationHandle *Station = (StationHandle *)Args;
		
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());

	//Storage_Format(Station);

	Ret = Storage_IsReady(Station);
	DevManage_ReportEnum(Station->DevManage->Mqtt, WO_IOT_CMD_SD_STATUS, Ret);
	if (Ret) {
		uint32_t Totol, Free;
		char CapacityInfo[128];
		
		Ret = Storage_GetCapacity(Station, &Totol, &Free);
		if (Ret < 0) {
			Totol = 0;
			Free = 0;
		}
		sprintf(CapacityInfo, "{\"unit\": \"M\",\"total\": %d,\"current\": %d}", Totol, Totol-Free);
		DevManage_ReportString(Station->DevManage->Mqtt, WO_IOT_CMD_SD_CAPACITY, CapacityInfo);
	}

	pthread_exit(NULL);
}

static void *DevManage_UnbindThread(void *Args)
{
	StationHandle *Station = (StationHandle *)Args;
	
	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());
	
	sleep(1);
	CamManage_RemoveCamInfo(Station);
	//Profile_Write(Station, "Iot", "uid", "xx");
	System_Reboot(Station);

	pthread_exit(NULL);
}

static void DevManage_AttrMsgCallBack(void *Handle, DEV_MSG_ST *pstMsg)
{
	DevManageHandle *DevManage = (DevManageHandle *)Handle;

	DevManage_MsgPrint(pstMsg);
	
#if 0
	if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_INDICATOR)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Camera.Indicator;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_ANTI_FLICKER)) {
		pstMsg->stAttr.unValue.uiEnum = System->Setting->Camera.AntiFlicker;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_FLIP)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Camera.FlipMirror == 0 ? FALSE : TRUE;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_WATERMARK)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Camera.Stamp;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_POWERMODE)) {
		pstMsg->stAttr.unValue.uiEnum = 1;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PIR_SWITCH)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Detect.Enable;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PIR_SENSITIVITY)) {
		pstMsg->stAttr.unValue.iValue = System->Setting->Detect.Sense;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_SD_STATUS)) {
		uint32_t Totol, Free;
		
		Ret = Storage_GetCapacity(&Totol, &Free);
		pstMsg->stAttr.unValue.uiEnum = Ret < 0 ? 0 : 1;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_SD_CAPACITY)) {
		uint32_t Totol, Free;
		//cJSON *RootObj;
		
		Ret = Storage_GetCapacity(&Totol, &Free);
		if (Ret < 0) {
			Totol = 0;
			Free = 0;
		}
		//RootObj = cJSON_CreateObject();
		sprintf(pstMsg->stAttr.unValue.acStr, "{\"unit\": \"M\",\n\"total\": %d,\n\"current\": %d\n}", Totol, Totol - Free);
		/*Data = cJSON_PrintUnformatted(RootObj);
		if (NULL != Data) 
		{
			DevManage_ReportString(WO_IOT_CMD_DETECT_PLAN, Data);
		}
		cJSON_Delete(RootObj);*/
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_LIGHTING_SWITCH)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Camera.LightEnable;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_LIGHTING_SET)) {
		pstMsg->stAttr.unValue.uiEnum = System->Setting->Camera.LightLvl;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_ALARM_SWITCH)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Detect.AlarmEnable;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PERSON_ALARM_TYPE)) {
		pstMsg->stAttr.unValue.uiEnum = System->Setting->Detect.HumanAlarmMode;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_CAR_ALARM_TYPE)) {
		pstMsg->stAttr.unValue.uiEnum = System->Setting->Detect.VichleAlarmMode;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PET_ALARM_TYPE)) {
		pstMsg->stAttr.unValue.uiEnum = System->Setting->Detect.PetAlarmMode;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_MOVE_ALARM_TYPE)) {
		pstMsg->stAttr.unValue.uiEnum = System->Setting->Detect.MotionAlarmMode;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_SD_VIDEO_SWITCH)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Detect.RecordEnable;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_TIME)) {
		pstMsg->stAttr.unValue.iValue = System->Setting->Detect.RecordDuration;
	}
#ifdef C_JSON
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PLAN)) {
		UINT8 i;
		char Buf[64], *Data;
		char WDay[7][12] = {0};
		cJSON *RootObj, *Array;
		CONST char *Week[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
		SysSetting	*Setting;
		
		Setting = System->Setting;
		RootObj = cJSON_CreateObject();
		if (RootObj == NULL) {
			LOG_ERROR(TAG, "cJSON_CreateObject failed\n");
			return ;
		}
	
		Array = cJSON_CreateArray();
		for (i = 0; i < 7; i++) {
			if(Setting->Detect.WDay.Day & (1<<i)) {
				cJSON_AddItemToArray(Array, cJSON_CreateString(Week[i]));
			}
		}
		cJSON_AddItemToObject(RootObj, "week", Array);
		/*UINT8 Days;
		for (i = 0, Days = 0; i < 7; i++) {
			if(Setting->Detect.WDay.Day & (1<<i)) {
				strcpy(WDay[Days++], Week[i]);
			}
		}
		cJSON_AddItemToObject(RootObj, "week", cJSON_CreateStringArray((CONST char **)WDay, Days));*/
		sprintf(Buf, "%02d:%02d", Setting->Detect.StartHour, Setting->Detect.StartMin);
		cJSON_AddItemToObject(RootObj, "startTm", cJSON_CreateString(Buf));
		sprintf(Buf, "%02d:%02d", Setting->Detect.EndHour, Setting->Detect.EndMin);
		cJSON_AddItemToObject(RootObj, "endTm", cJSON_CreateString(Buf));
		cJSON_AddItemToObject(RootObj, "wholeDay", cJSON_CreateBool(Setting->Detect.WholeDay));
		
		Data = cJSON_PrintUnformatted(RootObj);
		if (NULL != Data) 
		{
			DevManage_ReportString(WO_IOT_CMD_DETECT_PLAN, Data);
		}
		cJSON_Delete(RootObj);
	}
#elif defined JSON_C
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PLAN)) {
		UINT8 i;
		char Buf[64], *Data;
		struct json_object *RootObj, *Array;
		CONST char *Week[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
		SysSetting	*Setting;
		
		Setting = System->Setting;
		RootObj = json_object_new_object();
		if (RootObj == NULL) {
			LOG_ERROR(TAG, "json_object_new_object failed\n");
			return ;
		}

		Array = json_object_new_array();
		for (i = 0; i < 7; i++) {
			if(Setting->Detect.WDay.Day & (1<<i)) {
				json_object_array_add(Array, json_object_new_string(Week[i]));
			}
		}
		json_object_object_add(RootObj, "week", Array);
		sprintf(Buf, "%02d:%02d", Setting->Detect.StartHour, Setting->Detect.StartMin);
		json_object_object_add(RootObj, "startTm", json_object_new_string(Buf));
		sprintf(Buf, "%02d:%02d", Setting->Detect.EndHour, Setting->Detect.EndMin);
		json_object_object_add(RootObj, "endTm", json_object_new_string(Buf));
		json_object_object_add(RootObj, "wholeDay", json_object_new_boolean(Setting->Detect.WholeDay ? true : false));
		
		Data = (char *)json_object_to_json_string(RootObj);
	    if (NULL != Data) 
	    {
	        DevManage_ReportString(WO_IOT_CMD_DETECT_PLAN, Data);
	    }
		json_object_put(RootObj);
	}

#endif
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PERSON_SWITCH)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Detect.HumanDet;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_CAR_SWITCH)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Detect.VichleDet;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PET_SWITCH)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Detect.PetDet;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_MARK)) {
		pstMsg->stAttr.unValue.bBool = System->Setting->Detect.Mark;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_VOLUME)) {
		pstMsg->stAttr.unValue.iValue = System->Setting->Camera.SpeakerVol;
	}
#endif
	pstMsg->stAttr.unValue.bBool = 0;
	WoMqtt_AttrResponse(DevManage->Mqtt, pstMsg);
}

static void DevManage_CmdMsgCallBack(void *Handle, DEV_MSG_ST *pstMsg)
{
	int32_t Ret;
	DevManageHandle *DevManage = (DevManageHandle *)Handle;

	DevManage_MsgPrint(pstMsg);

	Ret = 0;
	if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_INDICATOR)) {		
		//System->Setting->Camera.Indicator = pstMsg->stAttr.unValue.bBool;
		System_LedSet(DevManage->Station, LED_STA_RED, pstMsg->stAttr.unValue.bBool);
	}
	/*else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_ANTI_FLICKER)) {
		System->Setting->Camera.AntiFlicker = pstMsg->stAttr.unValue.uiEnum;
		Ret = Video_SetAntiFlicker(Station, pstMsg->stAttr.unValue.uiEnum);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_FLIP)) {
		System->Setting->Camera.FlipMirror = pstMsg->stAttr.unValue.bBool;
		Ret = Video_SetFlip(Station, pstMsg->stAttr.unValue.bBool);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_WATERMARK)) {
		System->Setting->Camera.Stamp = pstMsg->stAttr.unValue.bBool;
		Ret = Video_SetStampState(Station, pstMsg->stAttr.unValue.bBool);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_WATERMARK_FMT)) {
		System->Setting->Camera.StampFmt = pstMsg->stAttr.unValue.uiEnum;
		Ret = Video_SetStampFormat(Station, pstMsg->stAttr.unValue.uiEnum);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_INFRARED_OR_FULL_COLOR_MODE)) {
		System->Setting->Camera.NightMode = pstMsg->stAttr.unValue.uiEnum;
		Ret = Video_SetNightMode(Station, pstMsg->stAttr.unValue.uiEnum);
	}*/
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_SD_FORMAT)) {
		pthread_t FormatThd;

		if (pthread_create(&FormatThd, NULL, DevManage_FormatThread, DevManage->Station) != 0) {
			LOG_ERROR(TAG, "pthread_create FormatThread failed\n");
			Ret = 1;
		}
		else {
			Ret = 0;
		}
	}
	/*else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PIR_SWITCH)) {
		System->Setting->Detect.Enable = pstMsg->stAttr.unValue.bBool;
		Detection_SaveTagParm(Station);
		Ret = MsgMgr_SendMsgsEx(Station, MSG_POWER_SET_DET_ENABLE, &pstMsg->stAttr.unValue.bBool, sizeof(pstMsg->stAttr.unValue.bBool));
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PIR_SENSITIVITY)) {
		System->Setting->Detect.Sense = pstMsg->stAttr.unValue.iValue;
		Ret = MsgMgr_SendMsgsEx(Station, MSG_POWER_SET_DET_SENSE, &pstMsg->stAttr.unValue.iValue, sizeof(pstMsg->stAttr.unValue.iValue));
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_LIGHTING_SWITCH)) {
		System->Setting->Camera.LightEnable = pstMsg->stAttr.unValue.bBool;
		Ret = Video_SetNightMode(Station, pstMsg->stAttr.unValue.bBool ? Station->System->Setting->Camera.NightMode : 2);
		//Ret = Video_SetLightLevel(Station, System->Setting->Camera.LightEnable ? Station->System->Setting->Camera.LightLvl : 0);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_LIGHTING_SET)) {
		System->Setting->Camera.LightLvl = pstMsg->stAttr.unValue.uiEnum + 1;
		Video_SetLightLevel(Station, System->Setting->Camera.LightLvl);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_ALARM_SWITCH)) {
		System->Setting->Detect.AlarmEnable = pstMsg->stAttr.unValue.bBool;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PERSON_ALARM_TYPE)) {
		System->Setting->Detect.HumanAlarmMode = pstMsg->stAttr.unValue.uiEnum;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_CAR_ALARM_TYPE)) {
		System->Setting->Detect.VichleAlarmMode = pstMsg->stAttr.unValue.uiEnum;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_PET_ALARM_TYPE)) {
		System->Setting->Detect.PetAlarmMode = pstMsg->stAttr.unValue.uiEnum;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_MOVE_ALARM_TYPE)) {
		System->Setting->Detect.MotionAlarmMode = pstMsg->stAttr.unValue.uiEnum;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_SD_VIDEO_SWITCH)) {
		System->Setting->Detect.RecordEnable = pstMsg->stAttr.unValue.bBool;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_TIME)) {
		System->Setting->Detect.RecordDuration = pstMsg->stAttr.unValue.iValue;
	}
#ifdef C_JSON
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PLAN)) {
		//char *Data;
		cJSON *RootObj, *SubObj;
		SysSetting	*Setting;
		
		Setting = System->Setting;
		RootObj = cJSON_Parse((CONST char* )pstMsg->stAttr.unValue.acStr);
		if (RootObj == NULL) {
			LOG_ERROR(TAG, "cJSON_Parse failed\n");
			return ;
		}

		//Data = cJSON_Print(RootObj);
		SubObj = cJSON_GetObjectItem(RootObj, "week");
		if (SubObj) {
			UINT8 i, j, Days;
			CONST char *Week[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
		
			Days = cJSON_GetArraySize(SubObj);
			Setting->Detect.WDay.Day = 0;
			for (i = 0; i < Days; i++) {
				cJSON *items = cJSON_GetArrayItem(SubObj, i);
				for (j = 0; j < 7; j++) {
					if (strstr(items->valuestring, Week[j])) {
						Setting->Detect.WDay.Day |= (1<<j);
						break;
					}
				}
			}
		}
		
		SubObj = cJSON_GetObjectItem(RootObj, "startTm");
		if (SubObj) {
			int32_t StartH, StartM;
			
			sscanf(SubObj->valuestring, "\"%02d:%02d\"", &StartH, &StartM);
			Setting->Detect.StartHour = StartH;
			Setting->Detect.StartMin = StartM;
		}
		SubObj = cJSON_GetObjectItem(RootObj, "endTm");
		if (SubObj) {
			int32_t EndH, EndM;
			
			sscanf(SubObj->valuestring, "\"%02d:%02d\"", &EndH, &EndM);
			Setting->Detect.EndHour = EndH;
			Setting->Detect.EndMin = EndM;
		}
		SubObj = cJSON_GetObjectItem(RootObj, "wholeDay");
		if (SubObj) {
			Setting->Detect.WholeDay = SubObj->valueint;
		}
		cJSON_Delete(RootObj);
	}
#elif defined JSON_C
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PLAN)) {
		//char *Data;
		struct json_object *RootObj, *SubObj;
		SysSetting	*Setting;
		
		Setting = System->Setting;
		RootObj = json_tokener_parse((CONST char* )pstMsg->stAttr.unValue.acStr);
		if (RootObj == NULL) {
			LOG_ERROR(TAG, "json_tokener_parse failed\n");
			return ;
		}

		//Data = cJSON_Print(RootObj);
		SubObj = json_object_object_get(RootObj, "week");
		if (SubObj) {
			UINT8 i, j, Days;
			CONST char *Week[] = {"Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"};
		
			Days = json_object_array_length(SubObj);
			Setting->Detect.WDay.Day = 0;
			for (i = 0; i < Days; i++) {
				struct json_object *items = json_object_array_get_idx(SubObj, i);
				for (j = 0; j < 7; j++) {
					if (strstr(json_object_to_json_string(items), Week[j])) {
						Setting->Detect.WDay.Day |= (1<<j);
						break;
					}
				}
			}
		}
		
		SubObj = json_object_object_get(RootObj, "startTm");
		if (SubObj) {
			int32_t StartH, StartM;
			
			sscanf(json_object_to_json_string(SubObj), "\"%02d:%02d\"", &StartH, &StartM);
			Setting->Detect.StartHour = StartH;
			Setting->Detect.StartMin = StartM;
		}
		SubObj = json_object_object_get(RootObj, "endTm");
		if (SubObj) {
			int32_t EndH, EndM;
			
			sscanf(json_object_to_json_string(SubObj), "\"%02d:%02d\"", &EndH, &EndM);
			Setting->Detect.EndHour = EndH;
			Setting->Detect.EndMin = EndM;
		}
		SubObj = json_object_object_get(RootObj, "wholeDay");
		if (SubObj) {
			Setting->Detect.WholeDay = json_object_get_boolean(SubObj);
		}
		json_object_put(RootObj);
	}
#endif
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_MOVE_SWITCH)) {
		System->Setting->Detect.MotionDet = pstMsg->stAttr.unValue.bBool;
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PERSON_SWITCH)) {
		System->Setting->Detect.HumanDet = pstMsg->stAttr.unValue.bBool;
		Detection_SaveTagParm(Station);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_CAR_SWITCH)) {
		System->Setting->Detect.VichleDet = pstMsg->stAttr.unValue.bBool;
		Detection_SaveTagParm(Station);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_PET_SWITCH)) {
		System->Setting->Detect.PetDet = pstMsg->stAttr.unValue.bBool;
		Detection_SaveTagParm(Station);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_DETECT_MARK)) {
		System->Setting->Detect.Mark = pstMsg->stAttr.unValue.bBool;
		Detection_SaveTagParm(Station);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_SPEAKER_SWITCH)) {
		System->Setting->Camera.Speaker = pstMsg->stAttr.unValue.bBool;
		Audio_SetVolMute(Station, pstMsg->stAttr.unValue.bBool ? 0 : 1);
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_VOLUME)) {
		System->Setting->Camera.SpeakerVol = pstMsg->stAttr.unValue.iValue;
		Audio_SetVol(Station, pstMsg->stAttr.unValue.iValue);
		if (System->Setting->Camera.SpeakerVol && !System->Setting->Camera.Speaker) {
			System->Setting->Camera.Speaker = 1;
			Audio_SetVolMute(Station, 0);
		}
		else if (!System->Setting->Camera.SpeakerVol && System->Setting->Camera.Speaker) {
			System->Setting->Camera.Speaker = 0;
			Audio_SetVolMute(Station, 1);
		}
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_MICROPHONE_SWITCH)) {
		System->Setting->Camera.Microphone = pstMsg->stAttr.unValue.bBool;
		if (pstMsg->stAttr.unValue.bBool) {
			Audio_Stop(Station);
		}
		else {
			Audio_Start(Station);
		}
	}
	else if(!strcmp(pstMsg->stAttr.acCode, WO_IOT_CMD_BATTERY_ALARM)) {
		System->Setting->Camera.PowerAlarmThred = pstMsg->stAttr.unValue.iValue;
	}*/
	else {
		LOG_INFO(TAG, "%s: %s\n", __func__, pstMsg->stAttr.acCode);
	}
	
	//System_WriteSetting(System);
	if (pstMsg->bResult < 0) {
		return ;
	}
	WoMqtt_CmdResponse(DevManage->Mqtt, pstMsg, Ret == 0 ? 1 : 0);
}

static void DevManage_AssignNetCallBack(void *Handle, DEV_MSG_ST *pstMsg)
{
	LOG_INFO(TAG, "%d\n", pstMsg->bResult);

	//gLongzy->AssignNetRes = pstMsg->bResult;
}

static void DevManage_GeneralReqCallBack(void *Handle, GEN_MSG_ST *GenMsg, void *ReqData)
{
	char RespBuf[64];
	DevManageHandle *DevManage = (DevManageHandle *)Handle;
	StationHandle *Station = DevManage->Station;
	
	LOG_INFO(TAG, "%s\n", GenMsg->acType);
	if (!strcmp(GenMsg->acType, "AppUnbind") || !strcmp(GenMsg->acType, "HistoryUnbind")) {
		char Uuid[48];
		pthread_t UnbindThd;
		IpcUnbindReq *Req = (IpcUnbindReq *)ReqData;
		IpcUnbindResp *Resp = (IpcUnbindResp *)RespBuf;
		
		memset(Uuid, 0, sizeof(Uuid));
		Profile_Read(Station, "Iot", "uid", Uuid);

		Resp->result = 1;
		Resp->objDevId = Req->objDevId;
		Resp->uid = Req->uid;
		if (strcmp(Uuid, Req->uid)) {
			LOG_ERROR(TAG, "uid is error\n");
			Resp->result = 0;
		}
		else if (pthread_create(&UnbindThd, NULL, DevManage_UnbindThread, Station) != 0) {
			LOG_ERROR(TAG, "pthread_create failed\n");
			Resp->result = 0;
		}
	}
	else if (!strcmp(GenMsg->acType, "Reboot")) {
		IpcRebootReq *Req = (IpcRebootReq *)ReqData;
		IpcRebootResp *Resp = (IpcRebootResp *)RespBuf;

		Resp->result = 1;
		Resp->objDevId = Req->objDevId;
	}
	else if (!strcmp(GenMsg->acType, "UpgradeFirm")) {
		IpcUpgradeFirmReq *Req = (IpcUpgradeFirmReq *)ReqData;
		IpcUpgradeFirmResp *Resp = (IpcUpgradeFirmResp *)RespBuf;
		pthread_t DownLoadFwThd;
		char *ReqBuf;

		ReqBuf = (char *)malloc(2048);
		if (ReqBuf == NULL) {
			return ;
		}
		
		IpcUpgradeFirmReq *ReqBak = (IpcUpgradeFirmReq *)(ReqBuf + sizeof(int32_t));
		
		memset(ReqBuf, 0, sizeof(2048));
		*((int32_t *)ReqBuf) = (int32_t)Station;
		ReqBak->objDevId = ReqBuf + sizeof(int32_t) + sizeof(IpcUpgradeFirmReq);
		strcpy(ReqBak->objDevId, Req->objDevId);
		
		ReqBak->otaId = ReqBak->objDevId + strlen(Req->objDevId) + 1;
		strcpy(ReqBak->otaId, Req->otaId);
		
		ReqBak->url = ReqBak->otaId + strlen(Req->otaId) + 1;
		strcpy(ReqBak->url, Req->url);

		ReqBak->type = Req->type;

		ReqBak->version = ReqBak->url + strlen(Req->url) + 1;
		strcpy(ReqBak->version, Req->version);

		LOG_INFO(TAG,"UpgradeFirm objDevId=%s otaId=%s url=%s type=%d version=%s\n", ReqBak->objDevId, ReqBak->otaId, ReqBak->url, ReqBak->type, ReqBak->version);
		if (pthread_create(&DownLoadFwThd, NULL, DevManage_DownLoadFwThread, ReqBuf) != 0) {
			LOG_ERROR(TAG, "pthread_create DownLoadFw failed\n");
			Resp->result = 1;
		}
		else {
			Resp->result = 0;
		}
		Resp->objDevId = Req->objDevId;
	}
	else if (!strcmp(GenMsg->acType, "StartPlay")) {
		//IpcDevIdReq *Req = (IpcDevIdReq *)ReqData;
		if(Station->P2p != NULL){
			Station->P2p->ListenTimeout = 0;
		}
		return ;
	}
	
	else if(!strcmp(GenMsg->acType, "CloudPackage")){
		LOG_INFO(TAG, "recv CloudPackage\n");
		IpcCloudPackageBo *Req = (IpcCloudPackageBo *)ReqData;
		IpcComResp *Resp = (IpcComResp *)RespBuf; 
		
		LOG_INFO(TAG,"objDevId=%s,status=%d,dayType=%d,startime=%lld,endTime=%lld\n", Req->objDevId,Req->status ,Req->dayType, Req->startTime, Req->endTime);
		LOG_INFO(TAG,"id=%s, packageId=%s,accesskey=%s,secretkey=%s,Region=%s,endpoint=%s\n" , Req->id, Req->packageId, Req->accessKey, Req->secretKey, Req->region,Req->endpoint);
		LOG_INFO(TAG,"bucket=%s,ossType=%s,imagebucket=%s\n" , Req->bucket,Req->ossType, Req->imageBucket );

		char DeviceId[64];
		int32_t Ret = Profile_Read(Station, "Iot", "deviceId", DeviceId);
		if(Ret != 0) {
			LOG_ERROR(TAG, "Profile_Read DeviceId failed\n");
		}

		if(!strcmp(Req->objDevId, DeviceId) && Req->status == 1)
		{
			#if 1
			char Value[128];
			memset(Value,0x00,sizeof(Value));
			sprintf(Value, "%lld", Req->startTime);
			Profile_Write(Station, "CLOUDSTORAGE", "starTime", Value);

			memset(Value,0x00,sizeof(Value));
			sprintf(Value, "%lld", Req->endTime);
			Profile_Write(Station, "CLOUDSTORAGE", "endTime", Value);

			memset(Value,0x00,sizeof(Value));
			sprintf(Value, "%d", Req->dayType);
			Profile_Write(Station, "CLOUDSTORAGE", "dayType", Value);

			Profile_Write(Station, "CLOUDSTORAGE", "S3status", "1");
			
			Profile_Write(Station, "CLOUDSTORAGE", "accessKey", Req->accessKey);
			Profile_Write(Station, "CLOUDSTORAGE", "secretKey", Req->secretKey);
			Profile_Write(Station, "CLOUDSTORAGE", "region", Req->region);
			Profile_Write(Station, "CLOUDSTORAGE", "endpoint", Req->endpoint);
			Profile_Write(Station, "CLOUDSTORAGE", "ossType", Req->ossType);
			Profile_Write(Station, "CLOUDSTORAGE", "bucket", Req->bucket);
			if(strlen(Req->imageBucket) > 0)
			{
				Profile_Write(Station, "CLOUDSTORAGE", "imageBucket", Req->imageBucket);
				Profile_Write(Station, "CLOUDSTORAGE", "imageRegion", Req->region);
				Profile_Write(Station, "CLOUDSTORAGE", "imageEndpoint", Req->endpoint);
			}
			#endif
		}
		else
		{
			LOG_INFO(TAG, "CLOUDSTORAGE objDevId=%s status=%d\n",Req->objDevId,Req->status);
		}

		Resp->result = 1;
		Resp->objDevId = Req->objDevId;
		Resp->msg = "CloudPackage";
	}
	else if(!strcmp(GenMsg->acType, "StopCloudPackage")){
		LOG_INFO(TAG, "recv StopCloudPackage\n");
		IpcStopCloudPackageBo *Req = (IpcStopCloudPackageBo *)ReqData;
		IpcComResp *Resp = (IpcComResp *)RespBuf;
		
		LOG_INFO(TAG,"objDevId=%s,id=%s,packageId=%s,status=%d\n",Req->objDevId,Req->id,Req->packageId,Req->status);
		
		char DeviceId[64];
		int32_t Ret = Profile_Read(Station, "Iot", "deviceId", DeviceId);
		if(Ret != 0) {
			LOG_ERROR(TAG, "Profile_Read DeviceId failed\n");
		}

		if(!strcmp(Req->objDevId, DeviceId))
		{
			Profile_Write(Station, "CLOUDSTORAGE", "S3status", "0");
			Profile_Write(Station, "CLOUDSTORAGE", "starTime", "0");
			Profile_Write(Station, "CLOUDSTORAGE", "endTime", "0");
		}
		Resp->result = 1;
		Resp->objDevId = Req->objDevId;
		Resp->msg = "StopCloudPackage";
	}
	else if(!strcmp(GenMsg->acType, "StoreBucket")){
		LOG_INFO(TAG, "recv StoreBucket\n");
		IpcCloudStoreBucketType *Req = (IpcCloudStoreBucketType *)ReqData;
		IpcComResp *Resp = (IpcComResp *)RespBuf;
		
		LOG_INFO(TAG,"Devld=%s,DataType=%s,OssType=%s,AcessKey=%s,Secretkey=%s,endpoint=%s,region=%s,bucket=%s\n",Req->objDevId, Req->dataType, Req->ossType, Req->accessKey, Req->secretKey, Req->endpoint, Req->region, Req->bucket);
		char DeviceId[64];
		
		int32_t Ret = Profile_Read(Station, "Iot", "DeviceId", DeviceId);
		if(Ret != 0) {
			LOG_ERROR(TAG, "Profile_Read DeviceId failed\n");
		}

		if(!strcmp(Req->objDevId, DeviceId))
		{
			Profile_Write(Station, "CLOUDSTORAGE", "accessKey", Req->accessKey);
			Profile_Write(Station, "CLOUDSTORAGE", "secretKey", Req->secretKey);
			Profile_Write(Station, "CLOUDSTORAGE", "imageRegion", Req->region);
			Profile_Write(Station, "CLOUDSTORAGE", "imageEndpoint", Req->endpoint);
			Profile_Write(Station, "CLOUDSTORAGE", "imageBucket", Req->bucket);
		}

		Resp->result = 1;
		Resp->objDevId = Req->objDevId;
		Resp->msg = "StoreBucket";
	}
	
	WoMqtt_GeneralResponse(DevManage->Mqtt, GenMsg, RespBuf);

	/*if (!strcmp(GenMsg->acType, "Reboot")) {
		WoMqtt_SendKeeplive(0);
		//MsgMgr_SendMsgsEx(Station, MSG_POWER_RESET, "", 0);
	}*/
}

static void DevManage_GeneralRespCallBack(void *Handle, GEN_MSG_ST *GenMsg, void *RespData)
{
	if (!strcmp(GenMsg->acType, "DevUnbind")) {
	}
	else if (!strcmp(GenMsg->acType, "ShadowGet")) {
		cJSON *RootObj, *SubObj;

		if (strlen((char* )RespData) == 0) {
			LOG_WARN(TAG, "ShadowGet do not get any data\n");
			return ;
		}
		//printf("%s\n", (char* )RespData);
		RootObj = cJSON_Parse((const char* )RespData);
		if (RootObj == NULL) {
			LOG_ERROR(TAG, "cJSON_Parse failed\n");
			return ;
		}
		SubObj = cJSON_GetObjectItem(RootObj, "list");
		if (SubObj) {
			cJSON *Items;

			SubObj = SubObj->child;
			while(SubObj != NULL) {
				DEV_MSG_ST stMsg = {0};

				stMsg.bResult = -1;
				Items = cJSON_GetObjectItem(SubObj, "code");
				if (Items) {
					strcpy(stMsg.stAttr.acCode, Items->valuestring);
					//printf("%s： code=%s ", __func__, Items->valuestring);
				}
				Items = cJSON_GetObjectItem(SubObj, "type");
				if (Items) {
					stMsg.stAttr.enType = Items->valueint;
					//printf("type=%d ", Items->valueint);
				}
				Items = cJSON_GetObjectItem(SubObj, "value");
				if (Items) {
					//printf("value=%s\n", Items->valuestring);
					switch(stMsg.stAttr.enType) {
					case MSG_TYPE_BOOLEAN:
						stMsg.stAttr.unValue.bBool = atoi(Items->valuestring);
						break;
					case MSG_TYPE_INTEGER:
						stMsg.stAttr.unValue.iValue = atoi(Items->valuestring);
						break;
					case MSG_TYPE_ENUM:
						stMsg.stAttr.unValue.uiEnum = atoi(Items->valuestring);
						break;
					case MSG_TYPE_JSON:
					case MSG_TYPE_STRING:
						strncpy(stMsg.stAttr.unValue.acStr, Items->valuestring, sizeof(stMsg.stAttr.unValue.acStr) - 1);
						break;
					default:
						break;
					}
				}
				DevManage_CmdMsgCallBack(Handle, &stMsg);
				SubObj = SubObj->next;
			}
		}
		cJSON_Delete(RootObj);
	}
	else if (!strcmp(GenMsg->acType, "CheckCloudPackage")) {
		LOG_INFO(TAG, "GenMsg->bResult=%d\n",GenMsg->bResult);
	}

}

static void DevManage_NtpSyncCb(void *Handle, int32_t TimeStamp)
{
	DevManageHandle *DevManage = (DevManageHandle *)Handle;
	StationHandle *Station = DevManage->Station;

	if (TimeStamp > 0) {
		Station->System->DateTimeSyncState = 2;
		System_SetTime(Station, TimeStamp);
	}
}

static void *DevManage_LoginThread(void *Args)
{
	DevManageHandle *DevManage;
	WoMqtt *Mqtt = NULL;
	int32_t Ret;
	int32_t Timeout;

	prctl(PR_SET_NAME, (unsigned long)__FUNCTION__);
	pthread_detach(pthread_self());
	
	DevManage = (DevManageHandle *)Args;

	Mqtt = (WoMqtt *)malloc(sizeof(WoMqtt) + sizeof(DevManageHandle *));
	if (Mqtt == NULL) {
		LOG_ERROR(TAG, "malloc Mqtt failed\n");
		goto DevManage_LoginThread_Error;
	}
	
	DevManage->Mqtt = Mqtt;
	memset(Mqtt, 0, sizeof(WoMqtt));
	Mqtt->Private[0] = DevManage;
	Ret = Profile_Read(DevManage->Station, "Iot", "productId", Mqtt->acProductId);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read productId failed\n");
		goto DevManage_LoginThread_Error;
	}
	
	Ret = Profile_Read(DevManage->Station, "Iot", "uid", Mqtt->acUid);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read uid failed\n");
		goto DevManage_LoginThread_Error;
	}
    
	Ret = Profile_Read(DevManage->Station, "Iot", "deviceId", Mqtt->acDeviceId);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read deviceId failed\n");
		goto DevManage_LoginThread_Error;
	}
	
	Ret = Profile_Read(DevManage->Station, "Iot", "homeId", Mqtt->acHomeId);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read homeId failed\n");
		goto DevManage_LoginThread_Error;
	}
	
	Ret = Profile_Read(DevManage->Station, "Iot", "userName", Mqtt->acUserName);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read userName failed\n");
		goto DevManage_LoginThread_Error;
	}
	
	Ret = Profile_Read(DevManage->Station, "Iot", "password", Mqtt->acPassword);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read password failed\n");
		goto DevManage_LoginThread_Error;
	}
	
	Ret = Profile_Read(DevManage->Station, "Iot", "serverAddr", Mqtt->acServerIp);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read serverAddr failed\n");
		goto DevManage_LoginThread_Error;
	}

	char Value[16] = {0};
	Ret = Profile_Read(DevManage->Station, "Iot", "serverPort", Value);
	if(Ret != 0) {
		LOG_ERROR(TAG, "Profile_Read serverPort failed\n");
		goto DevManage_LoginThread_Error;
	}
	Mqtt->iServerPort = atoi(Value);
	
	Mqtt->pfnNtpTimeCb = DevManage_NtpSyncCb;
	Mqtt->pfnCmdMsgCb = DevManage_CmdMsgCallBack;
	Mqtt->pfnAttrMsgCb = DevManage_AttrMsgCallBack;
	Mqtt->pfnAssignNetCb = DevManage_AssignNetCallBack,
	Mqtt->pfnGeneralReqCb = DevManage_GeneralReqCallBack;
	Mqtt->pfnGeneralResCb = DevManage_GeneralRespCallBack;
    LOG_INFO(TAG, "acUid: %s\n", Mqtt->acUid);
    LOG_INFO(TAG, "acDeviceId: %s\n", Mqtt->acDeviceId);
    LOG_INFO(TAG, "acProductId: %s\n", Mqtt->acProductId);
    LOG_INFO(TAG, "acHomeId: %s\n", Mqtt->acHomeId);
    LOG_INFO(TAG, "acUserName: %s\n", Mqtt->acUserName);
    LOG_INFO(TAG, "acPassword: %s\n", Mqtt->acPassword);
    LOG_INFO(TAG, "acServerIp: %s\n", Mqtt->acServerIp);
    LOG_INFO(TAG, "iServerPort: %d\n", Mqtt->iServerPort);

	Timeout = 30;
	while (!Network_IsReady(DevManage->Station)) {
		if (Timeout-- <= 0) {
			LOG_ERROR(TAG, "Network is not ready\n");
			goto DevManage_LoginThread_Error;
		}
		sleep(1);
	}

	Ret = WoMqtt_Init(Mqtt);
	if (Ret != 0) {
		goto DevManage_LoginThread_Error;
	}
	Ret = WoMqtt_Connect(Mqtt);
	if (Ret != 0) {
		goto DevManage_LoginThread_Error;
	}
	
	WoMqtt_SendNtpSync(Mqtt);
	DevManage->LoginThd = 0;
	
	pthread_exit(NULL);

DevManage_LoginThread_Error:
	if (Mqtt) {
		free(Mqtt);
	}
	DevManage->Mqtt = NULL;
	DevManage->LoginThd = 0;

	pthread_exit(NULL);
}

int32_t DevManage_Init(StationHandle *Station)
{
	int32_t Ret;
	DevManageHandle *DevManage;

	DevManage = calloc(1, sizeof(DevManageHandle));
	if (DevManage == NULL) {
		LOG_ERROR(TAG, "calloc DevManageHandle failed\n");
		return -1;
	}

	Station->DevManage = DevManage;
	DevManage->Station = Station;
	Ret = pthread_create(&DevManage->LoginThd, NULL, DevManage_LoginThread, DevManage);
	if(Ret != 0) {
		LOG_ERROR(TAG, "pthread_create iot login failed\n");
		goto DevManage_Init_Error;
	}
	
	return 0;

DevManage_Init_Error:
	free(DevManage);
	Station->DevManage = NULL;
	
	return -1;
}

void DevManage_Deinit(StationHandle *Station)
{
	DevManageHandle *DevManage = Station->DevManage;

	if (DevManage) {
		if (DevManage->LoginThd > 0) {
			pthread_cancel(DevManage->LoginThd);
		}

		if (DevManage->Mqtt) {
			WoMqtt_Exit(DevManage->Mqtt);
			free(DevManage->Mqtt);
			DevManage->Mqtt = NULL;
		}
		
		free(DevManage);
		
		Station->DevManage = NULL;
	}
}

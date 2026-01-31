#ifndef __LOG_H__
#define __LOG_H__

#include <stddef.h>
#include <stdint.h>
//#define       IMP_LOG_API

enum {
        LOG_LVL_DEBUG,
        LOG_LVL_INFO,
        LOG_LVL_WARN,
        LOG_LVL_ERROR,
};

#define LOG_LVL LOG_LVL_DEBUG
#if 0
#define LOG_FUNC(LVL, tag, fmt, ...) {\
        if(LVL >= LOG_LVL) { \
                fprintf(stderr, "[" tag "]" fmt, ##__VA_ARGS__); \
        }\
}

#define LOG_FUNC_DEBUG(LVL, tag, fmt, ...) {\
        if(LVL >= LOG_LVL) { \
                fprintf(stderr, "[" tag "/DEBUG]" fmt, ##__VA_ARGS__); \
        }\
}


#define LOG_FUNC_INFO(LVL,tag, fmt, ...) {\
        if(LVL >= LOG_LVL) { \
                fprintf(stderr, "[" tag "/INFO]" fmt, ##__VA_ARGS__); \
        }\
}

#define LOG_FUNC_WARN(LVL, tag, fmt, ...) {\
        if(LVL >= LOG_LVL) { \
                fprintf(stderr, "[" tag "/WARN]" fmt, ##__VA_ARGS__); \
        }\
}

#define LOG_FUNC_ERROR(LVL, tag, fmt, ...) {\
        if(LVL >= LOG_LVL) { \
                fprintf(stderr, "[" tag "/ERROR]" fmt, ##__VA_ARGS__); \
        }\
}


#define LOG_DEBUG(tag, fmt, ...)  LOG_FUNC_DEBUG(LOG_LVL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)   LOG_FUNC_INFO(LOG_LVL_INFO, tag, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)   LOG_FUNC_WARN(LOG_LVL_WARN, tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...)  LOG_FUNC_ERROR(LOG_LVL_ERROR, tag, fmt, ##__VA_ARGS__)
#endif

void Log_Print(int32_t Level, const char *Tag, const char *Func, const int32_t Line, const char *Fmt, ...);

#ifdef IMP_LOG_API
#include "imp/imp_log.h"

#define LOG_DEBUG(tag, fmt, ...)  IMP_LOG(tag, IMP_LOG_LEVEL_DEBUG, IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)   IMP_LOG(tag, IMP_LOG_LEVEL_INFO, IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)   IMP_LOG(tag, IMP_LOG_LEVEL_WARN, IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...)  IMP_LOG(tag, IMP_LOG_LEVEL_ERROR, IMP_LOG_GET_OPTION, fmt, ##__VA_ARGS__)

#else

void Log_Print(int32_t Level, const char *Tag, const char *Func, const int32_t Line, const char *Fmt, ...);

#define LOG_DEBUG(tag, fmt, ...)  Log_Print(LOG_LVL_DEBUG, tag, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(tag, fmt, ...)   Log_Print(LOG_LVL_INFO, tag, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARN(tag, fmt, ...)   Log_Print(LOG_LVL_WARN, tag, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...)  Log_Print(LOG_LVL_ERROR, tag, __FUNCTION__, __LINE__, fmt, ##__VA_ARGS__)
#endif

#endif

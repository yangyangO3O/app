#ifndef __SYSTEM_CALL_H__
#define __SYSTEM_CALL_H__

/**
 * @brief system_call: 跨进程的system调用，解决system引起的句柄异常隐患
 *
 * @param[in] cmd: 需要执行的命令
 * @param[in] timeout_ms: 超时时间，单位ms, 输入-1/0 时，使用默认时间 10 秒
 *
 * @return: 返回cmd执行后的结果，小于0 失败
 *
 * @attention: 调用该API之前，先启动system_call_daemon守护进程，并先调用 system_call_init API
 */
int system_call(char *cmd, int timeout_ms);

/**
 * @brief popen_call: 跨进程的popen调用，解决popen引起的句柄异常隐患
 *
 * @param[in] cmd: 需要执行的命令
 * @param[out] out: popen返回的内容
 * @param[in] max_size: 用来存放popen返回内容的buffer最大值
 * @param[in] timeout_ms: 超时时间，单位ms, 输入-1/0 时，使用默认时间 10 秒
 *
 * @return: 返回cmd执行后的结果，小于0 失败
 *
 * @attention: 调用该API之前，先启动system_call_daemon守护进程，并先调用 system_call_init API
 */
int popen_call(char *cmd, char *out, int max_size, int timeout_ms);

/**
 * @brief system_call_init: 初始化system_call功能

 * @return: 小于0 失败
 *
 * @attention: 调用该API之前，先启动system_call_daemon守护进程
 */
int system_call_init(void);

/**
 * @brief system_call_exit: 退出system_call功能

 * @return: 小于0 失败
 *
 * @attention:
 */
int system_call_exit(void);

#endif // __SYSTEM_CALL_H__

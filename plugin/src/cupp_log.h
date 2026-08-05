/* 修改内容：5.8.x 兼容版：用 DBG1/DBG2 替代 LOG_* 宏（5.8 无 LOG 宏） 修改人：pengjunlin 时间：2026-08-05 00:00:00 -- start ---- */
#ifndef CUPP_LOG_H_
#define CUPP_LOG_H_

/*
 * 统一日志入口，走 strongSwan 的 DBG 通道，前缀 [CUPP]。
 * 5.8.x 只有 DBG0-4 宏，没有 LOG_INFO/LOG_WARN/LOG_ERR。
 * DBG1 = info 级，DBG2 = debug 级。
 */
#include <library.h>
#include <utils/debug.h>

#define CUPP_TAG "[CUPP] "

/* 调试日志（DBG2 = debug 级别，仅当 charon.debug 等级足够时输出） */
#define cupp_dbg(fmt, ...) \
	DBG2(DBG_NET, CUPP_TAG fmt, ##__VA_ARGS__)

/* 常规信息 / 警告 / 错误：5.8 统一用 DBG1（info 级别） */
#define cupp_log(fmt, ...) \
	DBG1(DBG_NET, CUPP_TAG fmt, ##__VA_ARGS__)

#define cupp_warn(fmt, ...) \
	DBG1(DBG_NET, CUPP_TAG fmt, ##__VA_ARGS__)

#define cupp_err(fmt, ...) \
	DBG1(DBG_NET, CUPP_TAG fmt, ##__VA_ARGS__)

#endif /* CUPP_LOG_H_ */
/* 修改内容：5.8.x 兼容版：用 DBG1/DBG2 替代 LOG_* 宏（5.8 无 LOG 宏） 修改人：pengjunlin 时间：2026-08-05 00:00:00 -- end ---- */

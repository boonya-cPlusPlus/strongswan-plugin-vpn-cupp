/* 修改内容：创建统一日志宏 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef CUPP_LOG_H_
#define CUPP_LOG_H_

/*
 * 统一日志入口，走 strongSwan 的 DBG/LOG 通道，前缀 [CUPP]。
 * 依赖 strongswan.h 提供的 DBG1/LOG/Daemon 等。
 */
#include <strongswan.h>

#define CUPP_TAG "[CUPP] "

/* 调试日志（仅当 charon.debug 等级足够时输出） */
#define cupp_dbg(level, fmt, ...) \
	DBG(DBG_NET, dbg(DBG_NET, CUPP_TAG fmt, ##__VA_ARGS__))

/* 常规信息日志 */
#define cupp_log(fmt, ...) \
	LOG(LOG_INFO, CUPP_TAG fmt, ##__VA_ARGS__)

/* 警告日志 */
#define cupp_warn(fmt, ...) \
	LOG(LOG_WARNING, CUPP_TAG fmt, ##__VA_ARGS__)

/* 错误日志 */
#define cupp_err(fmt, ...) \
	LOG(LOG_ERR, CUPP_TAG fmt, ##__VA_ARGS__)

#endif /* CUPP_LOG_H_ */
/* 修改内容：创建统一日志宏 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

/* 修改内容：创建策略引擎接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef POLICY_ENGINE_H_
#define POLICY_ENGINE_H_

#include <library.h>
#include <networking/host.h>
#include "fixed_pool.h"

typedef struct policy_engine_t policy_engine_t;

/**
 * 用户策略引擎：加载 policy.yaml，提供固定 IP 查询与动态池端点查询。
 * 支持热重载（双 buffer 原子切换，读无锁）。
 */
struct policy_engine_t {
	/**
	 * 查询用户固定 IP。
	 * @return		借用的 host_t*（内部持有）；未命中返回 NULL
	 */
	host_t *(*lookup_fixed)(policy_engine_t *this, const char *user);

	/**
	 * 取动态池端点。
	 * @return		TRUE 成功且已配置池；FALSE 未配置池
	 */
	bool (*get_pool)(policy_engine_t *this, host_t **start, host_t **end);

	/**
	 * 热重载（SIGHUP / ipsec rereadall 触发）。
	 * @return		TRUE 成功（原子切换到新策略）；FALSE 失败（保留旧策略）
	 */
	bool (*reload)(policy_engine_t *this);
};

/**
 * 创建并加载策略。path 为 policy.yaml 路径。
 * 加载失败返回 NULL（fail-fast，让 charon 拒绝加载插件并日志）。
 */
policy_engine_t *policy_engine_create(const char *path);

void policy_engine_destroy(policy_engine_t *this);

#endif /* POLICY_ENGINE_H_ */
/* 修改内容：创建策略引擎接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

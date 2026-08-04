/* 修改内容：创建 IP 分配核心接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef IP_ALLOCATOR_H_
#define IP_ALLOCATOR_H_

#include <library.h>
#include <networking/host.h>
#include <attributes/attribute_provider.h>
#include <sa/ike_sa.h>
#include "policy_engine.h"

typedef struct ip_allocator_t ip_allocator_t;

/**
 * IP 分配核心。同时实现 attribute_provider_t 接口，注册到 charon 的 attribute manager。
 *
 * 机制（与 strongSwan VIP 流程一致）：
 *   - 连接配置 connections.<conn>.pools = <pool_name> 触发 charon 调用
 *     acquire_address(pools, ike_sa, requested)。CUPP 识别 pool_name 后按策略分配。
 *   - SA 断开时 charon 调用 release_address(pools, address, ike_sa) 回收到动态池。
 *   - create_attribute_enumerator 返回空（CUPP 不下发 DNS 等额外属性，留给 attr 插件）。
 *
 * 固定 IP 由 policy.yaml 无状态查出；动态池在内存维护，sweep 定时清理孤儿租约。
 */
struct ip_allocator_t {
	/**
	 * 公共 attribute_provider_t 接口（首成员，&this->provider 注册到 charon）。
	 */
	attribute_provider_t provider;

	/**
	 * 按 unique_id 幂等释放租约（event_listener 兜底用，与 release_address 共用，重复安全）。
	 */
	void (*release_by_uid)(ip_allocator_t *this, uint32_t unique_id);

	/**
	 * 扫描孤儿租约（acquired 超时且对应 ike_sa 已不存在），回收动态 IP。
	 * 由周期 callback_job 调用。
	 */
	void (*sweep)(ip_allocator_t *this);
};

/**
 * 创建分配器。
 * @param policy			策略引擎（借用的固定 IP / 动态池端点来源）
 * @param pool_name			CUPP 认领的池名（conn 用 pools = <pool_name> 引用）；NULL 则默认 "cupp"
 * @param lease_timeout		孤儿租约超时秒数
 */
ip_allocator_t *ip_allocator_create(policy_engine_t *policy, const char *pool_name,
									uint32_t lease_timeout);

void ip_allocator_destroy(ip_allocator_t *this);

#endif /* IP_ALLOCATOR_H_ */
/* 修改内容：创建 IP 分配核心接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

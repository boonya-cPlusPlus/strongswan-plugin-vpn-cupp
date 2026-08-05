/* 修改内容：创建会话租约管理接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef SESSION_MANAGER_H_
#define SESSION_MANAGER_H_

#include <library.h>
#include <networking/host.h>
#include <sys/types.h>

typedef struct session_manager_t session_manager_t;
typedef struct lease_t lease_t;

/**
 * 单条活跃租约。lease 拥有 ip（固定 IP 为 policy 的克隆，动态 IP 为池新分配）。
 * lease_destroy 一律释放 ip。fixed 仅用于 release 时判断是否回动态池。
 */
struct lease_t {
	char *username;			/* 堆字符串，lease 拥有 */
	uint32_t unique_id;		/* ike_sa unique_id，release 匹配用 */
	host_t *ip;				/* lease 拥有，lease_destroy 释放 */
	bool fixed;				/* TRUE=固定 IP（release 不回动态池） */
	time_t acquired;		/* 分配时间，sweep 判超时用 */
};

/**
 * 活跃租约表，双索引：
 *   by_unique_id : uint32_t -> lease_t*   （release 用）
 *   by_username   : char*     -> lease_t* （幂等/rekey 用）
 * 内部自保护（互斥锁）。
 */
struct session_manager_t {
	/**
	 * 绑定新租约（ip 所有权转移给 lease）。
	 * 调用前应由 ip_allocator 通过 lookup_by_user 排除 rekey 场景；
	 * 若内部仍检测到同用户已有租约（竞态），按"后到优先"覆盖旧租约
	 * （销毁旧 lease 含其 ip），新 lease 接管传入的 ip。
	 * @param ip		所有权转移给 lease（fixed=fixed_pool.clone，dynamic=pool.acquire）
	 * @return			绑定的 lease（借用，内部持有）；失败返回 NULL（ip 已被销毁）
	 */
	/* 修改内容：bind 重命名为 bind_lease，避免与 POSIX socket bind() 冲突 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- start ---- */
	lease_t *(*bind_lease)(session_manager_t *this, const char *username,
					 uint32_t unique_id, host_t *ip, bool fixed);
	/* 修改内容：bind 重命名为 bind_lease，避免与 POSIX socket bind() 冲突 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- end ---- */

	/**
	 * 迁移现有租约的 unique_id 到新值（rekey 场景，复用原 ip）。
	 * @return			借用的迁移后 lease；不存在返回 NULL
	 */
	lease_t *(*migrate)(session_manager_t *this, const char *username,
						uint32_t new_unique_id);

	/**
	 * 按用户名查租约（幂等/rekey 用）。
	 * @return			借用的 lease；无返回 NULL
	 */
	lease_t *(*lookup_by_user)(session_manager_t *this, const char *username);

	/**
	 * 按 unique_id 解绑租约。idempotent：首次返回 lease（所有权转移，调用者用完调 lease_destroy），
	 * 再次对同 uid 调用返回 NULL（防双重释放/双重回收）。
	 * @return			转移所有权的 lease；不存在/已解绑返回 NULL
	 */
	lease_t *(*unbind)(session_manager_t *this, uint32_t unique_id);

	/**
	 * 遍历所有租约，对 (now - acquired > timeout) 的逐个回调。
	 * 回调内若需释放，应调 unbind(lease->unique_id)。
	 */
	void (*foreach_expiring)(session_manager_t *this, time_t now, uint32_t timeout,
							 void (*cb)(lease_t *lease, void *ctx), void *ctx);

	/** 当前活跃租约数 */
	int (*count)(session_manager_t *this);
};

session_manager_t *session_manager_create(void);
void session_manager_destroy(session_manager_t *this);

/** 释放 unbind 返回的 lease：释放 username、ip 与结构体本身（lease 拥有 ip）。 */
void lease_destroy(lease_t *lease);

#endif /* SESSION_MANAGER_H_ */
/* 修改内容：创建会话租约管理接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

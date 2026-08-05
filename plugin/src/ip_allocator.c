/* 修改内容：创建 IP 分配核心实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#include "ip_allocator.h"
#include "identity_manager.h"
#include "dynamic_pool.h"
#include "session_manager.h"
#include "cupp_log.h"

#include <daemon.h>						/* charon */
#include <attributes/attribute_manager.h>
#include <sa/ike_sa_manager.h>
#include <collections/linked_list.h>
#include <utils/utils.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct private_ip_allocator_t private_ip_allocator_t;

/*
 * attribute_provider_t 是首成员：charon 持有 &this->provider 并回调其方法，
 * 方法内通过 container_of 等价（首成员偏移 0）转回 ip_allocator。
 */
struct private_ip_allocator_t {
	ip_allocator_t public;
	policy_engine_t *policy;	/* 借用 */
	dynamic_pool_t *dyn;		/* 动态池（NULL=未配置） */
	session_manager_t *sessions;
	char *pool_name;			/* CUPP 认领的池名（conn.pools 引用） */
	uint32_t lease_timeout;		/* 孤儿租约超时秒数 */
	pthread_mutex_t lock;		/* 序列化 acquire/release 复合操作 */
};

/* ---- 内部辅助 ---- */

/* 判断 pools 列表是否包含 CUPP 认领的池名（pools 为 char* 链表） */
static bool pools_include(private_ip_allocator_t *this, linked_list_t *pools)
{
	enumerator_t *e;
	char *name;

	if (!pools || pools->get_count(pools) == 0)
	{
		return FALSE;
	}
	e = pools->create_enumerator(pools);
	while (e->enumerate(e, &name))
	{
		if (name && streq(name, this->pool_name))
		{
			e->destroy(e);
			return TRUE;
		}
	}
	e->destroy(e);
	return FALSE;
}

/*
 * 按 unique_id 幂等释放租约。重复安全（unbind 第二次返回 NULL）。
 * 动态 IP：清池位 + lease_destroy（释放 ip）；固定 IP：仅 lease_destroy。
 * 返回 TRUE 表示找到并释放了租约；FALSE 表示无对应租约（已释放或从未分配）。
 */
static bool release_by_uid_impl(private_ip_allocator_t *this, uint32_t uid)
{
	lease_t *lease;

	pthread_mutex_lock(&this->lock);
	lease = this->sessions->unbind(this->sessions, uid);
	pthread_mutex_unlock(&this->lock);

	if (!lease)
	{
		return FALSE;	/* 不存在或已释放（幂等） */
	}
	if (!lease->fixed && this->dyn && lease->ip)
	{
		/* 动态 IP：先清池位（不释放 host），再由 lease_destroy 释放 host */
		this->dyn->release(this->dyn, lease->ip);
	}
	cupp_log("release uid=%u user=%s ip=%H (%s)",
			 uid, lease->username, lease->ip,
			 lease->fixed ? "fixed" : "pool, recycled");
	lease_destroy(lease);	/* 释放 ip + username + 结构体 */
	return TRUE;
}

/* ---- attribute_provider_t 实现 ---- */

METHOD(attribute_provider_t, acquire_address, host_t*,
	private_ip_allocator_t *this, linked_list_t *pools,
	ike_sa_t *ike_sa, host_t *requested)
{
	char *username = NULL;
	uint32_t uid;
	lease_t *lease = NULL;
	host_t *fixed_src, *ip = NULL, *result = NULL;
	bool is_fixed = FALSE;

	(void)requested;	/* CUPP 按策略分配，忽略客户端请求的特定地址 */

	/* 诊断日志：打印 charon 传入的 pools 内容 */
	{
		enumerator_t *e;
		char *name;
		u_int count = 0;
		if (pools)
		{
			count = pools->get_count(pools);
		}
		cupp_log("acquire ENTER: pools_count=%u", count);
		if (pools)
		{
			e = pools->create_enumerator(pools);
			while (e->enumerate(e, &name))
			{
				cupp_log("  pool name=[%s] our_pool=[%s] match=%d",
						 name ? name : "(null)",
						 this->pool_name,
						 (name && streq(name, this->pool_name)));
			}
			e->destroy(e);
		}
	}

	if (!pools_include(this, pools) || !ike_sa)
	{
		cupp_warn("acquire: pools_include=FALSE or ike_sa=NULL, skipped");
		return NULL;	/* 非本插件池，交由其他 provider */
	}
	cupp_log("acquire: pools_include=TRUE, proceeding");

	/* 修改内容：username 为 NULL 时回退动态池（用占位名 bind_lease），避免连接直接失败 修改人：pengjunlin 时间：2026-08-05 16:00:00 -- start ---- */
	username = identity_get_username(ike_sa);
	uid = ike_sa->get_unique_id(ike_sa);

	/* 无法提取字符串用户名：不能查固定表，但仍走动态池让连接成功 */
	if (!username)
	{
		char anon_buf[32];
		cupp_warn("acquire: no string identity, falling back to dynamic pool");
		snprintf(anon_buf, sizeof(anon_buf), "anon-%u", uid);
		username = strdup(anon_buf);
		if (!username)
		{
			/* OOM */
			return NULL;
		}
	}

	pthread_mutex_lock(&this->lock);

	/* rekey/重连幂等：同用户已有活跃租约 → 迁移 unique_id，返回原 IP 的克隆 */
	lease = this->sessions->lookup_by_user(this->sessions, username);
	if (lease)
	{
		this->sessions->migrate(this->sessions, username, uid);
		result = lease->ip->clone(lease->ip);
		cupp_log("acquire uid=%u user=%s -> %H (rekey, reused %s)",
				 uid, username, result, lease->fixed ? "fixed" : "pool");
		pthread_mutex_unlock(&this->lock);
		free(username);
		return result;
	}

	/* 新连接：先查固定 IP，否则动态池 */
	{
		bool is_anon = (strncmp(username, "anon-", 5) == 0);
		if (!is_anon)
		{
			fixed_src = this->policy->lookup_fixed(this->policy, username);
			if (fixed_src)
			{
				ip = fixed_src->clone(fixed_src);	/* lease 拥有此克隆 */
				is_fixed = TRUE;
			}
		}
	}
	if (!ip && this->dyn)
	{
		ip = this->dyn->acquire(this->dyn);	/* 所有权转入 lease */
		is_fixed = FALSE;
	}
	/* 修改内容：username 为 NULL 时回退动态池（用占位名 bind_lease），避免连接直接失败 修改人：pengjunlin 时间：2026-08-05 16:00:00 -- end ---- */

	if (!ip)
	{
		/* 固定用户配置缺失，或动态池耗尽 */
		pthread_mutex_unlock(&this->lock);
		cupp_warn("acquire uid=%u user=%s -> no IP (pool exhausted or unconfigured)",
				  uid, username);
		free(username);
		return NULL;
	}

	/* 修改内容：bind 重命名为 bind_lease，避免与 POSIX socket bind() 冲突 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- start ---- */
	lease = this->sessions->bind_lease(this->sessions, username, uid, ip, is_fixed);
	/* 修改内容：bind 重命名为 bind_lease，避免与 POSIX socket bind() 冲突 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- end ---- */
	if (!lease)
	{
		/* bind 失败已销毁 ip；返回 NULL 让连接失败 */
		pthread_mutex_unlock(&this->lock);
		cupp_err("acquire uid=%u user=%s -> bind failed", uid, username);
		free(username);
		return NULL;
	}
	/* lease 拥有 ip；返回独立克隆给 charon（charon 负责释放） */
	result = lease->ip->clone(lease->ip);
	if (!result)
	{
		/* 克隆失败（OOM）：先放锁，再用 release_by_uid_impl 回滚租约（避免 IP 空占） */
		pthread_mutex_unlock(&this->lock);
		release_by_uid_impl(this, uid);
		cupp_err("acquire uid=%u user=%s -> clone failed", uid, username);
		free(username);
		return NULL;
	}
	cupp_log("acquire uid=%u user=%s -> %H (%s)",
			 uid, username, result, is_fixed ? "fixed" : "pool");

	pthread_mutex_unlock(&this->lock);
	free(username);
	return result;
}

METHOD(attribute_provider_t, release_address, bool,
	private_ip_allocator_t *this, linked_list_t *pools,
	host_t *address, ike_sa_t *ike_sa)
{
	uint32_t uid;

	(void)pools;
	(void)address;		/* charon 拥有 address 并自行释放；CUPP 用 lease 内的 ip */

	if (!ike_sa)
	{
		return FALSE;
	}
	uid = ike_sa->get_unique_id(ike_sa);
	return release_by_uid_impl(this, uid);
}

METHOD(attribute_provider_t, create_attribute_enumerator, enumerator_t*,
	private_ip_allocator_t *this, linked_list_t *pools,
	ike_sa_t *ike_sa, linked_list_t *vips)
{
	/* CUPP 通过 acquire_address 下发 VIP，不在此下发 DNS 等额外属性。
	 * 返回空枚举，留给 attr 插件处理其他属性。 */
	(void)this; (void)pools; (void)ike_sa; (void)vips;
	return enumerator_create_empty();
}

/* ---- ip_allocator_t 公共方法 ---- */

METHOD(ip_allocator_t, release_by_uid, void,
	private_ip_allocator_t *this, uint32_t unique_id)
{
	release_by_uid_impl(this, unique_id);
}

/*
 * sweep 回调：对每条超时租约，checkout 其 ike_sa；若 SA 已不存在则视为孤儿并释放。
 * SA 仍存活（长连接）则跳过，等待下次扫描。
 */
static void sweep_cb(lease_t *lease, void *ctx)
{
	private_ip_allocator_t *this = ctx;
	ike_sa_t *ike_sa;

	ike_sa = charon->ike_sa_manager->checkout_by_id(
		charon->ike_sa_manager, lease->unique_id);
	if (ike_sa)
	{
		/* SA 仍存活，非孤儿：归还，保留租约 */
		charon->ike_sa_manager->checkin(charon->ike_sa_manager, ike_sa);
		return;
	}
	cupp_log("sweep: expired uid=%u user=%s ip=%H (recycled)",
			 lease->unique_id, lease->username, lease->ip);
	release_by_uid_impl(this, lease->unique_id);
}

METHOD(ip_allocator_t, sweep, void,
	private_ip_allocator_t *this)
{
	time_t now = time(NULL);
	this->sessions->foreach_expiring(this->sessions, now, this->lease_timeout,
									sweep_cb, this);
}

/* ---- 构造/析构 ---- */

ip_allocator_t *ip_allocator_create(policy_engine_t *policy, const char *pool_name,
									uint32_t lease_timeout)
{
	private_ip_allocator_t *this;
	host_t *start = NULL, *end = NULL;

	if (!policy)
	{
		return NULL;
	}
	this = calloc(1, sizeof(*this));
	if (!this)
	{
		return NULL;
	}
	this->policy = policy;
	this->sessions = session_manager_create();
	this->pool_name = strdup(pool_name ? pool_name : "cupp");
	this->lease_timeout = lease_timeout ? lease_timeout : 3600;

	/* 动态池：若 policy 配置了 pool 端点则创建，否则 NULL（仅固定 IP 模式） */
	if (policy->get_pool(policy, &start, &end))
	{
		this->dyn = dynamic_pool_create(start, end);	/* 内部 clone 端点 */
		if (!this->dyn)
		{
			cupp_err("dynamic_pool_create failed, dynamic allocation disabled");
		}
	}
	else
	{
		cupp_warn("no dynamic pool configured, only fixed IP will be served");
	}

	pthread_mutex_init(&this->lock, NULL);

	this->public.provider.acquire_address = _acquire_address;
	this->public.provider.release_address = _release_address;
	this->public.provider.create_attribute_enumerator = _create_attribute_enumerator;
	this->public.release_by_uid = _release_by_uid;
	this->public.sweep = _sweep;
	return &this->public;
}

/* 修改内容：修复 destroy 调用方式：dynamic_pool_t / session_manager_t / fixed_pool_t 结构体里没有 ->destroy 成员，必须用同名独立函数（与 strongswan 5.8.2 的 fixed_pool_destroy 设计一致） 修改人：pengjunlin 时间：2026-08-05 18:10:00 -- start ---- */
void ip_allocator_destroy(ip_allocator_t *this)
{
	private_ip_allocator_t *p = (private_ip_allocator_t*)this;

	if (!p)
	{
		return;
	}
	if (p->dyn)
	{
		dynamic_pool_destroy(p->dyn);
	}
	session_manager_destroy(p->sessions);
	free(p->pool_name);
	pthread_mutex_destroy(&p->lock);
	free(p);
}
/* 修改内容：修复 destroy 调用方式：dynamic_pool_t / session_manager_t / fixed_pool_t 结构体里没有 ->destroy 成员，必须用同名独立函数（与 strongswan 5.8.2 的 fixed_pool_destroy 设计一致） 修改人：pengjunlin 时间：2026-08-05 18:10:00 -- end ---- */
/* 修改内容：创建 IP 分配核心实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

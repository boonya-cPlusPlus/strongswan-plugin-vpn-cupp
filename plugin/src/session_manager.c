/* 修改内容：创建会话租约管理实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
/* 修改内容：修复 hashtable.h 头文件路径（strongswan 5.8.2 中位于 collections/ 非 utils/） 修改人：pengjunlin 时间：2026-08-05 18:10:00 -- start ---- */
#include "session_manager.h"
#include "cupp_log.h"

#include <collections/hashtable.h>
#include <utils/utils.h>
#include <collections/linked_list.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* 修改内容：修复 hashtable.h 头文件路径（strongswan 5.8.2 中位于 collections/ 非 utils/） 修改人：pengjunlin 时间：2026-08-05 18:10:00 -- end ---- */

typedef struct private_session_manager_t private_session_manager_t;

struct private_session_manager_t {
	session_manager_t public;
	pthread_mutex_t mutex;
	hashtable_t *by_uid;	/* (void*)(uintptr_t)uid -> lease_t* */
	hashtable_t *by_user;	/* char*(=lease->username) -> lease_t* */
};

/* uid 直接编码进 key 指针，零堆分配 */
static u_int cupp_u32_hash(const void *key)
{
	return (u_int)(uintptr_t)key;
}
static bool cupp_u32_eq(const void *a, const void *b)
{
	return a == b;
}
static u_int cupp_str_hash(const void *key)
{
	const char *s = key;
	u_int h = 5381;
	while (*s)
	{
		h = h * 33 + (u_char)*s++;
	}
	return h;
}
static bool cupp_str_eq(const void *a, const void *b)
{
	return streq((const char*)a, (const char*)b);
}

/*
 * lease 所有权规则（统一）：
 *   lease 始终拥有 ip（fixed 为 fixed_pool 的 clone，dynamic 为 pool.acquire 的产物）。
 *   lease_destroy 一律释放 ip。
 * by_user 的 key 即 lease->username（lease 拥有），随 lease_destroy 一并释放。
 */
static lease_t *lease_create(const char *username, uint32_t uid,
							 host_t *ip, bool fixed)
{
	lease_t *l = malloc(sizeof(*l));
	if (!l)
	{
		return NULL;
	}
	l->username = strdup(username);
	l->unique_id = uid;
	l->ip = ip;
	l->fixed = fixed;
	l->acquired = time(NULL);
	return l;
}

void lease_destroy(lease_t *lease)
{
	if (!lease)
	{
		return;
	}
	if (lease->ip)
	{
		lease->ip->destroy(lease->ip);
	}
	free(lease->username);
	free(lease);
}

/* 修改内容：bind 重命名为 bind_lease，避免与 POSIX socket bind() 冲突 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- start ---- */
METHOD(session_manager_t, bind_lease, lease_t*,
	private_session_manager_t *this, const char *username,
	uint32_t unique_id, host_t *ip, bool fixed)
/* 修改内容：bind 重命名为 bind_lease，避免与 POSIX socket bind() 冲突 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- end ---- */
{
	lease_t *existing, *lease;

	if (!username)
	{
		if (ip)
		{
			ip->destroy(ip);
		}
		return NULL;
	}
	pthread_mutex_lock(&this->mutex);

	/* 竞态兜底：同用户已有租约（ip_allocator 未持锁时的理论竞态）。
	 * 按"后到优先"覆盖：销毁旧 lease（含其 ip），新 lease 接管传入 ip。 */
	existing = this->by_user->get(this->by_user, (void*)username);
	if (existing)
	{
		this->by_uid->remove(this->by_uid, (void*)(uintptr_t)existing->unique_id);
		this->by_user->remove(this->by_user, existing->username);
		lease_destroy(existing);
	}

	lease = lease_create(username, unique_id, ip, fixed);
	if (!lease)
	{
		pthread_mutex_unlock(&this->mutex);
		if (ip)
		{
			ip->destroy(ip);
		}
		return NULL;
	}
	this->by_uid->put(this->by_uid, (void*)(uintptr_t)unique_id, lease);
	this->by_user->put(this->by_user, lease->username, lease);

	pthread_mutex_unlock(&this->mutex);
	return lease;
}

METHOD(session_manager_t, migrate, lease_t*,
	private_session_manager_t *this, const char *username,
	uint32_t new_unique_id)
{
	lease_t *lease;

	if (!username)
	{
		return NULL;
	}
	pthread_mutex_lock(&this->mutex);
	lease = this->by_user->get(this->by_user, (void*)username);
	if (lease)
	{
		this->by_uid->remove(this->by_uid, (void*)(uintptr_t)lease->unique_id);
		lease->unique_id = new_unique_id;
		lease->acquired = time(NULL);
		this->by_uid->put(this->by_uid, (void*)(uintptr_t)new_unique_id, lease);
	}
	pthread_mutex_unlock(&this->mutex);
	return lease;	/* 借用 */
}

METHOD(session_manager_t, lookup_by_user, lease_t*,
	private_session_manager_t *this, const char *username)
{
	lease_t *lease;

	if (!username)
	{
		return NULL;
	}
	pthread_mutex_lock(&this->mutex);
	lease = this->by_user->get(this->by_user, (void*)username);
	pthread_mutex_unlock(&this->mutex);
	return lease;	/* 借用 */
}

METHOD(session_manager_t, unbind, lease_t*,
	private_session_manager_t *this, uint32_t unique_id)
{
	lease_t *lease;

	pthread_mutex_lock(&this->mutex);
	lease = this->by_uid->remove(this->by_uid, (void*)(uintptr_t)unique_id);
	if (lease)
	{
		this->by_user->remove(this->by_user, lease->username);
	}
	pthread_mutex_unlock(&this->mutex);
	return lease;	/* 所有权转移给调用者；不存在/已解绑返回 NULL（幂等） */
}

static void lease_cb(lease_t *lease, void *ctx)
{
	/* 占位：实际回调由调用者传入，此处不直接用 */
	(void)lease; (void)ctx;
}

METHOD(session_manager_t, foreach_expiring, void,
	private_session_manager_t *this, time_t now, uint32_t timeout,
	void (*cb)(lease_t *lease, void *ctx), void *ctx)
{
	linked_list_t *snapshot;
	enumerator_t *e;
	lease_t *l;
	void *k;

	if (!cb)
	{
		cb = lease_cb;
	}
	snapshot = linked_list_create();

	pthread_mutex_lock(&this->mutex);
	e = this->by_uid->create_enumerator(this->by_uid);
	while (e->enumerate(e, &k, &l))
	{
		if (now - l->acquired > (time_t)timeout)
		{
			snapshot->insert_last(snapshot, l);
		}
	}
	e->destroy(e);
	pthread_mutex_unlock(&this->mutex);

	/* 回调在锁外执行（cb 可能触发 unbind） */
	e = snapshot->create_enumerator(snapshot);
	while (e->enumerate(e, &l))
	{
		cb(l, ctx);
	}
	e->destroy(e);
	snapshot->destroy(snapshot);
}

/* 修改内容：strongSwan 5.8.2 hashtable_t 无 count 成员，改用 enumerate 遍历计数 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- start ---- */
METHOD(session_manager_t, count, int,
	private_session_manager_t *this)
{
	int c = 0;
	enumerator_t *e;
	void *k;
	lease_t *l;

	pthread_mutex_lock(&this->mutex);
	e = this->by_uid->create_enumerator(this->by_uid);
	while (e->enumerate(e, &k, &l))
	{
		c++;
	}
	e->destroy(e);
	pthread_mutex_unlock(&this->mutex);
	return c;
}
/* 修改内容：strongSwan 5.8.2 hashtable_t 无 count 成员，改用 enumerate 遍历计数 修改人：pengjunlin 时间：2026-08-05 19:45:00 -- end ---- */

session_manager_t *session_manager_create(void)
{
	private_session_manager_t *this;

	INIT(this,
		.public = {
			.bind_lease = _bind_lease,
			.migrate = _migrate,
			.lookup_by_user = _lookup_by_user,
			.unbind = _unbind,
			.foreach_expiring = _foreach_expiring,
			.count = _count,
		},
/* 修改内容：hashtable_create 在 strongswan 5.8.2 中要求 3 参数（hash, equals, capacity），补 capacity=0 让库自选初始容量 修改人：pengjunlin 时间：2026-08-05 18:20:00 -- start ---- */
		.by_uid = hashtable_create(cupp_u32_hash, cupp_u32_eq, 0),
		.by_user = hashtable_create(cupp_str_hash, cupp_str_eq, 0),
/* 修改内容：hashtable_create 在 strongswan 5.8.2 中要求 3 参数（hash, equals, capacity），补 capacity=0 让库自选初始容量 修改人：pengjunlin 时间：2026-08-05 18:20:00 -- end ---- */
	);
	pthread_mutex_init(&this->mutex, NULL);
	return &this->public;
}

void session_manager_destroy(session_manager_t *this)
{
	private_session_manager_t *p = (private_session_manager_t*)this;
	enumerator_t *e;
	void *k;
	lease_t *l;

	e = p->by_uid->create_enumerator(p->by_uid);
	while (e->enumerate(e, &k, &l))
	{
		lease_destroy(l);	/* 释放 username（by_user key）与动态 ip */
	}
	e->destroy(e);

	p->by_uid->destroy(p->by_uid);
	p->by_user->destroy(p->by_user);
	pthread_mutex_destroy(&p->mutex);
	free(p);
}
/* 修改内容：创建会话租约管理实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

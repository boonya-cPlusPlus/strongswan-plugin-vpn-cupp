/* 修改内容：创建策略引擎实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
/* 修改内容：修复 private_policy_engine_t 缺少 typedef 导致 C 编译器报 unknown type name 修改人：pengjunlin 时间：2026-08-05 18:00:00 -- start ---- */
#include "policy_engine.h"
#include "yaml_loader.h"
#include "dynamic_pool.h"
#include "cupp_log.h"

#include <utils/utils.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/*
 * 双 buffer 策略：active 始终指向当前生效的 snapshot，读无锁（atomic load）。
 * reload 时构建新 snapshot，原子替换 active 指针，再销毁旧 snapshot。
 *
 * snapshot 内含 fixed_pool（用户固定 IP）与 pool 端点（动态池起止）。
 * fixed_pool.lookup 返回借用 host_t*；调用方（ip_allocator）用完即克隆，不持有。
 */
typedef struct snapshot_t {
	fixed_pool_t *fixed;		/* 用户名 -> host_t*（fixed 拥有） */
	host_t *pool_start;			/* 动态池起始（含），NULL=未配置 */
	host_t *pool_end;			/* 动态池结束（含） */
} snapshot_t;

/* C（非 C++）不会把 struct tag 自动当类型名，必须 typedef
 * 否则 METHOD 宏、函数签名里的 private_policy_engine_t 会报 unknown type name */
typedef struct private_policy_engine_t private_policy_engine_t;

struct private_policy_engine_t {
	policy_engine_t public;
	snapshot_t * volatile active;	/* 当前生效快照 */
	char *path;					/* policy.yaml 路径（engine 持有） */
	pthread_mutex_t reload_lock;/* 序列化 reload（读路径无锁） */
};
/* 修改内容：修复 private_policy_engine_t 缺少 typedef 导致 C 编译器报 unknown type name 修改人：pengjunlin 时间：2026-08-05 18:00:00 -- end ---- */

static void snapshot_destroy(snapshot_t *s)
{
	if (!s)
	{
		return;
	}
	if (s->fixed)
	{
		fixed_pool_destroy(s->fixed);
	}
	if (s->pool_start)
	{
		s->pool_start->destroy(s->pool_start);
	}
	if (s->pool_end)
	{
		s->pool_end->destroy(s->pool_end);
	}
	free(s);
}

/*
 * 从 path 加载构建新 snapshot。失败返回 NULL。
 * pool_start/pool_end 由 yaml_loader 创建（所有权转入 snapshot）。
 */
static snapshot_t *snapshot_load(const char *path)
{
	snapshot_t *s;
	host_t *start = NULL, *end = NULL;

	s = calloc(1, sizeof(*s));
	if (!s)
	{
		return NULL;
	}
	s->fixed = fixed_pool_create();
	if (!s->fixed)
	{
		free(s);
		return NULL;
	}
	if (yaml_load_policy(path, s->fixed, &start, &end) != 0)
	{
		cupp_err("policy load failed: %s", path);
		snapshot_destroy(s);
		if (start) start->destroy(start);
		if (end) end->destroy(end);
		return NULL;
	}
	s->pool_start = start;
	s->pool_end = end;

	if (start && end)
	{
		cupp_log("policy loaded: %d fixed users, pool %H-%H",
				 s->fixed->count(s->fixed), start, end);
	}
	else
	{
		cupp_log("policy loaded: %d fixed users, no dynamic pool",
				 s->fixed->count(s->fixed));
	}
	return s;
}

METHOD(policy_engine_t, lookup_fixed, host_t*,
	private_policy_engine_t *this, const char *user)
{
	snapshot_t *s = this->active;
	if (!s || !s->fixed || !user)
	{
		return NULL;
	}
	return s->fixed->lookup(s->fixed, user);
}

METHOD(policy_engine_t, get_pool, bool,
	private_policy_engine_t *this, host_t **start, host_t **end)
{
	snapshot_t *s = this->active;
	if (!s || !s->pool_start || !s->pool_end)
	{
		return FALSE;
	}
	if (start)
	{
		*start = s->pool_start;	/* 借用 */
	}
	if (end)
	{
		*end = s->pool_end;		/* 借用 */
	}
	return TRUE;
}

METHOD(policy_engine_t, reload, bool,
	private_policy_engine_t *this)
{
	snapshot_t *fresh, *old;

	pthread_mutex_lock(&this->reload_lock);
	fresh = snapshot_load(this->path);
	if (!fresh)
	{
		pthread_mutex_unlock(&this->reload_lock);
		cupp_err("reload failed, keeping old policy");
		return FALSE;
	}
	old = this->active;
	this->active = fresh;		/* 原子切换（reload_lock 内单写者） */
	pthread_mutex_unlock(&this->reload_lock);

	snapshot_destroy(old);
	cupp_log("policy reloaded");
	return TRUE;
}

policy_engine_t *policy_engine_create(const char *path)
{
	private_policy_engine_t *this;
	snapshot_t *s;

	if (!path)
	{
		return NULL;
	}
	s = snapshot_load(path);
	if (!s)
	{
		return NULL;
	}

	this = calloc(1, sizeof(*this));
	if (!this)
	{
		snapshot_destroy(s);
		return NULL;
	}
	this->public.lookup_fixed = _lookup_fixed;
	this->public.get_pool = _get_pool;
	this->public.reload = _reload;
	this->active = s;
	this->path = strdup(path);
	pthread_mutex_init(&this->reload_lock, NULL);

	return &this->public;
}

void policy_engine_destroy(policy_engine_t *pub)
{
	private_policy_engine_t *this;

	if (!pub)
	{
		return;
	}
	this = (private_policy_engine_t *)((char*)pub - offsetof(private_policy_engine_t, public));
	snapshot_destroy(this->active);
	free(this->path);
	pthread_mutex_destroy(&this->reload_lock);
	free(this);
}
/* 修改内容：创建策略引擎实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

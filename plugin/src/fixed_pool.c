/* 修改内容：创建固定 IP 池实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
/* 修改内容：修复 hashtable.h 头文件路径（strongswan 5.8.2 中位于 collections/ 非 utils/） 修改人：pengjunlin 时间：2026-08-05 18:10:00 -- start ---- */
#include "fixed_pool.h"
#include "cupp_log.h"

#include <collections/hashtable.h>
#include <utils/utils.h>
#include <stdlib.h>
#include <string.h>
/* 修改内容：修复 hashtable.h 头文件路径（strongswan 5.8.2 中位于 collections/ 非 utils/） 修改人：pengjunlin 时间：2026-08-05 18:10:00 -- end ---- */

typedef struct private_fixed_pool_t private_fixed_pool_t;

struct private_fixed_pool_t {
	fixed_pool_t public;
	hashtable_t *users;		/* char* -> host_t*（key/val 均内部拥有） */
	int count;
};

/* 字符串键 hash/equals（不依赖 strongswan 字符串辅助函数名，跨版本通用） */
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

METHOD(fixed_pool_t, lookup, host_t*,
	private_fixed_pool_t *this, const char *user)
{
	if (!user)
	{
		return NULL;
	}
	return this->users->get(this->users, (void*)user);
}

METHOD(fixed_pool_t, add, bool,
	private_fixed_pool_t *this, const char *user, host_t *ip)
{
	char *key;
	host_t *old;

	if (!user || !ip)
	{
		return FALSE;
	}
	key = strdup(user);
	old = this->users->put(this->users, key, ip->clone(ip));
	if (old)
	{
		/* 同名条目已存在：旧 key 被保留、新 key 未用，需释放；旧值销毁 */
		old->destroy(old);
		free(key);
	}
	else
	{
		this->count++;
	}
	return TRUE;
}

METHOD(fixed_pool_t, count, int,
	private_fixed_pool_t *this)
{
	return this->count;
}

fixed_pool_t *fixed_pool_create(void)
{
	private_fixed_pool_t *this;

	INIT(this,
		.public = {
			.lookup = _lookup,
			.add = _add,
			.count = _count,
		},
/* 修改内容：hashtable_create 在 strongswan 5.8.2 中要求 3 参数（hash, equals, capacity），补 capacity=0 让库自选初始容量 修改人：pengjunlin 时间：2026-08-05 18:20:00 -- start ---- */
		.users = hashtable_create(cupp_str_hash, cupp_str_eq, 0),
/* 修改内容：hashtable_create 在 strongswan 5.8.2 中要求 3 参数（hash, equals, capacity），补 capacity=0 让库自选初始容量 修改人：pengjunlin 时间：2026-08-05 18:20:00 -- end ---- */
	);
	return &this->public;
}

void fixed_pool_destroy(fixed_pool_t *this)
{
	private_fixed_pool_t *p = (private_fixed_pool_t*)this;
	enumerator_t *e;
	char *k;
	host_t *v;

	e = p->users->create_enumerator(p->users);
	while (e->enumerate(e, &k, &v))
	{
		v->destroy(v);
		free(k);
	}
	e->destroy(e);
	p->users->destroy(p->users);
	free(p);
}
/* 修改内容：创建固定 IP 池实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

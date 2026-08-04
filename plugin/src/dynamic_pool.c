/* 修改内容：创建动态地址池实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#include "dynamic_pool.h"
#include "cupp_log.h"

#include <utils/utils.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <string.h>

typedef struct private_dynamic_pool_t private_dynamic_pool_t;

struct private_dynamic_pool_t {
	dynamic_pool_t public;
	pthread_mutex_t mutex;
	host_t *start;
	host_t *end;
	uint32_t start_h;		/* 主机字节序起止 */
	uint32_t end_h;
	uint32_t capacity;
	uint8_t *bits;			/* 占用位图 */
	uint32_t used;
};

static uint32_t host_to_u32(host_t *h)
{
	chunk_t c = h->get_address(h);
	uint32_t v = 0;
	if (c.len >= 4)
	{
		memcpy(&v, c.ptr, 4);
	}
	return ntohl(v);
}

static host_t *u32_to_host(uint32_t v)
{
	uint32_t n = htonl(v);
	return host_create_from_chunk(AF_INET, chunk_from_thing(n), 0);
}

METHOD(dynamic_pool_t, acquire, host_t*,
	private_dynamic_pool_t *this)
{
	host_t *result = NULL;
	uint32_t i;

	pthread_mutex_lock(&this->mutex);
	if (this->used < this->capacity)
	{
		for (i = 0; i < this->capacity; i++)
		{
			if (!(this->bits[i / 8] & (1 << (i % 8))))
			{
				this->bits[i / 8] |= (1 << (i % 8));
				this->used++;
				result = u32_to_host(this->start_h + i);
				break;
			}
		}
	}
	pthread_mutex_unlock(&this->mutex);
	return result;	/* 池满返回 NULL */
}

METHOD(dynamic_pool_t, release, void,
	private_dynamic_pool_t *this, host_t *ip)
{
	uint32_t v, i;

	if (!ip || ip->get_family(ip) != AF_INET)
	{
		return;
	}
	v = host_to_u32(ip);
	if (v < this->start_h || v > this->end_h)
	{
		return;
	}
	i = v - this->start_h;
	pthread_mutex_lock(&this->mutex);
	if (this->bits[i / 8] & (1 << (i % 8)))
	{
		this->bits[i / 8] &= ~(1 << (i % 8));
		if (this->used > 0)
		{
			this->used--;
		}
	}
	pthread_mutex_unlock(&this->mutex);
}

METHOD(dynamic_pool_t, is_in_pool, bool,
	private_dynamic_pool_t *this, host_t *ip)
{
	uint32_t v;

	if (!ip || ip->get_family(ip) != AF_INET)
	{
		return FALSE;
	}
	v = host_to_u32(ip);
	return v >= this->start_h && v <= this->end_h;
}

METHOD(dynamic_pool_t, capacity, uint32_t,
	private_dynamic_pool_t *this)
{
	return this->capacity;
}

METHOD(dynamic_pool_t, used, uint32_t,
	private_dynamic_pool_t *this)
{
	uint32_t u;
	pthread_mutex_lock(&this->mutex);
	u = this->used;
	pthread_mutex_unlock(&this->mutex);
	return u;
}

dynamic_pool_t *dynamic_pool_create(host_t *start, host_t *end)
{
	private_dynamic_pool_t *this;
	uint32_t s, e, cap;

	if (!start || !end ||
		start->get_family(start) != AF_INET ||
		end->get_family(end) != AF_INET)
	{
		cupp_err("dynamic pool requires IPv4 start/end");
		return NULL;
	}
	s = host_to_u32(start);
	e = host_to_u32(end);
	if (e < s)
	{
		cupp_err("dynamic pool end < start");
		return NULL;
	}
	cap = e - s + 1;

	INIT(this,
		.public = {
			.acquire = _acquire,
			.release = _release,
			.is_in_pool = _is_in_pool,
			.capacity = _capacity,
			.used = _used,
		},
		.start = start->clone(start),
		.end = end->clone(end),
		.start_h = s,
		.end_h = e,
		.capacity = cap,
		.bits = calloc((cap + 7) / 8, 1),
	);
	if (!this->bits)
	{
		cupp_err("dynamic pool bitmap alloc failed");
		this->start->destroy(this->start);
		this->end->destroy(this->end);
		free(this);
		return NULL;
	}
	pthread_mutex_init(&this->mutex, NULL);
	cupp_log("dynamic pool %H-%H capacity=%u", this->start, this->end, cap);
	return &this->public;
}

void dynamic_pool_destroy(dynamic_pool_t *this)
{
	private_dynamic_pool_t *p = (private_dynamic_pool_t*)this;

	pthread_mutex_destroy(&p->mutex);
	free(p->bits);
	p->start->destroy(p->start);
	p->end->destroy(p->end);
	free(p);
}
/* 修改内容：创建动态地址池实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

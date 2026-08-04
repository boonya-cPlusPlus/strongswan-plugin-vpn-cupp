/* 修改内容：创建动态地址池接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef DYNAMIC_POOL_H_
#define DYNAMIC_POOL_H_

#include <library.h>
#include <networking/host.h>

typedef struct dynamic_pool_t dynamic_pool_t;

/**
 * 动态虚拟 IP 池：[start, end] 区间内的 IPv4 地址，按位图跟踪占用。
 * 内部自保护（互斥锁）。
 */
struct dynamic_pool_t {
	/**
	 * 分配一个空闲 IP。
	 * @return		新 host_t*（调用者拥有，需 host->destroy）；池满返回 NULL
	 */
	host_t *(*acquire)(dynamic_pool_t *this);

	/**
	 * 回收一个 IP 到池（清占用位）。idempotent：重复回收安全。
	 */
	void (*release)(dynamic_pool_t *this, host_t *ip);

	/** 判断该 IP 是否在池区间内 */
	bool (*is_in_pool)(dynamic_pool_t *this, host_t *ip);

	/** 池容量（end - start + 1） */
	uint32_t (*capacity)(dynamic_pool_t *this);

	/** 当前已分配数量 */
	uint32_t (*used)(dynamic_pool_t *this);
};

/**
 * 创建动态池。start/end 必须为 IPv4 且 start <= end。
 * 失败返回 NULL。
 */
dynamic_pool_t *dynamic_pool_create(host_t *start, host_t *end);

void dynamic_pool_destroy(dynamic_pool_t *this);

#endif /* DYNAMIC_POOL_H_ */
/* 修改内容：创建动态地址池接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

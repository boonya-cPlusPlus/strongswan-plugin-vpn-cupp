/* 修改内容：创建固定 IP 池接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef FIXED_POOL_H_
#define FIXED_POOL_H_

#include <library.h>
#include <networking/host.h>

typedef struct fixed_pool_t fixed_pool_t;

/**
 * 固定虚拟 IP 表：用户名 -> host_t。
 * 由 policy_engine 加载填充，ip_allocator 只读查询。
 * 固定 IP 永远属于该用户，无分配/回收逻辑。
 */
struct fixed_pool_t {
	/**
	 * 查询用户对应的固定 IP。
	 * @return		借用的 host_t*（内部持有，调用者不释放）；未命中返回 NULL
	 */
	host_t *(*lookup)(fixed_pool_t *this, const char *user);

	/**
	 * 添加一条固定 IP 映射。host_t 引用计数 +1。
	 * @return		TRUE 成功
	 */
	bool (*add)(fixed_pool_t *this, const char *user, host_t *ip);

	/** 当前条目数 */
	int (*count)(fixed_pool_t *this);
};

/**
 * 创建空的固定 IP 表。
 */
fixed_pool_t *fixed_pool_create(void);

/**
 * 销毁固定 IP 表，释放所有持有的 host_t 引用。
 */
void fixed_pool_destroy(fixed_pool_t *this);

#endif /* FIXED_POOL_H_ */
/* 修改内容：创建固定 IP 池接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

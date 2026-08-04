/* 修改内容：创建事件监听实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#include "event_listener.h"
#include "cupp_log.h"

#include <sa/ike_sa.h>
#include <utils/utils.h>
#include <stdlib.h>

typedef struct private_event_listener_t private_event_listener_t;

struct private_event_listener_t {
	event_listener_t public;
	ip_allocator_t *allocator;	/* 借用 */
};

/*
 * ike_updown：SA 起来时 up=TRUE（忽略），SA 关闭时 up=FALSE（兜底释放租约）。
 * release_by_uid 幂等：即便 release_address 已释放过，此处再调也安全。
 * 返回 TRUE 保持注册（监听器生命周期与插件相同）。
 */
METHOD(listener_t, ike_updown, bool,
	private_event_listener_t *this, ike_sa_t *ike_sa, bool up)
{
	if (!up && ike_sa)
	{
		uint32_t uid = ike_sa->get_unique_id(ike_sa);
		this->allocator->release_by_uid(this->allocator, uid);
	}
	return TRUE;	/* 保持注册 */
}

event_listener_t *event_listener_create(ip_allocator_t *allocator)
{
	private_event_listener_t *this;

	if (!allocator)
	{
		return NULL;
	}
	this = calloc(1, sizeof(*this));
	if (!this)
	{
		return NULL;
	}
	this->allocator = allocator;

	/* listener_t 其余回调保持 NULL：bus 对 NULL 回调直接跳过，不触发断言 */
	this->public.listener.ike_updown = _ike_updown;
	return &this->public;
}

void event_listener_destroy(event_listener_t *this)
{
	private_event_listener_t *p = (private_event_listener_t*)this;
	if (!p)
	{
		return;
	}
	free(p);
}
/* 修改内容：创建事件监听实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

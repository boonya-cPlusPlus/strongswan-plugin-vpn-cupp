/* 修改内容：创建事件监听接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef EVENT_LISTENER_H_
#define EVENT_LISTENER_H_

#include <bus/listeners/listener.h>
#include "ip_allocator.h"

typedef struct event_listener_t event_listener_t;

/**
 * IKE_SA 事件监听器。注册到 charon->bus。
 *
 * ike_updown(up=FALSE) 作为 release_address 的兜底：SA 异常关闭时确保动态 IP 回收。
 * release_by_uid 幂等，与 release_address 重复调用安全。
 */
struct event_listener_t {
	listener_t listener;	/* 首成员，&this->listener 注册到 bus */
};

event_listener_t *event_listener_create(ip_allocator_t *allocator);

void event_listener_destroy(event_listener_t *this);

#endif /* EVENT_LISTENER_H_ */
/* 修改内容：创建事件监听接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

/* 修改内容：创建身份获取实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#include "identity_manager.h"
#include "cupp_log.h"

#include <sa/auth/auth_cfg.h>
#include <credentials/identification.h>
#include <utils/utils.h>

/*
 * 从 IKE_SA 提取 EAP 用户名。
 * 优先 AUTH_RULE_EAP_IDENTITY，回退 AUTH_RULE_IDENTITY，再回退 ike_sa->get_other_id()。
 * 返回堆字符串（调用者 free）；取不到返回 NULL。
 */
char *identity_get_username(ike_sa_t *ike_sa)
{
	auth_cfg_t *ac;
	identification_t *id = NULL;

	if (!ike_sa)
	{
		return NULL;
	}

	/* local=FALSE 取对端（client）认证配置；服务端视角即 EAP 客户端身份 */
	ac = ike_sa->get_auth_cfg(ike_sa, FALSE);
	if (ac)
	{
		id = (identification_t*)ac->get(ac, AUTH_RULE_EAP_IDENTITY);
		if (!id)
		{
			id = (identification_t*)ac->get(ac, AUTH_RULE_IDENTITY);
		}
	}
	if (!id)
	{
		id = ike_sa->get_other_id(ike_sa);
	}
	if (!id)
	{
		cupp_warn("no identity available on IKE_SA");
		return NULL;
	}
	/* identification_t::to_string 返回堆字符串，调用者 free */
	return id->to_string(id);
}
/* 修改内容：创建身份获取实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

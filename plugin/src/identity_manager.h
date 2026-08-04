/* 修改内容：创建身份获取接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef IDENTITY_MANAGER_H_
#define IDENTITY_MANAGER_H_

#include <sa/ike_sa.h>

/**
 * 从 IKE_SA 提取 EAP 用户名。
 *
 * 顺序：AUTH_RULE_EAP_IDENTITY → AUTH_RULE_IDENTITY → ike_sa->get_other_id()。
 * @return		堆字符串（调用者 free）；取不到返回 NULL
 */
char *identity_get_username(ike_sa_t *ike_sa);

#endif /* IDENTITY_MANAGER_H_ */
/* 修改内容：创建身份获取接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

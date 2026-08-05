/* 修改内容：5.8.x 兼容：identification_t 无 to_string，用 get_encoding 获取字符串 修改人：pengjunlin 时间：2026-08-05 00:00:00 -- start ---- */
/* 修改内容：修复 EAP identity 获取：添加 get_other_eap_id() + ID 类型检查 + 修复 %.*H 格式符错误 修改人：pengjunlin 时间：2026-08-05 15:30:00 -- start ---- */
#include "identity_manager.h"
#include "cupp_log.h"

#include <credentials/auth_cfg.h>
#include <utils/identification.h>
#include <utils/chunk.h>
#include <utils/utils.h>

/*
 * 从 IKE_SA 提取 EAP 用户名。
 * 优先级：
 *   1. AUTH_RULE_EAP_IDENTITY（auth_cfg，EAP 认证完成后设置）
 *   2. ike_sa->get_other_eap_id()（IKE_SA 字段，EAP identity 收到即设置）
 *   3. AUTH_RULE_IDENTITY（auth_cfg，IKE IDi）
 *   4. ike_sa->get_other_id()（IKE_SA 字段，IDi）
 * 只有字符串类型（FQDN/RFC822/KEY_ID）的 encoding 才转为用户名；
 * IP 地址类型（ID_IPV4_ADDR 等）的 encoding 是二进制，直接跳过。
 * 返回堆字符串（调用者 free）；取不到返回 NULL。
 */
char *identity_get_username(ike_sa_t *ike_sa)
{
	auth_cfg_t *ac;
	identification_t *id = NULL;
	chunk_t enc;
	char *result = NULL;
	id_type_t id_type;

	if (!ike_sa)
	{
		cupp_warn("identity_get_username: ike_sa is NULL");
		return NULL;
	}

	cupp_log("identity_get_username: extracting username...");

	/* 1. 优先从 auth_cfg 取 AUTH_RULE_EAP_IDENTITY */
	ac = ike_sa->get_auth_cfg(ike_sa, FALSE);
	cupp_log("identity_get_username: auth_cfg=%p", (void*)ac);
	if (ac)
	{
		id = (identification_t*)ac->get(ac, AUTH_RULE_EAP_IDENTITY);
		cupp_log("identity_get_username: EAP_IDENTITY=%p", (void*)id);
	}

	/* 2. 直接从 IKE_SA 取 EAP identity（charon 内部也是用此方法打印 peer 名） */
	if (!id)
	{
		id = ike_sa->get_other_eap_id(ike_sa);
		cupp_log("identity_get_username: other_eap_id=%p", (void*)id);
	}

	/* 3. 回退到 AUTH_RULE_IDENTITY */
	if (!id && ac)
	{
		id = (identification_t*)ac->get(ac, AUTH_RULE_IDENTITY);
		cupp_log("identity_get_username: IDENTITY=%p", (void*)id);
	}

	/* 4. 最后回退到 get_other_id（注意：Windows IKEv2 客户端的 IDi 可能是 IP 地址） */
	if (!id)
	{
		id = ike_sa->get_other_id(ike_sa);
		cupp_log("identity_get_username: other_id=%p", (void*)id);
	}

	if (!id)
	{
		cupp_warn("identity_get_username: no identity available on IKE_SA");
		return NULL;
	}

	/* 检查 ID 类型：只有字符串类型才能作为用户名 */
	id_type = id->get_type(id);
	enc = id->get_encoding(id);
	cupp_log("identity_get_username: id_type=%d encoding_len=%u",
			 (int)id_type, enc.len);

	if (enc.len == 0 || !enc.ptr)
	{
		cupp_warn("identity_get_username: encoding empty");
		return NULL;
	}

	/* ID_FQDN / ID_RFC822_ADDR / ID_KEY_ID 的 encoding 即为原始字符串 */
	if (id_type == ID_FQDN || id_type == ID_RFC822_ADDR || id_type == ID_KEY_ID)
	{
		result = strndup((char*)enc.ptr, enc.len);
		cupp_log("identity_get_username: returning [%s] (type=%d)",
				 result ? result : "(null)", (int)id_type);
		return result;
	}

	/* ID_IPV4_ADDR/ID_IPV6_ADDR/ID_ASN1_DN 等类型编码为二进制，不能作为用户名 */
	cupp_warn("identity_get_username: id_type=%d is not a string type, skipping",
			  (int)id_type);
	return NULL;
}
/* 修改内容：修复 EAP identity 获取：添加 get_other_eap_id() + ID 类型检查 + 修复 %.*H 格式符错误 修改人：pengjunlin 时间：2026-08-05 15:30:00 -- end ---- */
/* 修改内容：5.8.x 兼容：identification_t 无 to_string，用 get_encoding 获取字符串 修改人：pengjunlin 时间：2026-08-05 00:00:00 -- end ---- */

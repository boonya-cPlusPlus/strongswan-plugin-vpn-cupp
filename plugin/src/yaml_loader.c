/* 修改内容：创建 YAML 加载实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
/* 修改内容：修正 switch(event.type) 用枚举：event.type 是 yaml_event_type_t，必须用 *_EVENT 后缀（上游 libyaml 在 yaml_token_type_t 和 yaml_event_type_t 两个 enum 里分别定义了 *_TOKEN 和 *_EVENT 两套值，不能混用）。注意 yaml_event_type_t 里没有 BLOCK_/FLOW_ 前缀（只有 YAML_MAPPING_START_EVENT/YAML_MAPPING_END_EVENT），BLOCK_/FLOW_ 前缀只存在于 yaml_token_type_t 修改人：pengjunlin 时间：2026-08-05 19:30:00 -- start ---- */
#include "yaml_loader.h"
#include "cupp_log.h"

#include <yaml.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
/* 修改内容：修正 switch(event.type) 用枚举：event.type 是 yaml_event_type_t，必须用 *_EVENT 后缀（上游 libyaml 在 yaml_token_type_t 和 yaml_event_type_t 两个 enum 里分别定义了 *_TOKEN 和 *_EVENT 两套值，不能混用）。注意 yaml_event_type_t 里没有 BLOCK_/FLOW_ 前缀（只有 YAML_MAPPING_START_EVENT/YAML_MAPPING_END_EVENT），BLOCK_/FLOW_ 前缀只存在于 yaml_token_type_t 修改人：pengjunlin 时间：2026-08-05 19:30:00 -- end ---- */

/*
 * libyaml 事件流解析 policy.yaml 的受限子集：
 *   users:
 *     <name>:
 *       ip: <ipv4>
 *   pool:
 *     start: <ipv4>
 *     end:   <ipv4>
 */
typedef enum {
	S_TOP,		/* 顶层 mapping */
	S_USERS,	/* users: 的值 mapping */
	S_USER,		/* 单个用户的 mapping */
	S_POOL,		/* pool: 的值 mapping */
} state_t;

int yaml_load_policy(const char *path, fixed_pool_t *fixed,
					 host_t **pool_start, host_t **pool_end)
{
	yaml_parser_t parser;
	yaml_event_t event;
	FILE *fp;
	state_t st = S_TOP;
	char *pending = NULL;		/* 待匹配的键 */
	char cur_user[256] = {0};
	int done = 0, rc = -1;

	if (!path || !fixed)
	{
		return -1;
	}
	fp = fopen(path, "r");
	if (!fp)
	{
		cupp_err("open policy %s failed: %s", path, strerror(errno));
		return -1;
	}
	if (!yaml_parser_initialize(&parser))
	{
		cupp_err("yaml_parser_initialize failed");
		fclose(fp);
		return -1;
	}
	yaml_parser_set_input_file(&parser, fp);

	while (!done)
	{
		if (!yaml_parser_parse(&parser, &event))
		{
			cupp_err("yaml parse error: %s",
					 parser.problem ? parser.problem : "unknown");
			goto cleanup;
		}
		switch (event.type)
		{
			case YAML_STREAM_END_EVENT:
			done = 1;
			break;

		/* 修改内容：YAML_BLOCK_MAPPING_START_EVENT 在 yaml_event_type_t 中不存在（event 枚举不区分 block/flow），改为 YAML_MAPPING_START_EVENT 修改人：pengjunlin 时间：2026-08-05 19:30:00 -- start ---- */
		case YAML_MAPPING_START_EVENT:
			/* 修改内容：YAML_BLOCK_MAPPING_START_EVENT 在 yaml_event_type_t 中不存在（event 枚举不区分 block/flow），改为 YAML_MAPPING_START_EVENT 修改人：pengjunlin 时间：2026-08-05 19:30:00 -- end ---- */
			if (st == S_TOP && pending)
				{
					if (streq(pending, "users"))
					{
						st = S_USERS;
					}
					else if (streq(pending, "pool"))
					{
						st = S_POOL;
					}
					free(pending);
					pending = NULL;
				}
				else if (st == S_USERS && pending)
				{
					/* 用户名键 → 进入该用户 mapping */
					snprintf(cur_user, sizeof(cur_user), "%s", pending);
					st = S_USER;
					free(pending);
					pending = NULL;
				}
				break;

			case YAML_MAPPING_END_EVENT:
				if (st == S_USER)
				{
					st = S_USERS;
					cur_user[0] = '\0';
				}
				else if (st == S_USERS)
				{
					st = S_TOP;
				}
				else if (st == S_POOL)
				{
					st = S_TOP;
				}
				break;

			case YAML_SCALAR_EVENT:
			{
				char *val = (char*)event.data.scalar.value;
				if (!pending)
				{
					pending = strdup(val);
				}
				else
				{
					/* (pending, val) 键值对 */
					if (st == S_USER && streq(pending, "ip"))
					{
						host_t *ip = host_create_from_string(val, 0);
						if (ip)
						{
							fixed->add(fixed, cur_user, ip);
							ip->destroy(ip);
						}
						else
						{
							cupp_warn("invalid ip '%s' for user '%s', skipped",
									  val, cur_user);
						}
					}
					else if (st == S_POOL && pool_start && pool_end)
					{
						if (streq(pending, "start"))
						{
							*pool_start = host_create_from_string(val, 0);
						}
						else if (streq(pending, "end"))
						{
							*pool_end = host_create_from_string(val, 0);
						}
					}
					free(pending);
					pending = NULL;
				}
				break;
			}

			default:
				break;
		}
		yaml_event_delete(&event);
	}

	rc = 0;

cleanup:
	if (pending)
	{
		free(pending);
	}
	yaml_parser_delete(&parser);
	fclose(fp);
	return rc;
}
/* 修改内容：创建 YAML 加载实现 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

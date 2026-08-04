/* 修改内容：创建 YAML 加载接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- start ---- */
#ifndef YAML_LOADER_H_
#define YAML_LOADER_H_

#include <library.h>
#include <networking/host.h>
#include "fixed_pool.h"

/**
 * 用 libyaml 解析 policy.yaml，填充 fixed_pool 与动态池端点。
 *
 * 仅识别受限子集：
 *   users:
 *     <name>:
 *       ip: <ipv4>
 *   pool:
 *     start: <ipv4>
 *     end:   <ipv4>
 *
 * @param path			policy.yaml 路径
 * @param fixed			填充固定 IP 表（host_t 引用 +1）
 * @param pool_start	输出动态池起始（借用，内部持有，调用者不释放）
 * @param pool_end		输出动态池结束
 * @return				0 成功；-1 解析/IO 错误
 */
int yaml_load_policy(const char *path, fixed_pool_t *fixed,
					 host_t **pool_start, host_t **pool_end);

#endif /* YAML_LOADER_H_ */
/* 修改内容：创建 YAML 加载接口 修改人：pengjunlin 时间：2026-08-04 16:42:14 -- end ---- */

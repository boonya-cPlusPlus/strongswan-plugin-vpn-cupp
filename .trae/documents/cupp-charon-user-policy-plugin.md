# CUPP（Charon User Policy Plugin）实现计划

## 摘要

按简化版 `README.md` 实现一个 StrongSwan charon 插件 `libcharon-user-policy.so`，通过 StrongSwan Plugin API 提供自定义 Virtual IP 分配策略，
替代默认地址池选择逻辑。vip 用户按 `policy.yaml` 分配固定 IP，普通用户从 CUPP 自维护的动态地址池分配，用户断开时即时释放回池。C11 + CMake + libyaml，零侵入 strongswan 核心。

核心是 `ip_allocator.c`：它协调 `fixed_pool` / `dynamic_pool` / `session_manager`，对外只暴露 `acquire(identity, ike_sa_id) → host_t*` 与 `release(ike_sa_id)`。

---

## 一、现状分析

### 1.1 仓库现状
- `e:\AI\C++\strongswan-plugin-vpn` 仅含 `README.md`（已简化为 CUPP 规格），无源码。
- README 模块结构未规定，沿用用户 /plan 消息指定的 9 文件结构。

### 1.2 README 关键约束（简化版，权威）
- 项目名：**Charon User Policy Plugin (CUPP)**
- 目标：vip 固定 IP / 普通用户走插件动态池 / 不用 RADIUS、LDAP、DB
- 技术栈：C11、StrongSwan Plugin API、CMake、Linux、YAML
- MVP 6 项：① 插件加载 ② 获取 EAP 用户名 ③ 加载 YAML 策略 ④ 固定 IP 分配 ⑤ 动态池分配 ⑥ **断开释放 IP**
- YAML schema（flat，权威）：
  ```yaml
  users:
    vip-admin:
      ip: 10.10.10.50
    vip-finance:
      ip: 10.10.10.51
  pool:
    start: 10.10.10.100
    end: 10.10.10.200
  ```
- 后续 v1.2 才做 Qt6/C++17 管理端 → 本次不创建任何 Qt 文件。

### 1.3 技术基础（已核实）
- 插件入口 `plugin_t`（`src/libstrongswan/plugins/plugin.h`）：`get_name/get_features/reload/destroy` + 构造函数 `<name>_plugin_create`，可用 `PLUGIN_DEFINE(name)` 宏。
- 地址分配：注册 `attribute_provider_t` 到 `charon->attributes->add_provider()`，在 `create_attribute_enumerator(ike_sa, ...)` 回调里返回 `configuration_attribute_t`（`INTERNAL_IP4_ADDRESS` 编码 1，值 4 字节 IP）。
- 释放钩子：`listener_t::ike_updown(ike_sa, up)`，`up=FALSE` 即 IKE_DOWN，标准回收点。
- 用户名：`ike_sa->get_auth_cfg(peer=TRUE)` 取 `AUTH_RULE_EAP_IDENTITY`（`identification_t*`），回退 `AUTH_RULE_IDENTITY` → `ike_sa->get_other_id()`。
- "替代默认分配"= 不在 `ipsec.conf` 设 `rightsourceip` + CUPP 作为唯一返回 IP 的 provider（`attr` 插件未配 `address` 不会返回 IP，天然不冲突）。

---

## 二、技术决策（基于 README 推导，已定）

| 决策点 | 选择 | 依据 |
|---|---|---|
| 核心技术问题 | IP 生命周期管理（释放可靠性） | README MVP #6 + "动态池耗尽"风险 |
| 崩溃恢复 | 纯内存 + `lease_timeout` 兜底 | README"无 DB"、最简、重启即清空无泄漏传播 |
| 固定 IP 恢复 | 无状态，每次从 policy.yaml 查 | 重启后自动恢复，无需持久化 |
| YAML schema | flat（README 权威） | 用户"README 简化了" |
| YAML 解析 | 系统 libyaml | 通用性强、健壮（用户先前确认） |
| SO 名 | `libcharon-user-policy.so` | 用户 /plan 指定 |
| 配置路径 | `/etc/strongswan/cupp/policy.yaml` | 用户 /plan 指定 |
| 分配模型 | CUPP 唯一 attribute_provider | "替代默认分配" |
| rekey IP 稳定 | 按用户名幂等，迁移 SA 绑定 | 同一用户重协商不应换 IP |
| 租约键 | `unique_id`（释放匹配）+ username（幂等索引） | 双索引覆盖 rekey 与释放 |
| 并发 | allocator/session_manager 互斥锁；policy 双 buffer | charon 多线程 |

---

## 三、目标架构

```
strongswan-cupp/
├── CMakeLists.txt                 # 顶层
├── plugin/
│   ├── CMakeLists.txt
│   └── src/
│       ├── plugin.c               # plugin_t 入口 + 特征注册 + listener 注册
│       ├── event_listener.c       # listener_t: ike_updown→release, ike_rekey→迁移
│       ├── event_listener.h
│       ├── identity_manager.c     # 从 ike_sa 取 EAP identity → username
│       ├── identity_manager.h
│       ├── policy_engine.c        # policy.yaml 加载/热重载/查询
│       ├── policy_engine.h
│       ├── ip_allocator.c         # 核心：协调 fixed/dynamic/session，acquire/release
│       ├── ip_allocator.h
│       ├── fixed_pool.c           # 固定 IP 表（username→host_t）
│       ├── fixed_pool.h
│       ├── dynamic_pool.c         # 动态池（start/end + 位图 + next-free）
│       ├── dynamic_pool.h
│       ├── session_manager.c      # 活跃租约（ike_sa_id→lease, username→lease）
│       ├── session_manager.h
│       ├── yaml_loader.c          # libyaml 封装
│       ├── yaml_loader.h
│       └── cupp_log.h             # 统一日志宏 [CUPP]
├── config/
│   └── policy.yaml                # README 示例
└── README.deploy.md               # 部署说明（中文，不改 README.md）
```

> README.md 保留原样；不创建 Qt 文件。

### strongswan.conf 接口

```ini
charon {
  load_modular = yes
  plugins {
    include <path>/libcharon-user-policy.so
    user-policy {
      policy = /etc/strongswan/cupp/policy.yaml   # 默认值
      lease_timeout = 3600                         # 兜底租约超时（秒）
      sweep_interval = 60                          # 清理扫描周期（秒）
    }
  }
}
```

> 插件名 `user-policy`（对应 `user_policy_plugin_create`）。ipsec.conf 的 conn 里**不设** `rightsourceip`，让 CUPP 独占分配。

---

## 四、模块职责

### plugin.c — 入口
- `PLUGIN_DEFINE(user_policy)` → `user_policy_plugin_create()`。
- `get_features`：`PLUGIN_REGISTER(PROVIDER, ip_allocator_as_provider)` + `PLUGIN_REGISTER(LISTENER, event_listener)`，依赖 charon。
- 构造时：`policy_engine_create()` → `ip_allocator_create(policy, ...)` → `event_listener_create(allocator)` → 注册到 `charon->bus->add_listener()` 与 `charon->attributes->add_provider()`。
- `reload`：`policy_engine_reload()`（SIGHUP / `ipsec rereadall`）。
- `destroy`：逆序注销并释放；启动 `sweep` 定时 `callback_job`，析构时取消。

### identity_manager.c — 取用户名
- `char *identity_get_username(ike_sa_t *sa)`：
  1. `auth_cfg_t *ac = sa->get_auth_cfg(sa, TRUE)`（peer=本地 FALSE？取对端认证用 `AUTH_CLASS_EAP`，需 `get_auth_cfg` 的 local=FALSE 表示对端）。
  2. `identification_t *id = ac->get(ac, AUTH_RULE_EAP_IDENTITY)`；为空回退 `AUTH_RULE_IDENTITY`。
  3. 再为空 `id = sa->get_other_id(sa)`。
  4. `id->to_string(id)` → 返回堆字符串（调用者 `free`）。
- 纯函数，无状态。

### policy_engine.c — 策略加载
- `policy_t *policy_load(const char *path)`。
- `host_t *policy_lookup_fixed(policy_t*, const char *user)` → 命中返回 `host_t*`（调用者不释放，内部持有），未命中 NULL。
- `bool policy_get_pool(policy_t*, host_t **start, host_t **end)`。
- `policy_t *policy_reload(policy_t*, const char *path)`：双 buffer 原子切换。
- 内部 `hashmap_t *users`（username→`host_t`），`chunk_hash/equals` 比对。
- 加载失败 fail-fast（插件构造返回 NULL，charon 拒绝加载插件并日志）。

### yaml_loader.c — libyaml 封装
- `int yaml_load_policy(const char *path, policy_t *out)`：`yaml_parser_set_input_file`，流式 event 解析。
- 仅识别：顶层 `users:`（map：name → `ip: x.x.x.x`）和 `pool:`（`start:` / `end:`）。
- 容错：未知键 warning 跳过；格式错误返回 -1。
- IP 用 `host_t::host_create_from_string` 解析，失败 -1。

### ip_allocator.c — 核心（属性提供者 + 协调器）
- 同时实现 `attribute_provider_t` 接口（对外是 provider，对内协调三个子模块）。
- `create_attribute_enumerator(ike_sa, vip_request)`：
  1. `username = identity_get_username(ike_sa)`；为空 → 返回空枚举（不分配）。
  2. `host = policy_lookup_fixed(username)`；命中 → `session_manager_bind(username, ike_sa_unique_id, host, fixed=TRUE)` → 返回 host。
  3. 未命中 → `dynamic_pool_acquire(session_manager)`：先查 `session_manager_by_user(username)` 命中返回（幂等）；否则 `dynamic_pool_next_free()` → `session_manager_bind(..., fixed=FALSE)`。
  4. 池满：返回空枚举 + `LOG` warning（无 IP 分配，连接失败，可观测）。
  5. 仅响应 `INTERNAL_IP4_ADDRESS`（MVP 仅 v4，v6 留接口）。
  6. 构造 `configuration_attribute_t`（`configuration_attribute_create_chunk`）放进 `linked_list_t`，返回枚举。
- `release(uint32_t unique_id)`：`session_manager_unbind(unique_id)` → 若是动态 IP，`dynamic_pool_release(host)`；固定 IP 不回池（只解绑）。
- `sweep()`：`session_manager_foreach`，对超 `lease_timeout` 且对应 `ike_sa` 已不存在（`charon->sas->get_by_unique_id` 返回 NULL）的租约执行 `release`。由 `callback_job` 周期触发。
- 内部 `pthread_mutex_t` 保护跨子模块的操作序列。

### fixed_pool.c — 固定 IP 表
- 仅 `hashmap` 封装：`fixed_pool_lookup(name)→host_t*`。无分配/回收逻辑（固定 IP 永远属于该用户）。
- 由 `policy_engine` 填充，`ip_allocator` 只读。

### dynamic_pool.c — 动态池
- `dynamic_pool_create(host_t *start, host_t *end)`。
- `host_t *next_free()`：从 `start` 起按 `uint32_t` 递增扫描位图，命中未置位 IP → 置位 + 返回 `host_create_from_chunk`。
- `release(host_t*)`：清位。
- `bool is_in_pool(host_t*)`：用于 release 时校验。
- 位图：`uint8_t *bits`，按 IP 偏移 bit。池大小 = end-start+1。
- `pthread_mutex_t` 内部自保护。

### session_manager.c — 活跃租约
- 双索引 `hashmap`：
  - `by_unique_id`：`uint32_t unique_id → lease_t*`（release 用）
  - `by_username`：`char* username → lease_t*`（幂等/rekey 用）
- `lease_t { char *username; uint32_t unique_id; host_t *ip; bool fixed; time_t acquired; }`。
- `lease_t *bind(username, unique_id, ip, fixed)`：若 `by_username` 已有活跃租约（rekey 场景），迁移 `unique_id` 到新值，复用 ip（仅动态 IP 需通知 `dynamic_pool` 不变；固定 IP 直接复用）。
- `lease_t *unbind(unique_id)` → 返回 lease 供 allocator 决定是否回池。
- `lease_t *lookup_by_user(username)`。
- `foreach_expiring(now, timeout, cb)`：供 sweep 回调。
- `pthread_mutex_t` 内部自保护。

### event_listener.c — 事件监听
- 实现 `listener_t`。
- `ike_updown(ike_sa, up)`：`up=FALSE` → `allocator->release(ike_sa->get_unique_id())`。返回 TRUE（继续传播）。
- `ike_rekey(ike_sa, new_sa)`（如存在该回调，否则靠 `ike_updown`+重新 `acquire` 兜底）：预绑定新 SA 的 unique_id 到同 username 的现有 lease（迁移），避免 rekey 瞬间 IP 抖动。MVP 可先靠 `session_manager` 的 username 幂等实现——新 SA 的 CP 回调会 `lookup_by_user` 命中并迁移，无需显式 rekey 钩子。**MVP 走幂等路径**，rekey 钩子列为可选优化。

---

## 五、核心流程

### 5.1 登录分配
```
EAP 认证成功 → charon 发 CP 请求 → CUPP create_attribute_enumerator
  → identity_get_username(ike_sa)
  → policy_lookup_fixed(user)
       命中 → session_manager.bind(user, uid, fixed_ip, fixed=TRUE) → 返回 fixed_ip
       未命中 → session_manager.lookup_by_user(user)
                  命中 → 迁移到新 uid, 返回原 ip（rekey/重连幂等）
                  未命中 → dynamic_pool.next_free()
                            有 → session_manager.bind(user, uid, ip, fixed=FALSE) → 返回 ip
                            满 → 返回空（连接失败，日志 warning）
```

### 5.2 断开释放
```
IKE_DOWN → listener.ike_updown(up=FALSE)
  → allocator.release(uid)
       → session_manager.unbind(uid) → lease
            lease.fixed? → 仅 free lease（固定 IP 不回池）
            否则 → dynamic_pool.release(lease.ip) + free lease
```

### 5.3 崩溃恢复
- charon 崩溃：内存丢失，重启后池全空，无泄漏传播。
- 运行期 SA 异常消失（无 IKE_DOWN）：`sweep()` 每 `sweep_interval` 秒扫 `session_manager`，对 `now - acquired > lease_timeout` 且 `charon->sas->get_by_unique_id(uid)==NULL` 的租约执行 release。
- 固定 IP 用户重连：policy.yaml 无状态查出原 IP，自动恢复。

### 5.4 热重载
- `ipsec rereadall` / SIGHUP → `plugin.reload` → `policy_engine_reload`（双 buffer 原子切换）。
- 不影响已分配租约；新连接用新策略。
- 删除某 vip 用户的固定 IP：已连用户保留至断开，新连接走动态池。

---

## 六、具体文件清单（均新增）

1. **`CMakeLists.txt`**（顶层）：`cmake_minimum_required(3.10)`、`project(charon-user-policy C)`、`add_subdirectory(plugin)`。
2. **`plugin/CMakeLists.txt`**：`pkg_check_modules(STRONGSWAN REQUIRED strongswan)` + `yaml-0.1`；编译 `libcharon-user-policy.so`（`PREFIX ""`、`C_STANDARD 11`、`-Wall -Wextra`）；链接 `strongswan` `yaml`；安装到 `${CMAKE_INSTALL_PREFIX}/lib/ipsec/plugins/`（可 `-DPLUGIN_DIR=...` 覆盖）。
3. **`plugin/src/plugin.c`**：`PLUGIN_DEFINE(user_policy)` + `plugin_t` 实现 + 子模块装配 + `sweep` job 启停。
4. **`plugin/src/identity_manager.c/.h`**：`identity_get_username`。
5. **`plugin/src/policy_engine.c/.h`**：`policy_load/lookup_fixed/get_pool/reload/destroy`。
6. **`plugin/src/yaml_loader.c/.h`**：`yaml_load_policy`。
7. **`plugin/src/ip_allocator.c/.h`**：核心，`attribute_provider_t` 实现 + `acquire/release/sweep`。
8. **`plugin/src/fixed_pool.c/.h`**：`fixed_pool_lookup`。
9. **`plugin/src/dynamic_pool.c/.h`**：`next_free/release/is_in_pool`。
10. **`plugin/src/session_manager.c/.h`**：`bind/unbind/lookup_by_user/foreach_expiring`。
11. **`plugin/src/event_listener.c/.h`**：`listener_t`，`ike_updown`。
12. **`plugin/src/cupp_log.h`**：`cupp_dbg/cupp_log` 宏（走 `DBG/LOG`，前缀 `[CUPP]`）。
13. **`config/policy.yaml`**：README 示例（vip-admin/vip-finance + pool 100-200）。
14. **`README.deploy.md`**（中文）：依赖（libyaml-dev、libstrongswan-dev）、编译安装、strongswan.conf 配置、ipsec.conf 不设 rightsourceip、`ipsec rereadall` 热重载、日志位置、权限。

### 不改动
- `README.md`（保留简化版原样）。
- StrongSwan 核心源码。
- 不创建 Qt 文件。

---

## 七、验证步骤

### 7.1 编译
```bash
cd plugin && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j
```
产出 `libcharon-user-policy.so`，无 warning。

### 7.2 单元测试（CTest）
- `policy_engine`：`policy_test.yaml` → `lookup_fixed("vip-admin")`==`10.10.10.50`，`lookup_fixed("unknown")`==NULL，`get_pool` 返回 100-200。
- `dynamic_pool`：池 `100-102`，连续 `next_free` 得 100/101/102，第四次 NULL；`release(100)` 后 `next_free`==100。
- `session_manager`：`bind("u1", uid=1, ip=100)`；`lookup_by_user("u1")`==ip=100；`bind("u1", uid=2, ip=100)`（rekey）→ `lookup_by_user` 迁移到 uid=2，ip 不变；`unbind(2)` 返回 lease。

### 7.3 集成验证（strongswan 环境）
1. 装 `libcharon-user-policy.so` 到 `/usr/lib/ipsec/plugins/`。
2. `strongswan.conf` 加载 `user-policy` + `policy.yaml` 放 `/etc/strongswan/cupp/`。
3. `ipsec.conf` 的 conn **不设** `rightsourceip`。
4. `ipsec restart`，`ipsec statusall` 确认 `user-policy` 在 loaded plugins。
5. Windows IKEv2 用 `vip-admin` 登录 → `ipconfig` 显示 `10.10.10.50`。
6. 普通用户 `user01` 登录 → `10.10.10.100`；断开后 `user02` 登录 → `10.10.10.100`（复用）；`user01` 再登录 → `10.10.10.101`（原 100 已被 user02 占）。
7. `user01` 登录后 `kill -9 $(pidof charon)` → 重启 → `user01` 再登录得 `10.10.10.100`（池清空，重新分配，无泄漏）。
8. `ipsec rereadall` 后修改 policy.yaml 的 vip-admin IP → 新连接用新 IP。

### 7.4 日志
- 启动：`[CUPP] loaded policy: 2 fixed users, pool 10.10.10.100-200`。
- 分配：`[CUPP] user=vip-admin -> 10.10.10.50 (fixed)` / `[CUPP] user=user01 -> 10.10.10.100 (pool)`。
- 释放：`[CUPP] release uid=42 user=user01 ip=10.10.10.100 (pool, recycled)`。
- 清理：`[CUPP] sweep: expired uid=7 user=stale (recycled)`。

---

## 八、实施顺序

1. 顶层 + `plugin/CMakeLists.txt`（打通编译，链接 strongswan/yaml）。
2. `cupp_log.h` + `plugin.c`（最小可加载，`ipsec statusall` 见 `user-policy`）。
3. `yaml_loader.c` + `policy_engine.c` + `config/policy.yaml`（独立可测）。
4. `fixed_pool.c` + `dynamic_pool.c` + `session_manager.c`（独立可测）。
5. `identity_manager.c`（取 EAP identity）。
6. `ip_allocator.c`（串联：provider 接口 + acquire/release/sweep）。
7. `event_listener.c`（ike_updown → release）+ 在 `plugin.c` 注册 listener。
8. `README.deploy.md`。
9. 单元测试 + 集成验证清单。

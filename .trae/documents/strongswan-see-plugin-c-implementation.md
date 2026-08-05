# StrongSwan SEE 插件实现计划（C11）

## 摘要

按 README.md 规格实现一个 StrongSwan 增强插件 `libstrongswan-see.so`，解决 Windows IKEv2 VPN 用户无法按账号绑定固定虚拟 IP 的问题。插件在 EAP 认证成功、charon 进行 CP（Configuration Payload）分配虚拟 IP 时介入：根据 `ike_sa` 的认证身份读取用户名 → 查询 `policy.yaml` → 命中则返回该用户的固定 IP；未命中则从 SEE 自管的动态地址池分配。采用 C11 + CMake + libyaml，零侵入 StrongSwan 核心。

---

## 一、现状分析

### 1.1 项目当前状态
- 仓库 `e:\AI\C++\strongswan-plugin-vpn-cupp` 目前仅含 `README.md`（ChatGPT 生成），无任何源码。
- README 规定的目录结构与本次实现目标一致：`plugin/`、`config/`、`CMakeLists.txt`。`qt-console/` 属 v1.2 范围，本次 MVP 不实现。

### 1.2 README 与用户意图的差异
- README 写“开发语言：C11”、源文件为 `.c`。用户已确认“C 就 C 吧”，**本次以 C11 实现**。
- README 的 MVP 范围：① 插件加载 ② 获取用户名 ③ 读 YAML ④ 固定 IP 分配。用户进一步要求**普通用户动态池也由 SEE 自管**（README 中 `default.pool` 不再交给 strongswan 原生池），因此动态池分配是本次必做项。

### 1.3 技术基础（已核实）
- StrongSwan 插件接口 `plugin_t`（`src/libstrongswan/plugins/plugin.h`）：需实现 `get_name / get_features / reload / destroy`，构造函数名为 `see_plugin_create`，可用 `PLUGIN_DEFINE(see)` 宏生成版本符号。
- StrongSwan `attr` 插件官方文档已确认：属性通过 CP（IKEv2）/ Mode Config（IKEv1）下发，**仅当对端请求虚拟 IP 时**才分配。
- 自定义固定 IP 需注册 `attribute_provider_t`（libcharon），在 `create_attribute_enumerator` 回调中按 `ike_sa` 的 `auth_cfg` 取 EAP 身份。`cp_payload_t` 携带 `configuration_attribute_t`，`INTERNAL_IP4_ADDRESS` 编码为 1。

### 1.4 关键决策（用户已拍板）
| 决策点 | 选择 | 理由 |
|---|---|---|
| 动态地址池管理方 | SEE 自管 | 配置集中、行为可观测 |
| YAML 解析 | 系统 libyaml | 健壮、符合规范、通用性强 |
| 语言 | C11 | 与 README 一致，strongswan 原生插件均为 C |
| 通用性 | 优先 | 跨发行版可编译、可热重载、状态可持久化 |

---

## 二、目标架构

```
strongswan-see/
├── plugin/
│   ├── CMakeLists.txt
│   ├── src/
│   │   ├── see_plugin.c       # 插件入口：plugin_t 实现 + 特征注册
│   │   ├── see_provider.c     # attribute_provider_t 实现：取用户名→查策略→返回IP
│   │   ├── see_provider.h
│   │   ├── policy.c           # policy.yaml 加载/热重载/查询
│   │   ├── policy.h
│   │   ├── yaml_loader.c      # libyaml 封装：把 policy.yaml 解析成 policy_t
│   │   ├── yaml_loader.h
│   │   ├── ip_allocator.c     # 固定IP直配 + 动态池 next-free + 租约持久化
│   │   ├── ip_allocator.h
│   │   └── see_log.h          # 统一 dbg() 宏，走 strongswan 的 DBG/LOG 通道
│   └── config/
│       └── see.conf.snippet   # strongswan.conf 片段示例
├── config/
│   └── policy.yaml            # 用户策略样例（README 示例）
├── CMakeLists.txt             # 顶层
└── README.deploy.md           # 部署说明（仅本插件，不动 README.md）
```

> 说明：README.md 已存在且为 ChatGPT 产出，不改写；新增 `README.deploy.md` 专写部署。不创建任何 Qt 相关文件（v1.2 范围）。

### 2.1 模块职责

**see_plugin.c** — 插件入口
- 用 `PLUGIN_DEFINE(see)` 定义 `see_plugin_create()`。
- `get_features` 通过 `PLUGIN_REGISTER(PROVIDER, see_provider_create)` 等宏声明：注册一个 `attribute_provider_t`，依赖 `charon`（libcharon）。
- `reload`：触发 `policy_reload()`（SIGHUP / `ipsec rereadall` 时调用）。
- `destroy`：释放 provider、policy、allocator。

**see_provider.c** — 属性提供者（核心）
- 实现 `attribute_provider_t` 接口。
- 关键回调 `create_attribute_enumerator(ike_sa, pool, vip, ...)`：
  1. 从 `ike_sa->get_auth_cfg(peer, TRUE)` 取 `AUTH_RULE_EAP_IDENTITY` 或 `AUTH_RULE_IDENTITY`，得到用户名（`identification_t`）。
  2. 调 `policy_lookup(username)` → 命中固定 IP：返回该 `host_t`。
  3. 未命中：调 `ip_allocator_acquire(username, ike_sa_id)` 从动态池分配；分配失败返回 NULL（让其他 provider 或原生池兜底，避免单点故障）。
  4. 仅响应 `INTERNAL_IP4_ADDRESS` / `INTERNAL_IP6_ADDRESS` 请求，其它属性透传。
- 线程安全：policy 查询走 RCU 风格（读无锁、重载时双 buffer 切换）；allocator 内部 `pthread_mutex`。

**policy.c / yaml_loader.c** — 策略加载
- `yaml_loader` 用 libyaml 的 `yaml_parser_t` 流式解析，仅识别 SEE 所用的受限子集：顶层 `users:`（map：用户名→`ip: x.x.x.x`）和 `default:`（含 `pool: a-b`）。
- `policy_t` 结构：固定 IP 用 `hashmap_t *users`（用户名→`host_t`）；`default_pool` 存起止 `host_t`。
- 加载完原子替换全局指针（`pthread_mutex` + 双 buffer），旧 policy 引用计数归零后销毁。
- 配置路径：默认 `/etc/ipsec/see/policy.yaml`，可通过 `strongswan.conf` 的 `charon.plugins.see.policy` 覆盖（用 strongswan 的 `settings` API 读取）。
- 热重载：`reload()` 回调 + 每次 `policy_lookup` 前比对 `stat()` 的 mtime（廉价，<1µs），变更则后台 `job` 异步重载，绝不阻塞 CP 回调。

**ip_allocator.c** — 动态池 + 租约
- 池表示：起始/结束 `host_t`，内部用 `uint32_t` 位图或 `linked_list_t` 跟踪已分配 IP。
- `acquire(username, ike_sa_id)`：
  1. 先查租约表（`hashmap`：`ike_sa_id` → `host_t`），同一 SA 重协商返回原 IP（幂等）。
  2. 未租：next-free 扫描，命中未分配 IP 即租出。
  3. 池满：返回 NULL，记 `LOG` warning。
- `release(ike_sa_id)`：在 `ike_sa_t` 的 `destroy` 钩子或 charon 的 `ike_updown` 事件回调中触发，回收 IP。注册方式：在 provider 里订阅 `charon->ike->register_init`/`register_destroy`，或用 `callback_job` 轮询清理超时租约（兜底）。
- 持久化：租约表每次变更追加写到 `/var/lib/strongswan-see/leases.state`（行格式：`<ike_sa_id_hex> <ip>`），启动时读回。崩溃恢复用 mtime + 内容校验。**默认开启，可通过 `charon.plugins.see.persist = no` 关闭**。

### 2.2 配置接口（strongswan.conf）

```ini
charon {
  load_modular = yes
  plugins {
    include <path-to>/libstrongswan-see.so
    see {
      policy = /etc/ipsec/see/policy.yaml   # 默认值
      persist = yes                          # 租约持久化
      lease_timeout = 86400                  # 兜底租约超时（秒）
    }
  }
}
```

---

## 三、具体文件改动

### 新增文件（均为必需）

1. **`plugin/CMakeLists.txt`**
   - `find_package(PkgConfig REQUIRED)`，`pkg_check_modules(STRONGSWAN REQUIRED strongswan)` 与 `libyaml`。
   - 取 strongswan 头文件目录（`pkg-config --variable=includedir strongswan`，实际为 `src/libstrongswan` + `src/libcharon` 安装路径）。
   - 编译 `libstrongswan-see.so`，链接 `strongswan`、`charon`（如分离）、`yaml`。
   - `set_target_properties(... PROPERTIES PREFIX "" C_STANDARD 11 C_STANDARD_REQUIRED ON)`。
   - 安装目标：`libstrongswan-see.so` → `${CMAKE_INSTALL_PREFIX}/lib/ipsec/plugins/`（可由 `-DPLUGIN_DIR=...` 覆盖）。

2. **`plugin/src/see_plugin.c`**
   - `PLUGIN_DEFINE(see)` + `plugin_t` 结构体实现。
   - `get_features` 返回 `PLUGIN_REGISTER(PROVIDER, see_provider_create)`，并 `PLUGIN_SDEPEND()` 声明对 charon 的依赖。
   - 持有 `see_provider_t *`、`policy_t *`、`ip_allocator_t *` 子模块指针，`destroy` 逆序释放。

3. **`plugin/src/see_provider.c` / `.h`**
   - `see_provider_t` 继承 `attribute_provider_t`。
   - `create_attribute_enumerator`：核心逻辑见 §2.1。
   - `get_address` / `release_address`（IKEv1 mode-config 走这两个）。
   - 构造时注册到 `charon->attributes->add_provider()`，析构时 `remove_provider()`。

4. **`plugin/src/policy.c` / `.h`**
   - `policy_t *policy_load(const char *path)`；`host_t *policy_lookup(policy_t*, const char *user)`；`bool policy_get_pool(policy_t*, host_t **start, host_t **end)`；`policy_t *policy_reload(policy_t*, const char *path)`；`void policy_destroy(policy_t*)`。
   - 内部 `hashmap_t` 用 `chunk_hash` + `chunk_equals` 比较用户名。
   - mtime 比对 + 双 buffer 切换。

5. **`plugin/src/yaml_loader.c` / `.h`**
   - `int yaml_load_policy(const char *path, policy_t *out)`：用 `yaml_parser_set_input_file`，逐 `event` 解析。
   - 容错：未知键记 warning 跳过；格式错误返回 -1 并日志。

6. **`plugin/src/ip_allocator.c` / `.h`**
   - `ip_allocator_t *ip_allocator_create(host_t *start, host_t *end, const char *state_file, bool persist, uint32_t timeout)`。
   - `host_t *acquire(ip_allocator_t*, const char *user, uint64_t ike_sa_id)`；`void release(ip_allocator_t*, uint64_t ike_sa_id)`。
   - `pthread_mutex_t` 保护；位图用 `uint8_t *` 数组按 bit 分配。
   - 持久化：`fopen(state_file, "a")` 追加，定期（每 N 次变更或 `release` 时）全量重写压缩。

7. **`plugin/src/see_log.h`**
   - 宏 `see_dbg(level, fmt, ...)` → `DBG(level, ...)`；`see_log(level, fmt, ...)` → `LOG(level, ...)`。统一前缀 `[SEE]`。

8. **`plugin/config/see.conf.snippet`** — 上述 §2.2 配置片段。

9. **`config/policy.yaml`** — README 示例直接落地（vip-admin/finance/ops + default.pool）。

10. **`CMakeLists.txt`**（顶层）— `add_subdirectory(plugin)`，`cmake_minimum_required(VERSION 3.10)`，`project(strongswan-see C)`。

11. **`README.deploy.md`** — 编译（依赖 libyaml-dev、libstrongswan-dev、libcharon-dev）、安装路径、strongswan.conf 配置、`ipsec reload` / `ipsec rereadall` 热重载、状态文件权限、SELinux/AppArmor 提示。**中文**。

### 不改动
- `README.md`（用户明确说是 ChatGPT 出的，保留原样）。
- StrongSwan 核心源码（README 硬性要求）。
- 不创建任何 Qt 文件（v1.2 范围外）。

---

## 四、关键实现要点与风险

### 4.1 用户名获取（最容易踩坑）
- IKEv2 EAP-MSCHAPv2 场景：用户名在 `auth_cfg` 的 `AUTH_RULE_EAP_IDENTITY`（`identification_t *`，类型多为 `ID_KEY_ID` 或 `ID_RFC822_ADDR`）。
- 备选：`AUTH_RULE_IDENTITY`（对端声明的 identity，如 `rightid`）。
- 实现：先取 `EAP_IDENTITY`，为空回退 `IDENTITY`，再为空回退 `ike_sa->get_other_id()`。
- 字符串化用 `identification_t->get_encoding()` 或 `->to_string()`。

### 4.2 线程安全
- charon 多线程，CP 回调可能并发。policy 读多写极少 → 双 buffer + 原子指针切换；allocator → 互斥锁。
- 绝不在 CP 回调里做阻塞 IO；热重载走 `callback_job_t` 异步。

### 4.3 租约回收
- 首选：订阅 charon 的 `ike_sa_t::destroy`（通过 `attribute_provider_t::release_address`，IKEv2 有标准回调）。
- 兜底：`lease_timeout` 定时清理（`libstrongswan` 的 `job` 机制，每 60s 扫一次）。
- 防双分配：`acquire` 先按 `ike_sa_id` 查租约，命中直接返回原 IP。

### 4.4 兼容性
- 仅用 `PLUGIN_REGISTER(PROVIDER, ...)` 特征，5.6+ 均支持（README 要求“保持版本兼容”）。
- 头文件用 `#include <strongswan.h>` 标准入口；libcharon 符号通过运行时 dlsym 由 strongswan 框架注入，不直接链接 `libcharon.so`（避免版本符号冲突）。
- CMake 用 `pkg-config` 而非硬编码路径，跨 Debian/Ubuntu/RHEL/Arch 通用。

### 4.5 失败兜底
- policy 加载失败：插件启动失败并日志（fail-fast，避免静默用错 IP）。
- 动态池满：返回 NULL，让 strongswan 原生 `rightsourceip` 兜底（即使 SEE 自管池，也建议 ipsec.conf 保留 `rightsourceip` 作 second-chance，部署文档写明）。

---

## 五、假设与决策

1. **目标 strongswan 版本**：5.9 LTS（与 `attr` 文档版本一致），向后兼容 5.6+。
2. **目标平台**：Linux x86_64（README 指定 Linux）。Windows 不支持 strongswan 服务端。
3. **配置路径默认值**：`/etc/ipsec/see/policy.yaml`、`/var/lib/strongswan-see/leases.state`，均可在 `strongswan.conf` 覆盖。
4. **租约持久化格式**：简单文本行（`<ike_sa_id_hex> <ip>`），不引入 SQLite 等额外依赖（MVP 保持轻量）。
5. **IPv6 支持**：`ip_allocator` 设计上支持 v4/v6，但 MVP 仅 v4 落地，v6 留接口（README 示例仅 v4）。
6. **不实现**：ACL、Web 管理、数据库、RADIUS、LDAP、Qt 控制台（均 README 明确列为后续）。

---

## 六、验证步骤

### 6.1 编译验证
```bash
cd plugin && mkdir build && cd build
cmake -DCMAKE_INSTALL_PREFIX=/usr ..
make -j
```
预期产出 `libstrongswan-see.so`，无 warning（`-Wall -Werror`）。

### 6.2 单元测试（轻量）
- `policy.c`：构造 `policy_test.yaml`，断言 `policy_lookup("vip-admin")` 返回 `10.10.10.50`，`policy_lookup("unknown")` 返回 NULL。
- `ip_allocator.c`：池 `10.10.10.100-102`，连续 `acquire` 三次得 100/101/102，第四次返回 NULL；`release` 后再 `acquire` 复用。
- 用 strongswan 的 `testing/` 测试框架或独立 `test_*.c` + `CTest`。

### 6.3 集成验证（需要 strongswan 环境）
1. 安装 `libstrongswan-see.so` 到 `/usr/lib/ipsec/plugins/`。
2. `strongswan.conf` 加载 `see` 插件 + `policy.yaml` 放 `/etc/ipsec/see/`。
3. `ipsec restart`，`ipsec statusall` 确认 `see` 在 loaded plugins 列表。
4. Windows IKEv2 客户端用 `vip-admin` 登录 → `ipconfig` 应显示 `10.10.10.50`。
5. 普通用户 `user01` 登录 → 应得 `10.10.10.100`，第二次登录同一用户保持 `10.10.10.100`（幂等）。
6. `kill -HUP $(pidof charon)` → 修改 `policy.yaml` 的 IP → 重新连接验证热重载生效。
7. `charon` 崩溃重启 → `leases.state` 恢复租约，已连用户重连得原 IP。

### 6.4 日志验证
- `charon.log` 出现 `[SEE] loaded policy: 3 users, pool 10.10.10.100-200`。
- 登录时 `[SEE] user=vip-admin -> 10.10.10.50 (fixed)` 或 `[SEE] user=user01 -> 10.10.10.100 (pool)`。

---

## 七、实施顺序（建议 todo 列表）

1. 顶层 `CMakeLists.txt` + `plugin/CMakeLists.txt`（先打通编译骨架，链接 strongswan/yaml）。
2. `see_log.h` + `see_plugin.c`（最小可加载插件，`ipsec statusall` 能看到 `see`）。
3. `yaml_loader.c` + `policy.c` + `config/policy.yaml`（独立可测）。
4. `ip_allocator.c`（独立可测）。
5. `see_provider.c`（串联：取用户名→查 policy→allocator）。
6. `see.conf.snippet` + `README.deploy.md`。
7. 单元测试 + 集成验证清单。

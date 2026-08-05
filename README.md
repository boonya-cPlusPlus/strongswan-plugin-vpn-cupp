# CUPP — Charon User Policy Plugin

![CUPP插件](images/logo.png)

一个 strongSwan charon 插件，用 EAP 用户名驱动虚拟 IP 分配：VIP 用户始终拿到固定 IP，普通用户从插件自维护的动态地址池按需借还。**不依赖 FreeRADIUS / LDAP / 数据库**，策略由一份 YAML 描述。

- 产物：`libstrongswan-user-policy.so`
- 语言：C11
- 接入方式：strongSwan Plugin API（`attribute_provider_t` + `listener_t`）
- 配置：`/etc/strongswan/cupp/policy.yaml`
- 适用场景：Windows IKEv2 客户端需要按账号绑定固定 VPN 内网 IP

---

## 为什么需要它

strongSwan 原生的虚拟 IP 分配只有两条路：

1. **内存池 / SQL 池**：按 IKE identity 做亲和性，无法按 EAP 用户名精确绑定固定 IP，也无显式释放机制。
2. **RADIUS / DHCP 后端**：要额外部署 FreeRADIUS 或 DHCP 服务器，运维成本高。

CUPP 填补中间地带——在 charon 进程内用一份 YAML 完成「用户名 → 固定 IP / 动态池」的策略管理，登录即分配、断开即回收，零外部依赖。

---

## 核心特性

| 特性 | 说明 |
|---|---|
| 固定 IP 绑定 | `policy.yaml` 中为指定用户声明固定 IP，登录即得、断开后他人不可占用 |
| 动态地址池 | 位图跟踪的内存池，按起止区间自动分配 / 回收，支持复用 |
| 地址生命周期 | `acquire_address` 分配 + `release_address` / `ike_updown` 双路径释放，幂等安全 |
| 孤儿租约回收 | 周期 sweep 任务扫描超时未释放的租约，避免异常断开导致池耗尽 |
| 策略热重载 | `ipsec rereadall` 触发双 buffer 原子切换，已连用户不打断 |
| Fail-fast 加载 | `policy.yaml` 缺失或格式错误时插件拒绝加载，避免空策略运行 |
| 线程安全 | 固定表 / 动态池 / 会话表均用 `pthread_mutex` 保护 |

---

## 工作原理

```
登录：Windows IKEv2 EAP 成功
        ↓
charon 调 acquire_address(pools=cupp, ike_sa, requested)
        ↓
CUPP 取 EAP 用户名 → 查固定表命中？→ 返回固定 IP（克隆）
                                  └→ 未命中 → 从动态池分配
        ↓
绑定租约（username + unique_id + ip + acquired）
        ↓
返回虚拟 IP 给客户端

断开：IKE_SA down
        ↓
listener.ike_updown(up=FALSE) → release_by_uid(unique_id)
        + charon 调 release_address(address, ike_sa)
        ↓
解绑租约 → 动态 IP 回收池（固定 IP 不回池）

兜底：周期 sweep（默认 60s）
        ↓
扫到 acquired 超过 lease_timeout 且对应 ike_sa 已消失
        ↓
强制回收（防异常断开导致池耗尽）
```

---

## 项目结构

```
strongswan-plugin-vpn-cupp/
├── CMakeLists.txt                 # 顶层构建入口
├── README.md                      # 本文件
├── README.deploy.md               # 详细部署说明
├── etc/ipsec.d/
│   └── policy.yaml                # 策略配置示例
└── plugin/
    ├── CMakeLists.txt             # 插件构建（支持 pkg-config 与源码头文件两种模式）
    └── src/
        ├── plugin.c               # 插件入口：user_policy_plugin_create + 特征注册 + sweep job
        ├── ip_allocator.c/.h      # 核心：attribute_provider_t 实现（acquire/release/sweep）
        ├── policy_engine.c/.h     # 策略加载 + 双 buffer 热重载
        ├── event_listener.c/.h    # listener_t.ike_updown 兜底释放
        ├── identity_manager.c/.h  # 从 ike_sa 提取 EAP 用户名
        ├── fixed_pool.c/.h        # 固定 IP 表（hashtable）
        ├── dynamic_pool.c/.h      # 动态 IP 池（位图）
        ├── session_manager.c/.h   # 租约管理（by_uid + by_user 双索引）
        ├── yaml_loader.c/.h       # libyaml 解析 policy.yaml
        └── cupp_log.h             # 统一日志宏（[CUPP] 前缀）
```

---

## 快速开始

### 1. 编译

```bash
# 依赖
sudo apt install build-essential cmake pkg-config libyaml-dev libstrongswan

# 方式 A：发行版有 libstrongswan-dev 包
sudo apt install libstrongswan-dev
cd strongswan-plugin-vpn-cupp
mkdir build && cd build
cmake ..
make -j$(nproc)

# 方式 B：发行版无开发包（如 Ubuntu 20.04 focal），用源码头文件
wget https://download.strongswan.org/strongswan-5.8.2.tar.bz2
tar -xjf strongswan-5.8.2.tar.bz2
cd strongswan-plugin-vpn-cupp
mkdir build && cd build
cmake -DSTRONGSWAN_SRC=$HOME/strongswan-5.8.2 ..
make -j$(nproc)
```

产物：`plugin/libstrongswan-user-policy.so`

### 2. 部署

详见 [README.deploy.md](README.deploy.md)。关键步骤：

```bash
sudo make install                                              # 装 .so 与示例 policy.yaml
sudo cp config/policy.yaml /etc/strongswan/cupp/policy.yaml    # 或就地编辑

# strongswan.conf 加载插件
# swanctl.conf 连接配置 pools = cupp
sudo ipsec restart
```

### 3. 验证

```bash
sudo ipsec statusall | grep user-policy      # 插件已加载
# Windows IKEv2 用 vip-admin 登录 → ipconfig 看到 10.10.10.50
sudo journalctl -u strongswan -f | grep CUPP # 实时日志
```

---

## 配置示例

### policy.yaml

```yaml
# 固定 IP 用户：登录即得，断开后他人不可占用
users:
  vip-admin:
    ip: 10.10.10.50
  vip-finance:
    ip: 10.10.10.51
  vip-ops:
    ip: 10.10.10.52

# 动态地址池：未命中固定表的用户从此分配（含端点）
pool:
  start: 10.10.10.100
  end:   10.10.10.200
```

### strongswan.conf（CUPP 自身参数，均有默认值）

```ini
charon {
    load_modular = yes
    plugins {
        user-policy {
            policy         = /etc/strongswan/cupp/policy.yaml
            pool_name      = cupp           # 需与 swanctl.conf 中 pools = <name> 一致
            lease_timeout  = 3600           # 孤儿租约超时秒数
            sweep_interval = 60             # sweep 周期秒数
        }
    }
}
```

### swanctl.conf（连接引用 CUPP 池）

```conf
connections {
    ike-vpn {
        version = 2
        local_addrs  = <服务器公网 IP>
        remote_addrs = %any
        pools = cupp                    # 关键：引用 CUPP 池名，不要再配 pools.<name>.addrs
        local  { auth = pubkey; certs = serverCert.pem; id = <服务器证书 Subject> }
        remote { auth = eap-mschapv2; eap_id = %any }
        children {
            ike-vpn {
                mode = tunnel
                local_ts  = 0.0.0.0/0
                remote_ts  = dynamic     # 由 CUPP 分配的 VIP 自动收窄
            }
        }
    }
}
```

---

## 技术要点

### 接入点：`attribute_provider_t`

CUPP 实现 strongSwan 的 `attribute_provider_t` 接口，注册到 `charon->attributes`：

- `acquire_address(pools, ike_sa, requested)` —— 连接配置 `pools = cupp` 时被调用，CUPP 识别池名后按策略分配
- `release_address(pools, address, ike_sa)` —— SA 断开时被调用，回收动态 IP
- `create_attribute_enumerator` —— 返回空，DNS 等属性留给 `attr` 插件

### 生命周期双保险

`release_address` 与 `listener.ike_updown(up=FALSE)` 都会调 `release_by_uid(unique_id)`，**幂等**：租约释放后从会话表移除，重复调用安全。

### Sweep 兜底

周期 `callback_job`（默认 60s）扫描会话表：对 `acquired` 超过 `lease_timeout` 且对应 `ike_sa` 已不存在的租约强制回收，防止异常断开（如客户端崩溃、网络中断）导致动态池泄漏。

### 插件命名约定

strongSwan `plugin_loader` 按文件名 `libstrongswan-<name>.so` 查找，构造符号为 `<name>_plugin_create`（连字符转下划线）：

- 插件名：`user-policy`
- 文件：`libstrongswan-user-policy.so`
- 符号：`user_policy_plugin_create`

### 引用计数与卸载安全

sweep `callback_job` 持有 plugin 实例的引用，卸载时通过 `terminating` 标志通知 job 停止重排，job cleanup 时释放最后一个 ref，避免 use-after-free。

---

## 日志

所有日志带 `[CUPP]` 前缀，通过 strongSwan 的 `DBG1` / `DBG2` 输出到 charon 日志通道。

```
[CUPP] plugin loaded (policy=/etc/strongswan/cupp/policy.yaml pool_name=cupp)
[CUPP] policy loaded: 3 fixed users, pool 10.10.10.100-10.10.10.200
[CUPP] acquire uid=42 user=vip-admin -> 10.10.10.50 (fixed)
[CUPP] acquire uid=43 user=normal-user -> 10.10.10.100 (pool)
[CUPP] release uid=43 user=normal-user ip=10.10.10.100 (pool, recycled)
[CUPP] sweep: expired uid=7 user=stale ip=10.10.10.101 (recycled)
[CUPP] policy reloaded: 3 fixed users, pool 10.10.10.100-10.10.10.200
```

---

## 兼容性

- strongSwan >=5.8.2+
- Linux（Debian/Ubuntu/CentOS 等）
- 客户端：Windows 10/11 IKEv2、iOS/macOS IKEv2、strongSwan 客户端

---

## 文档

- 部署与运维：[README.deploy.md](README.deploy.md)
- 设计文档：[.trae/documents/cupp-charon-user-policy-plugin.md](.trae/documents/cupp-charon-user-policy-plugin.md)

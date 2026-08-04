# CUPP 部署说明（Charon User Policy Plugin）

本文件指导如何编译、安装并启用 CUPP 插件，让 strongSwan 的 charon 守护进程按
`policy.yaml` 为 EAP 用户分配固定/动态虚拟 IP。

> 整体流程：编译插件 → 安装到 strongSwan 插件目录 → 告诉 charon 加载插件 →
> 提供 `policy.yaml` → 启动 VPN → Windows IKEv2 登录 → 插件接管 Virtual IP 分配。

---

## 一、环境依赖

以 Debian/Ubuntu 为例：

```bash
# 编译工具链
sudo apt install build-essential cmake pkg-config

# strongSwan 开发头文件（提供 plugin.h / attribute_provider.h / daemon.h 等）
sudo apt install libstrongswan-dev libstrongswan-standard-plugins

# libyaml（解析 policy.yaml）
sudo apt install libyaml-dev

# strongSwan 本体（运行时）
sudo apt install strongswan strongswan-charon strongswan-swanctl
```

> 要求 strongSwan ≥ 5.9（本插件基于 5.9.x 的 `attribute_provider_t` 接口开发）。
> 验证：`ipsec version` 应输出 5.9.x。

---

## 二、编译

```bash
cd /path/to/strongswan-plugin-vpn
mkdir build && cd build

# 默认安装到 /usr，插件目录 /usr/lib/ipsec/plugins
# 若 strongSwan 装在 /usr/lib/x86_64-linux-gnu 下，按需覆盖 PLUGIN_DIR
cmake -DCMAKE_INSTALL_PREFIX=/usr \
      -DPLUGIN_DIR=/usr/lib/ipsec/plugins ..

make -j$(nproc)
```

编译产物：`plugin/libstrongswan-user-policy.so`。

> strongSwan 的 `plugin_loader` 按文件名 `libstrongswan-<name>.so` 查找插件，
> 并按 `<name>_plugin_create`（连字符转下划线）查找构造符号。
> 本插件名为 `user-policy` → 文件 `libstrongswan-user-policy.so` →
> 符号 `user_policy_plugin_create`。

---

## 三、安装

```bash
sudo make install
```

安装内容：
- `libstrongswan-user-policy.so` → `$PLUGIN_DIR`
- `policy.yaml` 示例 → `/etc/strongswan/cupp/policy.yaml`

确认插件就位：

```bash
ls -l /usr/lib/ipsec/plugins/libstrongswan-user-policy.so
```

---

## 四、配置 strongSwan

### 4.1 `strongswan.conf` —— 加载插件

在 `/etc/strongswan.conf`（或 `strongswan.d/charon.conf`）中确保：

```ini
charon {
    load_modular = yes
    plugins {
        include <path>/libstrongswan-user-policy.so
    }
}
```

> `load_modular = yes` 时，charon 按 `charon.plugins.user-policy` 段读取本插件配置，
> 无需手动 `include`（`include` 仅用于显式指定 .so 路径，二者择一即可）。

CUPP 自身参数（均有默认值，可省略）：

```ini
charon {
    plugins {
        user-policy {
            # policy.yaml 路径
            policy = /etc/strongswan/cupp/policy.yaml
            # CUPP 认领的池名，需与连接配置里 pools = <pool_name> 一致
            pool_name = cupp
            # 孤儿租约超时（秒）：超过且对应 IKE_SA 已消失则回收
            lease_timeout = 3600
            # sweep 扫描周期（秒）
            sweep_interval = 60
        }
    }
}
```

### 4.2 `policy.yaml` —— 用户策略

编辑 `/etc/strongswan/cupp/policy.yaml`：

```yaml
users:
  vip-admin:
    ip: 10.10.10.50
  vip-finance:
    ip: 10.10.10.51

pool:
  start: 10.10.10.100
  end:   10.10.10.200
```

- `users.<name>.ip`：该用户固定虚拟 IP。
- `pool.start` / `pool.end`：动态地址池区间（含端点），未命中固定表的用户从此分配。

权限（包含 EAP 用户名，建议只 root 可读）：

```bash
sudo chmod 600 /etc/strongswan/cupp/policy.yaml
sudo chown root:root /etc/strongswan/cupp/policy.yaml
```

### 4.3 `swanctl.conf`（推荐）—— 连接引用 CUPP 池

关键：连接的 `pools` 引用 CUPP 的 `pool_name`，且**不要**再配置 `pools.<name>.addrs`
（否则走 strongSwan 内存池，绕过 CUPP）。

```conf
connections {
    ike-vpn {
        version = 2
        local_addrs  = <服务器公网 IP>
        remote_addrs = %any
        pools = cupp

        local {
            auth = pubkey
            certs = serverCert.pem
            id = <服务器证书 Subject>
        }
        remote {
            auth = eap-mschapv2
            eap_id = %any
        }
        children {
            ike-vpn {
                mode = tunnel
                # 不设 rightsourceip！由 CUPP 通过 pools=cupp 分配
                local_ts  = 0.0.0.0/0
                remote_ts  = dynamic
            }
        }
    }
}

secrets {
    eap-vip-admin    { id = vip-admin    secret = "..." }
    eap-vip-finance  { id = vip-finance  secret = "..." }
    eap-normal-user  { id = normal-user  secret = "..." }
}
```

> 若用旧版 `ipsec.conf`，等价写法是 `rightsourceip=%cupp`（池名引用）且
> **不**定义 `rightsourceip=10.10.10.0/24` 这类静态段。推荐用 swanctl。

---

## 五、启动与验证

### 5.1 启动

```bash
sudo ipsec restart          # 或 sudo systemctl restart strongswan
sudo ipsec statusall | grep user-policy
# 期望输出包含 user-policy（已加载插件列表）
```

### 5.2 Windows IKEv2 客户端登录

1. Windows 设置 → 网络 → VPN → 新建连接（IKEv2，服务器地址 = 公网 IP）。
2. 用 `vip-admin` / 密码登录。
3. 连接成功后在 PowerShell 执行 `ipconfig`，VPN 适配器应显示 `10.10.10.50`。
4. 换 `normal-user` 登录，应得到 `10.10.10.100`；断开后再次登录得 `10.10.10.100`（回收复用）。

### 5.3 日志

```bash
sudo journalctl -u strongswan -f      # 或 tail -f /var/log/syslog
```

关键日志前缀 `[CUPP]`：

```
[CUPP] plugin loaded (policy=/etc/strongswan/cupp/policy.yaml pool_name=cupp)
[CUPP] policy loaded: 2 fixed users, pool 10.10.10.100-10.10.10.200
[CUPP] acquire uid=42 user=vip-admin -> 10.10.10.50 (fixed)
[CUPP] acquire uid=43 user=normal-user -> 10.10.10.100 (pool)
[CUPP] release uid=43 user=normal-user ip=10.10.10.100 (pool, recycled)
[CUPP] sweep: expired uid=7 user=stale ip=10.10.10.101 (recycled)
```

若启动失败（`policy.yaml` 缺失/格式错误），charon 日志会显示：

```
[CUPP] abort plugin load: policy '...' unavailable
plugin 'user-policy': failed to load - user_policy_plugin_create returned NULL
```

此时 charon 拒绝加载本插件（fail-fast），其他插件不受影响。

---

## 六、热重载策略

修改 `policy.yaml` 后无需重启 charon：

```bash
sudo ipsec rereadall     # 触发 plugin.reload → policy_engine_reload（双 buffer 原子切换）
```

- 已连接用户：保留当前 IP 至断开。
- 新连接：按新策略分配。
- 删除某 vip 用户的固定 IP：已连用户保留至断开，新连接走动态池。

---

## 七、IP 生命周期（核心机制）

```
登录：EAP 成功 → charon 调 acquire_address(pools=cupp, ike_sa)
        → CUPP 取 EAP 用户名 → 查固定表/动态池 → 绑定租约 → 返回虚拟 IP

断开：IKE_DOWN → listener.ike_updown(up=FALSE) → release_by_uid
        + charon 调 release_address → unbind 租约 → 动态 IP 回收池

兜底：sweep 定时扫描，对超 lease_timeout 且 IKE_SA 已消失的孤儿租约强制回收
        （防止动态池因异常断开而耗尽）

崩溃恢复：charon 重启 → 内存池清空 → 无泄漏传播
```

---

## 八、常见问题

| 现象 | 排查 |
|---|---|
| 插件未加载 | `ipsec statusall` 看插件列表；查日志是否有 `failed to load`；确认 `.so` 在插件目录且权限可读 |
| 用户拿不到 IP | 确认连接 `pools = cupp` 与 `pool_name` 一致；确认未同时设 `pools.<name>.addrs`；查 `[CUPP] acquire` 日志 |
| 固定 IP 用户拿到动态 IP | 确认 `policy.yaml` 中用户名与 EAP 身份完全一致（区分大小写）；`ipsec statusall` 看 EAP identity |
| 动态池耗尽 | 查 `[CUPP] sweep` 是否在回收；调小 `sweep_interval`；扩大 `pool` 区间 |
| 热重载无效 | 确认用 `ipsec rereadall`（不是 `reload`）；查 `[CUPP] policy reloaded` 日志 |

---

## 九、卸载

```bash
# 从 strongswan.conf 移除 user-policy 段与 include
sudo ipsec restart
sudo rm /usr/lib/ipsec/plugins/libstrongswan-user-policy.so
```

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
# ============================================================
# 第 1 步：下 strongSwan 5.8.2 源码（不用编，只用头文件）
# ============================================================
cd ~
wget -q https://download.strongswan.org/strongswan-5.8.2.tar.bz2
tar -xjf strongswan-5.8.2.tar.bz2

# 验证头文件就位
ls ~/strongswan-5.8.2/src/libstrongswan/library.h && echo "libstrongswan headers OK"
ls ~/strongswan-5.8.2/src/libcharon/daemon.h && echo "libcharon headers OK"

# ============================================================
# 第 2 步：重传项目代码（因为本地改了 CMakeLists.txt，服务器上是旧的）
#     在 Windows PowerShell 里执行以下 3 行：
# ============================================================
cd E:\AI\C++
tar --format ustar -czf cupp.tar.gz -C e:\AI\C++ strongswan-plugin-vpn-cupp
scp cupp.tar.gz root@你的服务器IP:~/

# 服务器上解压（替换上面完成后执行）
cd ~
tar -xzf cupp.tar.gz
ls ~/strongswan-plugin-vpn-cupp/plugin/CMakeLists.txt && echo "Project OK"

# ============================================================
# 第 3 步：编译
# ============================================================
cd ~/strongswan-plugin-vpn-cupp
rm -rf build
mkdir build && cd build

cmake -DSTRONGSWAN_SRC=$HOME/strongswan-5.8.2 .. 2>&1 | tee cmake.log
echo "--- cmake done, exit: $? ---"

make -j$(nproc) 2>&1 | tee build.log
echo "--- make done, exit: $? ---"

# 看产物
ls -l plugin/libstrongswan-user-policy.so 2>/dev/null && echo "BUILD SUCCESS" || echo "BUILD FAILED"
```

重传操作：
```bash
cd ~
rm -rf strongswan-plugin-vpn-cupp
tar -xzf cupp.tar.gz
cd strongswan-plugin-vpn-cupp
mkdir build && cd build
cmake -DSTRONGSWAN_SRC=$HOME/strongswan-5.8.2 .. 2>&1 | tee cmake.log
make -j$(nproc) 2>&1 | tee build.log
ls -l plugin/libstrongswan-user-policy.so 2>/dev/null && echo "BUILD SUCCESS" || echo "BUILD FAILED"
```

编译产物：`plugin/libstrongswan-user-policy.so`。

> strongSwan 的 `plugin_loader` 按文件名 `libstrongswan-<name>.so` 查找插件，
> 并按 `<name>_plugin_create`（连字符转下划线）查找构造符号。
> 本插件名为 `user-policy` → 文件 `libstrongswan-user-policy.so` →
> 符号 `user_policy_plugin_create`。

---

## 三、关联插件到 charon

编译产出 `libstrongswan-user-policy.so` 后，需要完成 3 步关联，charon 才会加载本插件。
**三步缺一不可**，少了任何一步插件都不会出现在 `loaded plugins` 列表里。

### 3.1 拷贝 .so 到 strongSwan 插件目录

```bash
sudo cp ~/strongswan-plugin-vpn-cupp/build/plugin/libstrongswan-user-policy.so \
        /usr/lib/ipsec/plugins/

# 确认就位
ls -l /usr/lib/ipsec/plugins/libstrongswan-user-policy.so
```

> strongSwan 的 `plugin_loader` 默认从 `/usr/lib/ipsec/plugins/` 扫描 `libstrongswan-*.so`。

### 3.2 创建插件加载配置（关键！）

strongSwan 5.8.x 默认 `load_modular = yes`，**不会**自动加载所有插件，而是按
`/etc/strongswan.d/charon/<plugin>.conf` 里的 `load = yes` 指令决定是否加载。
**没有 .conf 文件 → charon 根本不会尝试 dlopen 我们的 .so**。

```bash
sudo tee /etc/strongswan.d/charon/user-policy.conf > /dev/null << 'EOF'
user-policy {
    # 必须为 yes，否则插件不会被加载
    load = yes

    # policy.yaml 路径（必须放在 /etc/ipsec.d/ 下，见 3.3 说明）
    policy = /etc/ipsec.d/cupp/policy.yaml

    # CUPP 认领的池名，需与连接配置里 pools = <pool_name> 一致
    pool_name = cupp

    # 孤儿租约超时（秒）：超过且对应 IKE_SA 已消失则回收
    lease_timeout = 3600

    # sweep 扫描周期（秒）
    sweep_interval = 60
}
EOF
```

> 对照参考：官方插件的 .conf 文件（如 `/etc/strongswan.d/charon/aes.conf`）都有
> `load = yes` 指令，机制完全相同。

### 3.3 创建 policy.yaml（必须放在 /etc/ipsec.d/cupp/ 下！）

**重要**：Ubuntu 20.04 的 AppArmor profile `/usr/lib/ipsec/charon` 默认只允许 charon
读取 `/etc/ipsec.d/**` 和 `/etc/strongswan.d/**`，**不包含** `/etc/strongswan/cupp/**`。
如果把 policy.yaml 放在 `/etc/strongswan/cupp/`，charon 读取时会报 `Permission denied`，
插件构造函数返回 NULL，charon 拒绝加载插件。

**方案 A（推荐）**：policy.yaml 放在 AppArmor 已放行的目录：

```bash
sudo mkdir -p /etc/ipsec.d/cupp
sudo tee /etc/ipsec.d/cupp/policy.yaml > /dev/null << 'EOF'
users:
  vip-admin:
    ip: 10.10.10.50
  vip-finance:
    ip: 10.10.10.51
  vip-ops:
    ip: 10.10.10.52

pool:
  start: 10.10.10.100
  end:   10.10.10.200
EOF

sudo chmod 600 /etc/ipsec.d/cupp/policy.yaml
sudo chown root:root /etc/ipsec.d/cupp/policy.yaml
```

**方案 B**：若坚持用 `/etc/strongswan/cupp/`，需修改 AppArmor profile：

```bash
# 在 /etc/apparmor.d/local/ 下新建片段（主 profile 末尾会 include 它）
sudo tee /etc/apparmor.d/local/usr.lib.ipsec.charon > /dev/null << 'EOF'
# CUPP plugin: allow reading policy.yaml
/etc/strongswan/cupp/   r,
/etc/strongswan/cupp/** r,
EOF

# 重新加载 profile
sudo apparmor_parser -r /etc/apparmor.d/usr.lib.ipsec.charon
```

> 强烈推荐方案 A，不依赖 AppArmor profile 修改，迁移到其他服务器时不会踩坑。

### 3.4 验证关联是否成功

> ⚠️ 此步骤会重启 charon，**断开所有已连接的 VPN 用户**。仅在首次部署时执行。

```bash
# 重启 charon（首次部署用，日常运维不要用）
sudo ipsec stop; sleep 1; sudo ipsec start; sleep 3

# 验证 1：插件出现在 loaded plugins 列表
cat /var/log/syslog | grep "loaded plugins" | tail -1
# 期望输出包含 "user-policy"

# 验证 2：ipsec listplugins 识别到插件
ipsec listplugins 2>&1 | grep -i policy
# 期望输出：
#   user-policy:
#       CUSTOM:user-policy

# 验证 3：CUPP 日志显示 policy 加载成功
cat /var/log/syslog | grep "CUPP" | tail -5
# 期望输出：
#   [CUPP] policy loaded: 3 fixed users, pool 10.10.10.100-10.10.10.200
#   [CUPP] dynamic pool 10.10.10.100-10.10.10.200 capacity=101
#   [CUPP] plugin loaded (policy=/etc/ipsec.d/cupp/policy.yaml pool_name=cupp)
#   [CUPP] plugin registered (pool=cupp lease_timeout=3600s sweep=60s)
```

若日志显示 `plugin 'user-policy': failed to load - user_policy_plugin_create returned NULL`，
说明 policy.yaml 读不了（多半是 AppArmor 拦截）或格式错误，回看 3.3。

---

## 四、配置 VPN 连接

CUPP 只管 IP 分配，VPN 隧道配置照常写在 swanctl.conf 或 ipsec.conf。
**关键**：连接的 `pools` 必须引用 CUPP 的 `pool_name`（默认 `cupp`），且**不要**再
配置 `pools.<name>.addrs`（否则走 strongSwan 内存池，绕过 CUPP）。

### 4.1 swanctl.conf（推荐）

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

### 4.2 ipsec.conf（旧版）

等价写法：`rightsourceip=%cupp`（池名引用），且**不**定义 `rightsourceip=10.10.10.0/24`
这类静态段。推荐用 swanctl。

---

## 五、启动与验证

### 5.1 启动

> ⚠️ **注意**：以下启动命令会**断开所有已连接的 VPN 用户**（charon 进程重启）。
> 仅在首次部署或维护窗口执行。日常新增/修改用户请用热重载（见第六章），不要重启。

```bash
sudo ipsec restart          # 或 sudo systemctl restart strongswan
```

> **重要（踩坑修正）**：Ubuntu 20.04 上**没有** `strongswan.service`，只有
> `strongswan-starter.service`（`ExecStart=/usr/sbin/ipsec start --nofork`）。
> 无论用 `sudo systemctl restart strongswan-starter` 还是 `sudo ipsec restart`，
> charon 启动后**都不会**自动加载 swanctl.conf 中的连接配置。
>
> **现象**：Windows 客户端连接被 `NO_PROPOSAL_CHOSEN` 拒绝，服务器日志：
> `no IKE config found for <server-ip>...<client-ip>, sending NO_PROPOSAL_CHOSEN`
> （阿里云自动重启后必现，因为 systemd 只拉起了 charon 进程，swanctl 连接定义为空）。
>
> **手动修复（临时）**：
> ```bash
> sudo swanctl --load-all       # 5.8.2 无 --load-certs，用 --load-all 一次全加载
> sudo swanctl --list-conns     # 验证连接定义已出现
> ```
>
> **永久修复（推荐）**：配置 systemd drop-in 开机自动 `swanctl --load-all`，见 §5.1.2。

#### 5.1.1 排查 swanctl 连接未加载

若 Windows 连接被 `NO_PROPOSAL_CHOSEN` 拒绝，按以下步骤排查：

```bash
# 1. 看是否找到 IKE 配置（关键日志）
cat /var/log/syslog | grep "no IKE config found\|selected peer config" | tail -5

# 若输出 "no IKE config found" → swanctl.conf 未加载
# 若输出 "selected peer config 'windows-client'" → 配置已加载，问题在别处

# 2. 手动加载连接（5.8.2 无 --load-certs，用 --load-all 一次全加载）
sudo swanctl --load-all

# 3. 验证连接列表
swanctl --list-conns
# 应输出 windows-client 连接定义

# 4. 确认 swanctl 插件启用
cat /etc/strongswan.d/charon/swanctl.conf
# 应包含：load = yes
```

#### 5.1.2 配置开机自动加载 swanctl 连接（永久修复）

阿里云 ECS 自动重启后，`strongswan-starter.service` 只启动 charon 进程，**不会**自动执行
`swanctl --load-all`，导致 swanctl.conf 中的连接定义为空，Windows 客户端被
`NO_PROPOSAL_CHOSEN` 拒绝。通过 systemd drop-in override 解决：

**第 1 步：补齐 swanctl 插件配置**

```bash
sudo mkdir -p /etc/strongswan.d/charon
sudo tee /etc/strongswan.d/charon/swanctl.conf > /dev/null << 'EOF'
swanctl {
    load = yes
}
EOF
```

**第 2 步：创建 systemd drop-in override**

```bash
sudo mkdir -p /etc/systemd/system/strongswan-starter.service.d
sudo tee /etc/systemd/system/strongswan-starter.service.d/10-swanctl-load-all.conf > /dev/null << 'EOF'
[Unit]
Wants=network-online.target

[Service]
# ipsec start --nofork 返回后，sleep 2 再 load-all，
# 给 charon/vici 留启动时间；|| true 保证失败不影响主服务状态
ExecStartPost=/bin/sh -c 'sleep 2; /usr/sbin/swanctl --load-all || true'
TimeoutStartSec=30
EOF
```

> 用 drop-in（`/etc/systemd/system/...service.d/`）而非直接改
> `/lib/systemd/system/strongswan-starter.service`，避免 apt upgrade 强Swan时被覆盖。

**第 3 步：重载并验证**

```bash
sudo systemctl daemon-reload
sudo systemctl restart strongswan-starter
sleep 5
sudo swanctl --list-conns     # 应输出连接定义（ike-vpn / windows-client 等）
```

验证通过后，每次阿里云重启或 `systemctl restart strongswan-starter` 都会自动加载
swanctl 连接配置，无需再手动执行 `swanctl --load-all`。

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
[CUPP] plugin loaded (policy=/etc/ipsec.d/cupp/policy.yaml pool_name=cupp)
[CUPP] policy loaded: 3 fixed users, pool 10.10.10.100-10.10.10.200
[CUPP] acquire uid=42 user=vip-admin -> 10.10.10.50 (fixed)
[CUPP] acquire uid=43 user=normal-user -> 10.10.10.100 (pool)
[CUPP] release uid=43 user=normal-user ip=10.10.10.100 (pool, recycled)
[CUPP] sweep: expired uid=7 user=stale ip=10.10.10.101 (recycled)
```

若启动失败（`policy.yaml` 缺失/格式错误/AppArmor 拦截），charon 日志会显示：

```
[CUPP] open policy /etc/.../policy.yaml failed: Permission denied
[CUPP] abort plugin load: policy '...' unavailable
plugin 'user-policy': failed to load - user_policy_plugin_create returned NULL
```

此时 charon 拒绝加载本插件（fail-fast），其他插件不受影响。回看 3.3。

---

## 六、热重载策略

修改 `policy.yaml` 后无需重启 charon：

```bash
sudo ipsec rereadall     # 触发 plugin.reload → policy_engine_reload（双 buffer 原子切换）
```

- 已连接用户：保留当前 IP 至断开。
- 新连接：按新策略分配。
- 删除某 vip 用户的固定 IP：已连用户保留至断开，新连接走动态池。

### 6.1 热重载验证与排查

**验证热重载是否生效**（关键）：

```bash
cat /var/log/syslog | grep "policy loaded\|policy reloaded" | tail -2
```

期望输出应反映最新配置（如新增用户后 `fixed users` 数量应增加）。

**常见失败原因与解决**：

#### 问题 A：`getcwd() failed: No such file or directory`

```
sh: 0: getcwd() failed: No such file or directory
```

**原因**：当前 shell 的工作目录已被删除或路径失效（常见于编译目录被 `rm -rf build` 后仍停留在该路径）。
`ipsec rereadall` 执行失败，charon 未收到 SIGHUP，policy 未重载。

**解决**：切换到有效目录再执行：

```bash
cd ~                        # 切到 home 目录
sudo ipsec rereadall        # 重新热重载
cat /var/log/syslog | grep "policy loaded\|policy reloaded" | tail -2   # 验证
```

#### 问题 B：`policy loaded` 数量未变化

热重载后 `fixed users` 数量仍是旧值，说明配置未生效。

**解决（按顺序尝试）**：

```bash
# 第 1 步：确认在 home 目录执行（最常见失败原因）
cd ~
sudo ipsec rereadall
cat /var/log/syslog | grep "policy loaded\|policy reloaded" | tail -2
```

若第 1 步仍无效，检查 policy.yaml 文件格式：

```bash
# 看隐藏字符（制表符会显示为 ^I，行尾显示为 $）
cat -A /etc/ipsec.d/cupp/policy.yaml | head -10
```

若文件格式无误但仍未加载，**最后手段**才是重启 charon：

> 🚨 **高风险操作警告** 🚨
>
> **`sudo ipsec restart` 会立即断开所有已连接的 VPN 用户！**
>
> 影响：
> - charon 进程重启 → 所有 IKE_SA 立即丢失
> - CUPP 内存租约表清空（租约不持久化到磁盘）
> - 所有已连接的 Windows 客户端会立即断开，需要重连
> - 正在传输的数据会中断
>
> **仅以下情况才考虑使用**：
> - 维护窗口期（已通知所有用户）
> - 确认无用户连接（深夜/测试环境）
> - 热重载完全失效，且无法通过其他方式解决
>
> **日常运维请始终使用 `ipsec rereadall`**（不影响已连接用户）。

```bash
cd ~                        # 同样需要先切到有效目录
sudo ipsec restart
sleep 3

# 重新加载 swanctl 配置（ipsec restart 不会自动加载 swanctl.conf）
sudo swanctl --load-conns
sudo swanctl --load-creds

# 验证 policy 加载
cat /var/log/syslog | grep "policy loaded" | tail -1
```

> 注意：Ubuntu 20.04 没有 `strongswan.service`，用 `sudo systemctl restart strongswan` 会报
> `Unit strongswan.service not found`。改用 `sudo ipsec restart` 即可。

#### 问题 C：用户仍拿到旧 IP（`rekey, reused pool`）

热重载已成功（`policy loaded` 显示正确数量），但用户连接后仍拿到动态池 IP，日志显示：

```
[CUPP] acquire uid=N user=xxx -> 10.10.10.100 (rekey, reused pool)
```

**原因**：该用户在旧策略下已有活跃租约（绑定了动态池 IP）。**rekey 时 CUPP 优先复用旧租约的 IP，不会重新查固定表**（这是正确设计，避免 rekey 时 IP 跳变导致连接中断）。

**解决**：强制断开用户连接，释放旧租约，下次连接才会按新策略分配：

```bash
# 1. 查找用户的 unique_id（日志中的 uid=N）
cat /var/log/syslog | grep "CUPP" | grep "acquire" | tail -5

# 2. 强制断开该 SA（用日志中的 uid）
sudo ipsec terminate <unique_id>

# 3. 确认租约已释放（应看到 release 日志）
cat /var/log/syslog | grep "CUPP" | tail -3

# 4. Windows 客户端重新连接 VPN，应按新策略分配固定 IP
```

### 6.2 热重载完整流程（推荐）

修改 `policy.yaml` 后的完整操作流程：

```bash
# 1. 切换到有效目录（关键！避免 getcwd 错误导致 rereadall 失败）
cd ~

# 2. 热重载 policy.yaml（不影响已连接用户）
sudo ipsec rereadall

# 3. 验证加载成功（fixed users 数量应正确）
cat /var/log/syslog | grep "policy loaded\|policy reloaded" | tail -2

# 4. 若某用户已连接且拿着旧 IP，只断开这一个用户（不影响其他人）
#    从日志找 uid：cat /var/log/syslog | grep "CUPP" | grep <username> | tail -1
sudo ipsec terminate <unique_id>

# 5. Windows 客户端重新连接，验证分配到正确的固定 IP
cat /var/log/syslog | grep "CUPP" | tail -3
```

> ⚠️ **重要**：每次执行 `ipsec rereadall` 前**必须先 `cd ~`**。
> 常见错误：在 `~/strongswan-plugin-vpn-cupp/build` 目录执行 `rm -rf build` 后，
> 当前 shell 的工作目录失效，`ipsec rereadall` 会报 `getcwd() failed` 且不生效。
> 先 `cd ~` 即可避免。

---

## 七、账号与 IP 管理（运维操作）

### 7.1 添加新账号（固定 IP 用户）

**场景**：给新员工 `vip-sales` 分配固定 IP `10.10.10.53`。

**步骤 1：在 policy.yaml 添加用户固定 IP**

```bash
sudo vi /etc/ipsec.d/cupp/policy.yaml
```

在 `users` 下新增：

```yaml
users:
  vip-admin:
    ip: 10.10.10.50
  vip-finance:
    ip: 10.10.10.51
  vip-ops:
    ip: 10.10.10.52
  vip-sales:              # ← 新增
    ip: 10.10.10.53
```

**步骤 2：在 swanctl.conf 添加 EAP 密钥**

```bash
sudo vi /etc/swanctl/swanctl.conf
```

在 `secrets` 下新增：

```conf
secrets {
    eap-vip-admin    { id = vip-admin    secret = "Admin@123456" }
    eap-vip-finance  { id = vip-finance  secret = "Finance@123456" }
    eap-vip-ops      { id = vip-ops      secret = "Ops@123456" }
    eap-vip-sales    { id = vip-sales    secret = "Sales@123456" }   # ← 新增
    private-key-1    { file = /etc/ipsec.d/private/server-key.pem  secret = "" }
}
```

**步骤 3：加载新配置**

```bash
# policy.yaml 热重载（无需重启 charon，不影响已连接用户）
sudo ipsec rereadall

# swanctl.conf 重新加载密钥
sudo swanctl --load-all
```

**步骤 4：验证**

```bash
# 确认策略已加载（应显示 4 个固定用户）
cat /var/log/syslog | grep "CUPP" | tail -3
# 期望：[CUPP] policy loaded: 4 fixed users, pool 10.10.10.100-10.10.10.200

# Windows 客户端用 vip-sales / Sales@123456 登录，应得到 10.10.10.53
```

### 7.2 添加动态池用户（无固定 IP）

**场景**：给临时用户 `guest-01` 分配动态池 IP（10.10.10.100-200 范围内自动分配）。

**只需在 swanctl.conf 添加 EAP 密钥**，无需修改 policy.yaml：

```bash
sudo vi /etc/swanctl/swanctl.conf
```

```conf
secrets {
    # ... 已有用户 ...
    eap-guest-01    { id = guest-01    secret = "Guest@123456" }   # ← 新增
}
```

```bash
sudo swanctl --load-creds      # strongSwan 5.8.2 用 --load-creds
```

guest-01 登录后会从动态池自动分配 IP（10.10.10.100 起，断开后回收复用）。

### 7.3 修改已有用户的固定 IP

**场景**：把 `vip-admin` 的 IP 从 `10.10.10.50` 改为 `10.10.10.60`。

```bash
sudo vi /etc/ipsec.d/cupp/policy.yaml
# 修改 vip-admin 的 ip 字段

sudo ipsec rereadall
```

**注意**：
- 已连接的 vip-admin：保留 10.10.10.50 至断开。
- 下次连接：分配新的 10.10.10.60。

### 7.4 删除用户

**场景**：禁用 `vip-sales` 账号。

```bash
# 1. 从 swanctl.conf 删除对应 eap-vip-sales 段
sudo vi /etc/swanctl/swanctl.conf
sudo swanctl --load-all

# 2. 从 policy.yaml 删除 vip-sales 段（可选，保留也无害）
sudo vi /etc/ipsec.d/cupp/policy.yaml
sudo ipsec rereadall

# 3. 若用户当前已连接，强制断开（通过 unique_id）
sudo ipsec statusall | grep vip-sales      # 找到 unique_id
sudo ipsec terminate <unique_id>
```

### 7.5 IP 分配规则总结

| 用户类型 | policy.yaml | swanctl.conf | 分配结果 |
|---|---|---|---|
| 固定 IP 用户 | `users.<name>.ip = X` | `eap-<name> { id=<name> secret=... }` | 固定 IP X |
| 动态池用户 | 不配置 | `eap-<name> { id=<name> secret=... }` | 动态池 IP（100-200） |
| 未配置密钥 | - | - | 连接被拒（EAP 认证失败） |

**关键原则**：
- **用户名一致性**：policy.yaml 的 `users.<name>` 必须与 swanctl.conf 的 `eap-<name>.id` **完全一致**（区分大小写），否则固定 IP 不生效，会走动态池。
- **IP 不冲突**：固定 IP 不能落在动态池范围内（10.10.10.100-200），否则可能冲突。建议固定 IP 用 .50-.99 段，动态池用 .100-.200 段。
- **热重载生效**：修改 policy.yaml 后必须 `ipsec rereadall`；修改 swanctl.conf 后必须 `swanctl --load-creds`（strongSwan 5.8.2 用 `--load-creds`，不是 `--load-secrets`）。

---

## 八、IP 生命周期（核心机制）

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

## 九、常见问题

| 现象 | 排查 |
|---|---|
| 插件不在 `loaded plugins` 列表 | ① 确认 .so 在 `/usr/lib/ipsec/plugins/` ② 确认 `/etc/strongswan.d/charon/user-policy.conf` 存在且 `load = yes`（见 3.2） |
| `failed to load - returned NULL` | policy.yaml 读不了或格式错误。查 syslog 是否有 `Permission denied`（AppArmor 拦截），policy.yaml 必须放在 `/etc/ipsec.d/cupp/` 下（见 3.3） |
| AppArmor `DENIED` 操作 | policy.yaml 放在了 `/etc/strongswan/cupp/`，charon 的 AppArmor profile 不允许读。改放 `/etc/ipsec.d/cupp/`（方案 A），或修改 AppArmor profile（方案 B） |
| Windows 连接被 `NO_PROPOSAL_CHOSEN` 拒绝 | swanctl.conf 未加载。用 `sudo systemctl restart strongswan`（自动加载），或手动 `swanctl --load-conns/--load-creds`（见 5.1.1） |
| 用户拿不到 IP | 确认连接 `pools = cupp` 与 `pool_name` 一致；确认未同时设 `pools.<name>.addrs`；查 `[CUPP] acquire` 日志 |
| 固定 IP 用户拿到动态池 IP | 确认 `policy.yaml` 中用户名与 EAP 身份完全一致（区分大小写）；查 `[CUPP] identity_get_username` 日志看实际提取的用户名（见 9.1） |
| `[CUPP] no string identity, falling back to dynamic pool` | EAP identity 未提取成功，回退动态池。查 `identity_get_username` 日志看 `id_type` 值；只有 `ID_FQDN(2)/ID_RFC822_ADDR(3)/ID_KEY_ID(11)` 才作为用户名 |
| 动态池耗尽 | 查 `[CUPP] sweep` 是否在回收；调小 `sweep_interval`；扩大 `pool` 区间 |
| 热重载无效 | ① `getcwd() failed` → 切到有效目录 `cd ~` 再执行（见 6.1 问题 A） ② `policy loaded` 数量未变 → 先检查 policy.yaml 格式，最后手段才是 `sudo ipsec restart`（⚠️会断开所有用户，见 6.1 问题 B） ③ 确认用 `ipsec rereadall`（不是 `reload`）；查 `[CUPP] policy reloaded` 日志 |
| 用户仍拿旧 IP（`rekey, reused pool`） | 热重载已生效但用户有旧租约。强制断开 `sudo ipsec terminate <uid>` 后重连（见 6.1 问题 C） |
| 用户名不匹配导致拿动态池 IP | policy.yaml 中用户名必须与 Windows 登录的用户名**完全一致**（区分大小写）。如 policy.yaml 配 `hrgk` 但登录用 `vip-hrgk`，会匹配失败走动态池 |
| `systemctl restart strongswan` 报 `Unit not found` | Ubuntu 20.04 无 `strongswan.service`，改用 `sudo ipsec restart`（见 5.1） |
| `swanctl --load-secrets` 报 `unrecognized option` | strongSwan 5.8.2 用 `swanctl --load-creds`（不是 `--load-secrets`）加载密钥 |

### 9.1 EAP identity 获取原理（开发者参考）

CUPP 插件通过 `identity_get_username()` 从 IKE_SA 提取 EAP 用户名，按以下优先级尝试：

1. `auth_cfg->get(AUTH_RULE_EAP_IDENTITY)` — EAP 认证完成后设置（最可靠）
2. `ike_sa->get_other_eap_id()` — IKE_SA 字段，EAP identity 收到即设置（charon 内部打印 peer 名也用此方法）
3. `auth_cfg->get(AUTH_RULE_IDENTITY)` — IKE IDi
4. `ike_sa->get_other_id()` — IKE_SA 字段，IDi（注意：Windows IKEv2 客户端的 IDi 可能是 IP 地址）

**ID 类型检查**：只有字符串类型（`ID_FQDN=2`、`ID_RFC822_ADDR=3`、`ID_KEY_ID=11`）的 encoding 才转为用户名。
IP 地址类型（`ID_IPV4_ADDR=1` 等）的 encoding 是二进制，直接跳过，避免把 4 字节 IP 当字符串误用。

**容错机制**：若所有方法都取不到字符串用户名，`acquire_address` 会用占位名 `anon-<uid>` 走动态池分配，
保证连接仍能建立（不会因取不到用户名而连接失败）。

典型日志（成功）：
```
[CUPP] identity_get_username: extracting username...
[CUPP] identity_get_username: auth_cfg=0x7f601400d760
[CUPP] identity_get_username: EAP_IDENTITY=(nil)
[CUPP] identity_get_username: other_eap_id=0x7f601400f450
[CUPP] identity_get_username: id_type=2 encoding_len=9
[CUPP] identity_get_username: returning [vip-admin] (type=2)
[CUPP] acquire uid=42 user=vip-admin -> 10.10.10.50 (fixed)
```

典型日志（回退动态池）：
```
[CUPP] identity_get_username: id_type=1 is not a string type, skipping
[CUPP] acquire: no string identity, falling back to dynamic pool
[CUPP] acquire uid=42 user=anon-42 -> 10.10.10.100 (pool)
```

---

## 十、卸载

```bash
# 1. 停止 strongSwan
sudo ipsec stop

# 2. 删除插件 .so
sudo rm /usr/lib/ipsec/plugins/libstrongswan-user-policy.so

# 3. 删除加载配置
sudo rm /etc/strongswan.d/charon/user-policy.conf

# 4. （可选）删除 policy.yaml
sudo rm -rf /etc/ipsec.d/cupp

# 5. 重启 strongSwan
sudo ipsec start
```

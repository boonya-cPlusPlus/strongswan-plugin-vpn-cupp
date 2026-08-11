# ============================================================
# 1) 补齐 swanctl 插件 load=yes 配置（/etc/strongswan.d/charon/swanctl.conf）
# ============================================================
sudo mkdir -p /etc/strongswan.d/charon
sudo tee /etc/strongswan.d/charon/swanctl.conf > /dev/null << 'EOF'
swanctl {
    load = yes
}
EOF
echo "=== [1/4] swanctl.conf 写入完成 ===" && cat /etc/strongswan.d/charon/swanctl.conf

# ============================================================
# 2) 创建 systemd drop-in override：服务启动后 2s 自动 swanctl --load-all
#    用 drop-in 而不是改 /lib/systemd/system/strongswan-starter.service，
#    避免 apt upgrade 强Swan时被覆盖
# ============================================================
sudo mkdir -p /etc/systemd/system/strongswan-starter.service.d
sudo tee /etc/systemd/system/strongswan-starter.service.d/10-swanctl-load-all.conf > /dev/null << 'EOF'
[Unit]
# 已经有 After=network-online.target 了，这里 Wants 确保它被拉起来
Wants=network-online.target

[Service]
# ipsec start --nofork 返回后，sleep 2 再 load-all，
# 给 charon/vici 留启动时间；|| true 保证失败不影响主服务状态
ExecStartPost=/bin/sh -c 'sleep 2; /usr/sbin/swanctl --load-all || true'
# 给 drop-in 的 ExecStartPost 单独放宽超时
TimeoutStartSec=30
EOF
echo ""
echo "=== [2/4] drop-in 写入完成 ===" && sudo systemctl cat strongswan-starter.service | tail -25

# ============================================================
# 3) 重载 systemd + 重启服务，模拟一次阿里云重启后的场景
# ============================================================
sudo systemctl daemon-reload
echo ""
echo "=== [3/4] daemon-reload 完成，重启 strongswan-starter（会断开当前 VPN） ==="
sudo systemctl restart strongswan-starter

# ============================================================
# 4) 验证：重启后连接配置是否自动加载了？
# ============================================================
sleep 5
echo ""
echo "=== [4/4] 验证：swanctl --list-conns 是否有 ike-vpn / windows-client ==="
sudo swanctl --list-conns
echo ""
echo "=== 如果上面非空，说明开机自启修复成功，下次阿里云重启不用再手动 load-all 了 ==="
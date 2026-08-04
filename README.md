\# Charon User Policy Plugin (CUPP)



\## 项目目标



开发一个 StrongSwan charon 插件，实现基于 EAP 用户名的 VPN 虚拟 IP 管理。



目标：



\- vip 用户分配固定 VPN IP

\- 普通用户从插件维护的动态地址池分配 IP

\- 不使用 FreeRADIUS、LDAP、数据库





\## 技术栈



核心插件：



\- C11

\- StrongSwan Plugin API

\- CMake

\- Linux

\- YAML配置





管理端（后续）：



\- Qt6

\- C++17





\## MVP 功能



必须实现：



1\. StrongSwan 插件加载



2\. 获取 EAP 用户名



3\. 加载 YAML 用户策略



4\. 固定 IP 分配



5\. 动态 IP Pool 分配



6\. 用户断开后释放 IP





\## 配置示例



policy.yaml



```yaml

users:

&#x20; vip-admin:

&#x20;   ip: 10.10.10.50



&#x20; vip-finance:

&#x20;   ip: 10.10.10.51



pool:

&#x20; start: 10.10.10.100

&#x20; end: 10.10.10.200


# server/ —— 业务服务端

> **归属 L1（net/）· L2（biz/ dao/）**。只写本模块规则；全组共识见根 [CLAUDE.md](../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 职责划分

| 子目录 | 归属 | 内容 |
| --- | --- | --- |
| `net/` | **L1** | socket 监听、epoll IO 线程、pthread 线程池、会话表、命令字分发、设备注册表 |
| `biz/` | **L2** | 九个业务服务，清单见 [biz/README.md](biz/README.md) |
| `dao/` | **L2** | SQLite 访问层 |

`main.cpp` 由 L1 维护；L2 只需在 `registerAllServices()` 中追加注册调用。

## 本模块特有规则

1. **网络层用 POSIX socket + epoll，叠加 Qt QSocketNotifier/QTimer 做事件调度。** [说明书] 1.6 Socket 编程。
2. **线程池用 pthread 实现多线程结构。** [说明书] 1.6 多线程考核点。
3. **每个工作线程一个数据库连接**，一律通过 `ecp::threadDb()` 获取，禁止自己 `addDatabase`。
4. **`SIGPIPE` 已在 main 中忽略**，不要移除——向已关闭连接写数据会杀死进程。
5. handler 抛异常会被 dispatcher 捕获并返回 `ERR_INTERNAL`，但不要依赖这个兜底，业务里该判的要判。

## 运行现状

核心闭环已全部打通：监听、epoll IO 线程、pthread 线程池、粘包/半包处理、会话表与 token 鉴权、
命令字分发、每线程 DB 连接、设备注册表与用户推送注册表、60s 定时清理过期会话（`SessionTable::sweepExpired()`）。

命令字覆盖：核心 35 个（`common/protocol.h`）+ 扩展 08 段 8 个（`common/protocol_ext.h`），
handler 分布见 [biz/README.md](biz/README.md) 与 [net/README.md](net/README.md)。
命令字 `0` 是连通性探针（不在协议表内，仅供联调）。

启动、构建与测试命令一律见 [docs/RUNBOOK.md](../docs/RUNBOOK.md)，本文不重复。

未注册的命令字返回 `ERR_CMD_UNKNOWN(1005)`。扩展模块的 handler 另受 `t_sys_config` 的 `feat_*` 开关控制：
`main.cpp` 的 `FeatureFlags` 启动时一次查询读回全部 `feat_*` 键并缓存，读不到按关闭处理并记 `LOG_E`。
新增扩展模块时在 `registerAllServices()` 里加一行条件注册即可。

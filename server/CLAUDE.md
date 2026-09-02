# server/ —— 业务服务端

> **归属 L1（net/）· L2（biz/ dao/）**。只写本模块规则；全组共识见根 [CLAUDE.md](../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 职责划分

| 子目录 | 归属 | 内容 |
| --- | --- | --- |
| `net/` | **L1** | socket 监听、pthread 线程池、会话表、命令字分发 |
| `biz/` | **L2** | 九个业务服务，清单见 [biz/README.md](biz/README.md) |
| `dao/` | **L2** | SQLite 访问层 |

`main.cpp` 由 L1 维护；L2 只需在 `registerAllServices()` 中追加注册调用。

## 本模块特有规则

1. **网络层用 POSIX 原生 socket，不是 QTcpServer。** [说明书] 1.6 点名 Socket 编程，这是考核点。
2. **线程池必须是 pthread，不得改用 QThread。** 同为 [说明书] 1.6 考核点。
3. **每个工作线程一个数据库连接**，一律通过 `ecp::threadDb()` 获取，禁止自己 `addDatabase`。
4. **`SIGPIPE` 已在 main 中忽略**，不要移除——向已关闭连接写数据会杀死进程。
5. handler 抛异常会被 dispatcher 捕获并返回 `ERR_INTERNAL`，但不要依赖这个兜底，业务里该判的要判。

## 骨架现状

框架已可运行：监听、pthread 线程池、粘包/半包处理、会话表与 token 鉴权、命令字分发、每线程 DB 连接。
命令字 `0` 是连通性探针（不在协议表内，仅供联调）。

**已实现的业务 handler（L2 样板，可改写）**：1001 手机号免密登录（含自动注册、冻结拦截）、1002 用户信息、2001 管理员登录。
它们示范了本模块必须遵守的写法——`ecp::threadDb()` 取连接、`prepare`+`addBindValue` 参数绑定、金额整数分、返回 `error_code.h` 错误码、`SessionTable` 发 token、鉴权接口只信 `req.session.id` 而非客户端传的 id。**其余七个服务照此实现。**

未实现的命令字仍返回 `ERR_CMD_UNKNOWN(1005)`。

## TODO

- [ ] **L2**：按 `biz/README.md` 实现其余七个服务并注册（用户/管理员登录已有样板）
- [ ] **L1**：`CMD_DEV_*`（9001–9004）设备侧处理与重启指令下发
- [ ] **L1**：会话过期清理定时任务（`SessionTable::sweepExpired()` 已就绪，未接定时器）

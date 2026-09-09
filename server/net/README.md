# server/net/ —— 服务端网络层

> 归属 **L1**。全组共识见根 [CLAUDE.md](../../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 技术方案 `[本组自定]`

Qt 主线程运行事件循环，负责 IO 事件通知与定时任务；创建独立 pthread IO 线程，基于 POSIX socket 接口统一处理全部 socket 读写；通过 pthread 线程池做多线程调度，执行业务短任务，实现 IO 与业务逻辑完全解耦。

底层网络与线程原语使用 POSIX 原生接口，利用 Qt 提供主线事件循环、QSocketNotifier、QTimer 组件，没有使用 Qt 封装的 QTcpSocket 网络类。

## 线程模型 `[本组自定]`

> 本节由 `docs/protocol.md` 第 7 节迁移而来——线程模型是服务端实现，不是通信契约，故从协议文档移入本目录。
> 对应 `[说明书]` 1.6「程序的主框架应该是一个多线程结构」「多线程 pthread 编程」：

- **主线程（Qt 事件循环）**：`QSocketNotifier` 监听 listen_fd（非阻塞，触发时循环 `accept` 到 EAGAIN）；`QTimer` 每 60s 清理过期会话 + 判定离线设备；跑 `app.exec()`主循环
- **epoll IO 线程（pthread）**：统一管理全部业务 socket 的 `recv`/`send`；`FrameParser` 切帧（处理粘包/半包），切出完整 payload 投入工作线程池，不执行业务
- **pthread 线程池**：默认 8 个工作线程（`config/app.ini` 的 `pool_size` 可配），执行短业务 handler 后归还；`pool_size` 是「最大并发业务处理数」，并发连接数不再受它限制
- **每连接发送队列**：业务线程把 `encodeFrame()` 后的完整帧入队，IO 线程按 `EPOLLOUT` 发送，支持服务端主动推送
- **会话表**：跨线程共享，读写加 `pthread_rwlock_t`；每线程独立 QSqlDatabase 连接（硬性规则第 2 条）

## 文件清单

| 文件 | 职责 | 使用说明 |
| --- | --- | --- |
| `thread_pool.h/.cpp` | pthread 线程池 | 内部；Task 内禁止调 `stop()` |
| `tcp_server.h/.cpp` | 监听 + 组装各组件 | 仅 main.cpp 用；`listenOn()` 后跑 `app.exec()` |
| `epoll_loop.h/.cpp` | epoll IO 线程 | 内部，业务不接触 |
| `conn_ctx.h/.cpp` | 单连接状态（发送队列/解析器） | 业务线程只调 `enqueueSend()`，不碰 fd |
| `session.h/.cpp` | token 会话表 | 接口已自带锁，勿自行加锁 |
| `dispatcher.h/.cpp` | 命令字分发 | L2 用 `registerHandler`；handler 只读 `req.session` |
| `device_registry.h/.cpp` | 电桩↔连接映射 | L2 用 `pushToDevice` / `isOnline` / `lastReport` / `startCharging` / `stopCharging` |
| `user_registry.h/.cpp` | 用户连接映射 | L2 用 `pushToUser` 推 1208 |
| `device_service.cpp` | 设备命令字 9001/9002/9004 处理 | 内部 |
| `io_wake.h/.cpp` | eventfd 唤醒 | 内部，业务不直接调 |

## 服务清单

> 对应 `[说明书]` 1.4「远程重启（模拟向电桩发送重启指令）」。

| 命令字 | 常量 | 方向 | 说明 |
| --- | --- | --- | --- |
| 9001 | `CMD_DEV_REGISTER` | 设备→服务端 | 注册上线，登记映射、置 online=1 |
| 9002 | `CMD_DEV_REPORT` | 设备→服务端 | 状态/电量上报 |
| 9003 | `CMD_DEV_REBOOT` | 服务端→设备 | 重启指令下发（由 2112 触发） |
| 9004 | `CMD_DEV_HEARTBEAT` | 设备→服务端 | 心跳，刷 last_heartbeat |
| 9005 | `CMD_DEV_START` | 服务端→设备 | 开始充电（累计清零） |
| 9006 | `CMD_DEV_STOP` | 服务端→设备 | 结束充电（停止累计） |

## 为什么是这个模型

从「一连接一线程」的阻塞模型改造而来。原模型每条连接占一个线程直到断开，`pool_size` 实际是最大并发**连接数**；
现在 epoll IO 线程统一收发，`pool_size` 只约束最大并发**业务处理数**，连接数不再受它限制。
accept 由 Qt 主线程的 `QSocketNotifier` 承担，过期会话清理由 `QTimer` 承担，阻塞式 `run()` 循环已删除。

设备注册表使服务端既能应答客户端请求，也能向指定电桩主动下发指令（2112 → 9003、起停充电 9005/9006）；
用户注册表同理支撑 1208 充电进度推送。

运行与联调命令见 [../../docs/RUNBOOK.md](../../docs/RUNBOOK.md)。

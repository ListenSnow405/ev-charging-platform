# server/net/ —— 服务端网络层

> 归属 **L1**。全组共识见根 [CLAUDE.md](../../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 线程模型 `[本组自定]`

> 本节由 `docs/protocol.md` 第 7 节迁移而来——线程模型是服务端实现，不是通信契约，
> 故从协议文档移入本目录。

对应 `[说明书]` 1.6「程序的主框架应该是一个多线程结构」「多线程 pthread 编程」：

- 主线程：`accept` 循环，接受连接后把 fd 投入任务队列
- **pthread 线程池**：默认 `N = 8` 个工作线程（`config/app.ini` 的 `pool_size` 可配），从任务队列取连接处理读写与业务分发
- 任务队列：`pthread_mutex_t` + `pthread_cond_t` 保护，**不使用 QThread 替代**（说明书点名 pthread，是考核点）
- 会话表跨线程共享，读写均加 `pthread_rwlock_t`
- **每个工作线程持有自己的 QSqlDatabase 连接**，连接名由线程 id 生成；禁止跨线程共享连接（硬性规则第 2 条）

## 对应实现

| 文件 | 职责 |
| --- | --- |
| `thread_pool.h/.cpp` | pthread 线程池 |
| `tcp_server.h/.cpp` | accept 循环 + 连接投递 |
| `session.h/.cpp` | 会话表（pthread_rwlock） |
| `dispatcher.h/.cpp` | 命令字分发 |

# AGENTS.md — 通用 Agent 入口（Codex 等）

> **本文件不含正文。项目上下文以 [CLAUDE.md](CLAUDE.md) 为唯一事实来源，请先完整读它。**

不同成员用的工具不同：Claude Code 读 `CLAUDE.md`，Codex 读 `AGENTS.md`。两份文件只为让不同工具进入同一份上下文，因此本文件**只写工具差异**。

> ⚠️ 禁止把 CLAUDE.md 内容抄到这里。两份长文档必然分叉，那正是本项目要防的事故。冲突时一律以 CLAUDE.md 为准。

## 会话开始必读

1. [CLAUDE.md](CLAUDE.md) —— 技术基线、目录归属、五条硬性规则、业务规则
2. [docs/protocol.md](docs/protocol.md) —— 通信协议（冻结契约）
3. [docs/db-schema.sql](docs/db-schema.sql) —— 数据库结构（冻结契约）
4. [WORKFLOW.md](WORKFLOW.md) —— 日常流程与指令模板

涉及网络收发的任务，额外读 `common/frame.h`。

## Codex 使用补充

- **上下文不会自动携带**：新会话只读到本文件，需在首轮明确让它打开上面四份，否则它不知道协议与表结构。
- **审批**：涉及 `docs/`、`common/`、`CLAUDE.md`、`AGENTS.md` 的写操作一律人工确认；自己目录下的常规编辑可放开。
- **不要让 agent 执行 `git push`**，本人确认后手动推。
- **契约变更通知后必须重开会话**——旧会话缓存的是过期协议。

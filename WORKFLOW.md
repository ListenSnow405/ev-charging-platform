# 开发流程：五人 · 全员 Agent 辅助

> 讲**怎么干活**。规则见 [CLAUDE.md](CLAUDE.md)，分工见 [DIVISION-OF-LABOR.md](DIVISION-OF-LABOR.md)，冻结与环境见 [docs/conventions.md](docs/conventions.md)。本文件不重复它们。

```
【一次性】 clone → check-env.sh → 装缺的包 → build-all.sh → 跑通空壳
【每次开会话】 git pull → 让 agent 读契约 → 声明目录边界
【每个功能点】 认领 → 写指令(§2) → 读懂产出(§3) → 编译自测 → 小步提交
【要改契约】 停手 → 群里提 → 属主改 → 全组重新同步 agent 上下文
```

## 1. 准备

**一次性**（每人一次）：

```bash
git clone <仓库> && cd ev-charging-platform
bash scripts/check-env.sh L3     # 换成自己的线号；按提示装包，再跑一次直到通过
bash scripts/build-all.sh        # 构建 + 建库 + 生成 config/app.ini
./build/bin/ecp-server           # 终端 1；再另起自己那条线的程序，确认空壳能跑
```

把 `check-env.sh` 的「结论」一行贴群里，五人都贴完 W1 环境关才算过。

**每次开 agent 会话前三步，别省**：

1. `git pull --rebase origin main` —— 契约可能变了，基于旧协议写的代码编译能过、联调必炸
2. **点名让 agent 读契约** —— Claude Code 自动读 `CLAUDE.md`、Codex 读 `AGENTS.md`，但**两者都不会自动打开 `docs/protocol.md` 和 `docs/db-schema.sql`**
3. 声明目录边界（见下方模板【边界】段）

## 2. 写第一条指令（模板，照抄改）

从 [docs/protocol.md](docs/protocol.md) 第 4 节挑一个命令字，开分支 `feat/L2-user-login`，然后：

```
【背景】
我负责 L2 数据与业务服务。请先读，再动手：
  CLAUDE.md / server/CLAUDE.md / docs/protocol.md 第4.1、5节
  docs/db-schema.sql（重点 t_user）/ server/biz/README.md

【任务】
实现命令字 1001「手机号免密登录 / 首次自动注册」，注册到 Dispatcher。

【边界】
  只改 server/biz/ 和 server/dao/
  docs/ 和 common/ 是冻结契约，只读。若认为契约有问题，先告诉我，不要自己改
  不要新建表、不要改表结构、不要新增命令字

【必须遵守】
  金额 qint64 整数分；连接用 ecp::threadDb()；SQL 用 prepare+bindValue
  返回值只能是 common/error_code.h 已定义的错误码；Qt 6.2.4，不用 Qt5 废弃 API

【验收】
  编译通过；非 11 位 → ERR_PHONE_FORMAT；不存在 → 自动注册，昵称「用户」+后4位
  status=1 → ERR_USER_FROZEN；成功返回 token/userId/nickname/balance/status
```

四段缺一不可：**读什么、做什么、不许碰什么、怎么算做完**。缺【边界】，agent 会顺手改契约让自己编译通过；缺【验收】，它不处理边界条件。

**一次只做一个功能点**，不要让它一口气生成整个模块。

## 3. 读懂产出（最容易被跳过）

**agent 写完，自己先从头读一遍。** 评审和答辩会问「这段为什么这么写」，没读过当场就露。

按 [CLAUDE.md 第 5 节](CLAUDE.md)那五条逐项查：绕过 `FrameCodec` 自己解析？跨线程共用 DB 连接？金额出现 `double`？网络线程直接改控件？Qt5 废弃 API？

有问题**补充约束重来**，不要自己手工改完了事——同一个坑下次它还踩。

顺手把**为什么这么设计**记进设计文档（为什么用整数分、为什么大屏不走服务端）。这些判断是 agent 生成不出来的部分，也正是评分最看重的。

## 4. 自测与提交

```bash
bash scripts/build-all.sh && ./build/bin/ecp-server &
./build/bin/ecp-user      # 点一遍
```

**重点测异常路径**：余额不足、账号冻结、断网、重复提交、未结算拦截。agent 写的正常流程通常没问题，异常路径几乎必漏。

提交只加自己目录下的文件，一个功能点一次，信息写人话。**不要让 agent 执行 `git push`**。合入后群里说一声，模块 `CLAUDE.md` 的 TODO 打勾。

## 5. 要改契约：停手

发现协议少字段、表少一列、错误码不够——**不要自己改，也不要让 agent 改**。走 [docs/conventions.md 第 2 节](docs/conventions.md)。

其中「改完通知全组重新同步 agent 上下文」最容易漏：**人知道了不等于 agent 知道了。** Codex 用户建议直接重开会话。

## 6. 这些不要交给 agent

环境与依赖、跨进程联调、需求判断、设计决策、答辩讲解——这几项 agent 加速几乎为零，硬交给它只是浪费时间。联调靠两端日志对照（格式统一），比反复问 agent 快。

## 7. 节奏

**每周交叉试读**（L2 组织）：互看代码，按 §3 清单查。全员用 agent 时评审强度要**提高**而非降低——agent 最擅长生成「看起来对」的错代码。

**W3 联调周**（L3 主责）：每天一次例会。客户端此前走 `mock/` 假数据，这周才切真实链路——`mock/` 要全程保留，任何时候都能脱离服务端演示。

## 附：常见情况

| 情况 | 怎么办 |
| --- | --- |
| agent 要改 `docs/` 或 `common/` | 拒绝，走 §5。这是它最常见的越界 |
| 编造不存在的表或字段 | 它没读 schema。重新点名让它读，别人工纠正一个字段了事 |
| 报 Qt5/Qt6 错误 | 提醒它本项目是 **Qt 6.2.4**，让它查 Qt6 正确用法 |
| 想新增命令字 | 先在 `docs/protocol.md` 占号（走 §5），再写代码 |
| 连续两三轮修不好同一个错 | 停下自己读报错。通常是它误解了某个前提，直接告诉它前提更快 |
| GUI 程序报 `could not connect to display` | SSH 会话没有 `DISPLAY`，不是 Qt 坏了。见 [conventions.md 第 5 节](docs/conventions.md) |
| 客户端报「不支持的请求类型」 | 该命令字的 handler 还没实现（L2 的活），不是链路故障。服务端能打印出命令字就说明链路是通的 |

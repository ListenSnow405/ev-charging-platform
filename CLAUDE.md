# CLAUDE.md — 全组 Agent 共享上下文

> Agent 上下文的**唯一事实来源**。Codex 用户见 [AGENTS.md](AGENTS.md)（不重复内容）。
> 分工 [DIVISION-OF-LABOR.md](DIVISION-OF-LABOR.md)　流程 [WORKFLOW.md](WORKFLOW.md)　冻结与环境 [docs/conventions.md](docs/conventions.md)　需求 [docs/project-spec.md](docs/project-spec.md)

## 0. 标记约定

- `[说明书]` —— 说明书正文明文要求，**不得擅自更改**；偏离需 PM 确认并记入变更记录
- `[本组自定]` —— 说明书未规定，本组决定，走变更流程可调整

**选型总约定**：一律以说明书**正文文字**（1.2 / 1.4 / 1.5 / 1.6 / 2.x）为准；系统结构图仅作模块与界面参考，**不作为选型依据**。

## 1. 项目

Linux + Qt 的电动汽车充电桩管理平台：充电用户端、PC 管理端两个 Qt 客户端，经自定义 TCP Socket 连接一个多线程业务服务端，数据存 QSQLite；另有 ECharts 大屏与 Python 负荷预测。

## 2. 技术基线

| 项 | 结论 | 标记 |
| --- | --- | --- |
| 运行系统 | Ubuntu 22.04+ | `[说明书]` 1.5 |
| Qt | **6.2.4**，全组一致 | `[本组自定]` |
| 语言 / 标准 | C++17 | `[说明书]` 1.6 + 自定 |
| 数据存储 | **QSQLite** 单一主库，图片存文件路径 | `[说明书]` 1.6 |
| 网络通信 | **Socket 编程**；服务端用 POSIX 原生 socket，非 QTcpServer | `[说明书]` 1.6 |
| 并发模型 | **pthread 线程池**，主框架为多线程结构 | `[说明书]` 1.6 + 1.2 |
| 报文格式 | 4 字节大端长度头 + UTF-8 JSON 体 | `[本组自定]` |
| JSON | Qt 自带 `QJsonDocument`，不引第三方库 | `[本组自定]` |
| 金额 | 一律整数「分」(`qint64`)，仅显示层除 100 | `[本组自定]` |
| 时间 | `TEXT` 格式 `yyyy-MM-dd HH:mm:ss` | `[本组自定]` |
| 大屏取数 | L5 只读 SQLite 导出 JSON 快照，大屏轮询静态文件 | `[本组自定]`，说明书未规定 |

> 大屏不走服务端 HTTP：浏览器连不上自定义 TCP，此方案使服务端保持纯 Socket，L2 也不必写 HTTP 服务。**此项是推论，需在设计文档写明理由。**

## 3. 冻结契约

`docs/protocol.md`(L1)　`docs/db-schema.sql`(L2)　`common/**`(L1)　`CLAUDE.md`/`AGENTS.md`(全组)

**非属主只读。** 需要改动 → 停手，走 [docs/conventions.md](docs/conventions.md) 第 2 节变更流程，不要自己动手改了再说。

## 4. 目录归属

**agent 只能修改自己所属目录下的文件。**

```
docs/ common/          冻结契约，属主见上

scripts/build-all.sh   L3
scripts/check-env.sh   L5

server/net/  tools/    L1                      server/biz/ server/dao/  L2
admin-client/          L3                      user-client/             L4
dataviz/  ml/          L5
```

## 5. 五条硬性规则

1. **收发一律走 `common/frame.h` 的 `FrameCodec`。** TCP 是字节流没有消息边界，禁止假设一次 `read()`/`readyRead()` 就是一个完整包。
2. **禁止跨线程共享 `QSqlDatabase` 连接。** 服务端一律用 `ecp::threadDb()`，它按线程 id 生成独立连接。
3. **金额用 `qint64` 整数分，禁止出现 `double`/`float` 金额变量。** 显示用 `ecp::fenToYuan()`。
4. **禁止在网络线程直接操作 Qt 控件。** 跨线程一律信号槽 + `Qt::QueuedConnection`。
5. **本项目是 Qt 6.2.4，禁止 Qt5 废弃 API。** 用 `QRegularExpression` 而非 `QRegExp`；Qt6 的 charts 类不在 `QtCharts` 命名空间，不要写 `QT_CHARTS_USE_NAMESPACE`。不要凭 Qt5 记忆写。

## 6. 业务规则（说明书明文，不得改动）

- 管理员默认账号 **admin / 123456**
- 用户端**手机号免密登录**：11 位手机号，存在即登录；不存在**自动注册**，昵称 `用户`+后 4 位，默认灰色头像
- 电桩状态三种：**在用 / 闲置 / 故障**；类型两种：**快充 / 慢充**
- 用户状态两种：**正常 / 冻结**，管理员可手动冻结解冻
- **进入充电页前必须校验未完成订单**；有则提示并**强制跳转结算页**
- 充电流程：预约 → 开始充电 → 计费 → 结算
- 管理端需 **近 7 / 30 日**趋势与**今日 / 本月 / 总营收**三指标
- 需支持**远程重启**电桩、**手机号模糊搜索**
- 机器学习预测未来 **1h / 6h / 24h** 负荷、空闲桩数、高峰时段

## 7. 编码规范

- 文件小写下划线；类 `PascalCase`；成员 `m_` 前缀；常量枚举 `UPPER_SNAKE`；头文件 `#pragma once`
- 对外接口返回 `common/error_code.h` 的错误码，不用裸 `bool`；错误先记日志再返回
- 日志统一 `LOG_I / LOG_W / LOG_E`，禁止混用 `qDebug()` / `printf`
- SQL 一律 `prepare` + `bindValue` 参数绑定，禁止字符串拼接
- 涉及说明书要求处，注释带 `[说明书]` 标记与条目号
- 小步提交，一个功能点一次，信息写人话

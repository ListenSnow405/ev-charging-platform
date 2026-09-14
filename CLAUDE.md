# CLAUDE.md — 全组 Agent 共享上下文

> Agent 上下文的**唯一事实来源**。Codex 用户见 [AGENTS.md](AGENTS.md)（不重复内容）。
> 分工 [DIVISION-OF-LABOR.md](DIVISION-OF-LABOR.md)　流程 [WORKFLOW.md](WORKFLOW.md)　冻结与环境 [docs/conventions.md](docs/conventions.md)　需求 [docs/project-spec.md](docs/project-spec.md)

## 0. 标记约定

- `[说明书]` —— 说明书正文明文要求，**不得擅自更改**；偏离需 PM 确认并记入变更记录
- `[本组自定]` —— 说明书未规定，本组决定，走变更流程可调整
- `[二阶段]` —— 第二阶段任务书明文要求（大数据与机器学习子系统），**同样不得擅自更改**

**选型总约定**：一律以说明书**正文文字**（1.2 / 1.4 / 1.5 / 1.6 / 2.x）为准；系统结构图仅作模块与界面参考，**不作为选型依据**。

## 1. 项目

Linux + Qt 的电动汽车充电桩管理平台：充电用户端、PC 管理端两个 Qt 客户端，经自定义 TCP Socket 连接一个多线程业务服务端，数据存 QSQLite；另有 ECharts 大屏与 Python 负荷预测。

**第二阶段**在其之上并行建设**大数据子系统**：原始数据落 HDFS，Spark 做清洗与多维分析，Spark MLlib 做负荷预测，结果存 MySQL，经 Flask 供 Vue3 + DataV 大屏取用。
两阶段**并存**——Qt 业务平台一行不改，只共用同一份数据源。任务清单见 [PHASE2-PLAN.md](PHASE2-PLAN.md)。

## 2. 技术基线

> 两阶段技术栈**基本不重叠**，各自独立，不要把一边的结论套到另一边。

### 2.1 业务平台（第一阶段）

| 项 | 结论 | 标记 |
| --- | --- | --- |
| 运行系统 | Ubuntu 22.04+ | `[说明书]` 1.5 |
| Qt | **6.2.4**，全组一致 | `[本组自定]` |
| 语言 / 标准 | C++17 | `[说明书]` 1.6 + 自定 |
| 数据存储 | **QSQLite** 单一主库，图片存文件路径 | `[说明书]` 1.6 |
| 网络通信 | **Socket 编程**；服务端 POSIX socket + epoll，叠加 Qt QSocketNotifier/QTimer | `[说明书]` 1.6 |
| 并发模型 | **pthread 线程池** + 独立 epoll IO 线程，主框架为多线程结构 | `[说明书]` 1.6 + 1.2 |
| 报文格式 | 4 字节大端长度头 + UTF-8 JSON 体 | `[本组自定]` |
| JSON | Qt 自带 `QJsonDocument`，不引第三方库 | `[本组自定]` |
| 金额 | 一律整数「分」(`qint64`)，仅显示层除 100 | `[本组自定]` |
| 时间 | `TEXT` 格式 `yyyy-MM-dd HH:mm:ss` | `[本组自定]` |
| 大屏取数 | **一阶段大屏维持不变**：L5 只读 SQLite 导出 JSON 快照，轮询静态文件 | `[本组自定]`，说明书未规定 |

> **Qt 服务端始终保持纯 Socket，不提供 HTTP。** 浏览器连不上自定义 TCP：一阶段的解法是导出静态快照；
> 二阶段引入的 Flask **不挂在 Qt 服务端上**，而是大数据子系统自己的一层，只读 MySQL 分析结果。
> 两条路径都不需要 L2 去写 HTTP 服务，说明书 1.6 的 Socket 考核点完好。**此项是推论，理由需在设计文档写明。**

### 2.2 大数据子系统（第二阶段）

| 项 | 结论 | 标记 |
| --- | --- | --- |
| Python | **3.11 或 3.12**；系统自带 3.10 不达标，apt 的 `python3.11` 候选是 RC 版，走 deadsnakes/pyenv | `[二阶段]` |
| 文件存储 | **Hadoop 3.x / HDFS** 作 ODS 只读原始层；本地测试可用文件系统，答辩须落 HDFS | `[二阶段]` |
| 计算引擎 | **PySpark**，负责数据清洗与多维分析 | `[二阶段]` |
| 机器学习 | **Spark MLlib**，不再沿用第一阶段的 scikit-learn | `[二阶段]` |
| 结果存储 | **MySQL**，只存分析结果，不存明细 | `[二阶段]` |
| Web 层 | **Flask**，只读接口 | `[二阶段]` |
| 前端 | **Node.js 23+** / **Vue 3** | `[二阶段]` |
| 大屏 | **DataV 开源 Vue3 组件库**（非阿里云 SaaS），图表类型不得单一 | `[二阶段]` |
| 分析维度 | **≥ 8 个**，其中**≥ 2 组对比分析** | `[二阶段]` |
| 数据清洗 | 按 [数据清洗基本流程-操作SOP.md](数据清洗基本流程-操作SOP.md) 六阶段走，每阶段产出齐全 | `[二阶段]` |

> **版本联动**：PySpark 与 Python 版本强绑定。安装顺序必须是 **JDK → Python → PySpark**，
> 装完先跑通一个最小 `SparkSession` 再写业务代码。具体版本组合以安装当天官方兼容矩阵为准，**不要凭记忆定版本**。

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

── 第二阶段 · 大数据子系统 ───────────────────────
bigdata/spark/         清洗与分析 job          L5
bigdata/mllib/         Spark MLlib 建模        L5
bigdata/api/           Flask 只读接口          L5
bigdata/web/           Vue3 + DataV 大屏       L5
scripts/check-env-phase2.sh                    L5
```

> 第二阶段暂由 **L5** 统一承担（大数据可视化与机器学习本就是其本职）。若后续分工调整，改本节并走变更流程。
> `bigdata/**` 目前**不是冻结契约**；待 MySQL 结果表结构定稿后，再按第 3 节决定是否冻结。

## 5. 硬性规则

### 5.1 第一阶段 · Qt / C++（五条）

1. **收发一律走 `common/frame.h` 的 `FrameCodec`。** TCP 是字节流没有消息边界，禁止假设一次 `read()`/`readyRead()` 就是一个完整包。
2. **禁止跨线程共享 `QSqlDatabase` 连接。** 服务端一律用 `ecp::threadDb()`，它按线程 id 生成独立连接。
3. **金额用 `qint64` 整数分，禁止出现 `double`/`float` 金额变量。** 显示用 `ecp::fenToYuan()`。
4. **禁止在网络线程直接操作 Qt 控件。** 跨线程一律信号槽 + `Qt::QueuedConnection`。
5. **本项目是 Qt 6.2.4，禁止 Qt5 废弃 API。** 用 `QRegularExpression` 而非 `QRegExp`；Qt6 的 charts 类不在 `QtCharts` 命名空间，不要写 `QT_CHARTS_USE_NAMESPACE`。不要凭 Qt5 记忆写。

### 5.2 第二阶段 · 大数据子系统（五条）

6. **ODS 原始层只读**（本地为目录，答辩为 HDFS）**。** 清洗、纠错、填充一律在 DWD 加工层做，原始快照一个字节不改——这是 SOP 第 2 节的硬性要求，也是「可复现、可回溯」的唯一保证。
7. **金额仍是整数「分」，电量仍是 `kwh_x100`。** Spark 聚合一律在整数上做，只在最终展示层除以 100；禁止把 `amount` 读成 float 再累加八千笔——与第 5.1 节第 3 条同源。
8. **Flask 只读。** 接口只查 MySQL 分析结果表，不写任何业务库，不现场触发 Spark job。
9. **删除必须留痕。** 清洗中的删除、修正、填充、合并都要记录数量、原因与样例；无法判定的记录进待核清单，**不静默丢弃**。
10. **分析不得直连 `charging.db`。** 一律走 ODS 原始层——本地开发期该层是文件系统目录，答辩时是 HDFS，**代码只认路径不认介质**。这样才能与业务库解耦、可重跑，也才能在本地与 HDFS 之间平移。

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

> 以上为第一阶段 C++/Qt 口径。**第二阶段**（Python / Vue）沿用其中与语言无关的三条：错误先记日志再返回、SQL 参数绑定禁止拼接、小步提交；
> 命名与文件组织按各自生态惯例（Python 用 PEP 8 蛇形，Vue 组件用 `PascalCase`），不强套 C++ 规范。

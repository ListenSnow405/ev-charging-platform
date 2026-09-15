# 电动汽车充电桩应用管理平台

面向电动汽车充电场景的综合管理平台实训项目，覆盖"用户端—服务端—数据端"全业务链路，融合 Linux 系统开发、Qt 跨平台 UI、数据库设计与大数据可视化等技术。

## 项目目标

- 完整体验从需求理解、整体/详细设计、开发、测试到发布的软件工程全流程
- 掌握 Linux 系统下的应用程序开发方法与 Qt Creator 工具链
- 掌握 Qt 图形库进行 UI 设计的能力
- 掌握 Linux 下多线程（pthread）编程与 Socket 通信编程方法
- 培养快速学习新技术、独立解决问题的能力

## 系统架构

![系统结构图](docs/assets/project-spec/system-architecture.png)

五个子系统：**充电用户端**（Qt，找桩 / 导航 / 个人中心 / 充电全流程）、**PC 管理端**（Qt，
数据总览 / 电站 / 电桩 / 用户 / 订单 / 负荷预测 / 碳排放报告）、**业务服务端**（自定义 TCP + epoll + pthread 线程池）、
**数据库端**（QSQLite 单一主库）、**数据端**（负荷预测模型 + ECharts 大屏）。

分层结构、命令字分段、目录归属与关键设计决策见 [ARCHITECTURE.md](ARCHITECTURE.md)，逐条需求见
[docs/project-spec.md](docs/project-spec.md)。

**第二阶段**在其之上并行建设**大数据子系统**：原始数据落 HDFS，Spark 做清洗与多维分析，
Spark MLlib 做负荷预测，结果存 MySQL，经 Flask 供 Vue3 + DataV 大屏取用。
两阶段**并存**——上面的 Qt 业务平台一行不改，只共用同一份数据源。
子系统导览见 [bigdata/README.md](bigdata/README.md)，任务与验收见 [PHASE2-PLAN.md](PHASE2-PLAN.md)。

## 技术栈

| 类别 | 技术 |
| --- | --- |
| 客户端/服务端 UI | Qt 框架 |
| 核心逻辑 | C++ |
| 业务数据存储 | QSQLite（说明书基础要求）|
| 网络通信 | Socket 编程 |
| 并发模型 | 多线程（pthread） |
| 数据可视化大屏 | Web + ECharts |
| 外部服务 | 腾讯地图 Web API、短信服务、支付服务（模拟）、天气 API |

**第二阶段**（与上表基本不重叠，各自独立）：

| 类别 | 技术 |
| --- | --- |
| 文件存储 | Hadoop 3.x / HDFS（ODS 只读原始层） |
| 计算引擎 | PySpark（数据清洗与多维分析） |
| 机器学习 | Spark MLlib |
| 结果存储 | MySQL（只存分析结果） |
| Web 层 | Flask（只读接口） |
| 前端与大屏 | Node.js 23+ / Vue 3 / DataV 开源 Vue3 组件库 |

## 开发环境

Ubuntu 22.04+（VMware 17）、Qt **6.2.4**、g++ / C++17、Python 3.10（数据端）。
第二阶段另需 JDK 17、Python **3.11**（独立的 `.venv-phase2`）、Hadoop 3.3.6、MySQL 8、Node 23+。
完整基线与自检办法见 [docs/conventions.md 第 5 节](docs/conventions.md)，
两个阶段各有一个自检脚本：`scripts/check-env.sh` 与 `scripts/check-env-phase2.sh`。

## 目录结构

```
.
├── docs/                     # 契约与过程文档
│   ├── project-spec.md       # 项目说明书（需求来源）
│   ├── protocol.md           # 通信协议（冻结契约，属主 L1）
│   ├── db-schema.sql         # 数据库结构（冻结契约，属主 L2）
│   ├── db-schema-ext-08.sql  # 扩展模块建表（幂等，随构建自动执行）
│   ├── conventions.md        # 规范与变更记录（SCML 维护）
│   ├── RUNBOOK.md            # 运行手册：构建 / 启动 / 流水线 / 测试 / 排障
│   ├── map_key.md            # 腾讯地图 Key 申请与额度
│   └── expand/               # 扩展模块规格与认领看板
├── common/                   # 全项目共享基座（冻结契约，属主 L1）
├── server/                   # 业务服务端  net/=L1  biz/ dao/=L2
├── admin-client/             # PC 管理端           L3
├── user-client/              # 充电用户端         L4
├── dataviz/                  # ECharts 大屏       L5
├── ml/                       # 机器学习与数据生成  L5
├── bigdata/                  # 第二阶段大数据子系统 L5
│   ├── spark/                # 导出 / 清洗 / 多维分析 job
│   ├── mllib/                # Spark MLlib 建模
│   ├── api/                  # Flask 只读接口
│   ├── web/                  # Vue3 + DataV 大屏
│   ├── ods/  dwd/  analysis/ # 数据分层产物（可重跑生成）
│   └── quality/              # 清洗六阶段产物与评估报告
├── tools/pile-simulator/     # 电桩模拟器         L1
├── config/                   # 本地配置模板（app.ini 不入库）
├── scripts/                  # 构建、环境自检、冒烟与集成测试
├── ev-charging-platform.pro  # 顶层 qmake 工程
├── ARCHITECTURE.md           # 系统架构与责任归属
├── CLAUDE.md / AGENTS.md     # 全组 agent 共享上下文
├── DIVISION-OF-LABOR.md      # 团队分工方案
├── WORKFLOW.md               # 日常开发流程
├── DEFENSE-SCRIPT.md         # 答辩 PPT 大纲与讲稿
├── PHASE2-PLAN.md            # 第二阶段任务清单、验收对照与踩坑记录
├── 数据清洗基本流程-操作SOP.md  # 第二阶段清洗必须遵循的六阶段流程
└── README.md
```

## 开发体制

项目按 PM / TL / PRL / SCML / PE 的实训角色分工推进，具体职责说明见项目说明书 [1.7 开发体制](docs/project-spec.md#17-开发体制)。

五人编制的具体任务划分、技术基线、以及全员 agent 辅助开发下的协作约束，见 [分工方案](DIVISION-OF-LABOR.md)。

开发前请先阅读 [CLAUDE.md](CLAUDE.md)（使用 Codex 的成员从 [AGENTS.md](AGENTS.md) 进入），其中的技术基线、目录归属与硬性规则适用于全组；日常怎么配合 agent 干活见 [开发流程](WORKFLOW.md)。

## 快速开始

```bash
bash scripts/check-env.sh        # 环境自检（先跑这个）
bash scripts/build-all.sh        # 一键构建 + 建库 + 生成 config/app.ini
./build/bin/ecp-server           # 服务端（先启动），再起 ecp-admin / ecp-user
```

第二阶段是独立的一条线，不需要启动 `ecp-server`：

```bash
bash scripts/check-env-phase2.sh   # 二阶段环境自检
bash scripts/hdfs-ctl.sh start     # 起 HDFS（不会开机自启）
```

完整的启动参数、预测流水线、测试入口与常见故障见 **[docs/RUNBOOK.md](docs/RUNBOOK.md)**
（第 1–6 节为第一阶段，**第 7 节为第二阶段**）。

> **技术选型约定**：一律以说明书**正文文字说明**为准，系统结构图仅作模块与界面参考，不作为选型依据。
> 全部文档与代码注释用 `[说明书]` / `[本组自定]` 两个标记区分「说明书明文要求」与「本组自行决定」。

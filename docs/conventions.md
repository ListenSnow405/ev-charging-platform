# 规范与变更记录

> 属主 **SCML（L5）**。编码规范见 [CLAUDE.md](../CLAUDE.md) 第 7 节，本文件不重复；只承载**冻结跟踪、变更记录、环境与配置基线、成果物归档**。
> `[说明书]` 1.7：配置负责人「按照已定义的规范对成员的开发流程及成果物进行跟踪，并对过程成果物进行配置」——本文件即该职责载体。

## 1. 冻结状态

| 契约 | 属主 | 状态 | 冻结日期 | 版本 |
| --- | --- | --- | --- | --- |
| `docs/protocol.md` | L1 | ✅ 已冻结 | 2026-09-02 | v1.0 |
| `docs/db-schema.sql` | L2 | ✅ 已冻结 | 2026-09-02 | v1.0 |
| `common/**` | L1 | ✅ 已冻结 | 2026-09-02 | v1.1 |
| `CLAUDE.md` / `AGENTS.md` | 全组 | ✅ 已冻结 | 2026-09-02 | **v2.0**（2026-09-14 第二阶段基线并入） |

**冻结** = 成果物定稿后进入受控状态，不再由个人随手改。判定三条同时成立：① 已合入 `main`　② 全组评审通过并知晓　③ 此后仅属主可改。
**冻结不等于永不修改**——协议会补命令字，schema 会加字段。它管的是「怎么改」，见下节。

## 2. 变更流程

① 群里提出（改什么 / 为什么 / 影响哪几条线）→ ② 属主与受影响担当评审，PRL 把关 → ③ **由属主一人修改** → ④ 群里通知，所有人**重新同步各自 agent 的上下文** → ⑤ SCML 在第 3.1 节记一条。**提出但尚未评审的申请，先挂在第 3.2 节。**

> 第 ④ 步是 agent 协作特有的，也最容易漏：**人知道了不等于 agent 知道了**，旧会话缓存的是过期协议。Codex 用户建议重开会话。

## 3. 变更记录

### 3.1 已生效变更

| 日期 | 契约 | 变更 | 评审 | 已通知 |
| --- | --- | --- | --- | --- |
| 2026-09-02 | 全部四项 | 初版编写完成，提交评审 | — | — |
| 2026-09-02 | 全部四项 | **全组评审通过，正式冻结 v1.0** | 全组 | ✅ |
| 2026-09-02 | `CLAUDE.md` | Qt 版本写死为 **6.2.4**（全组虚拟机一致） | 全组 | ✅ |
| 2026-09-02 | 工程骨架 | 建立四个 C++ 子工程与顶层 qmake 工程，编译验证通过 | — | ✅ |
| 2026-09-02 | `common/logger.h` | 修正编译错误：`pthread_t → quintptr` 的 `reinterpret_cast` 在 64 位 Linux 非法，改 `static_cast<quint64>` | PRL | ✅ |
| 2026-09-02 | `common/app_path.h` | 新增 `resPath()`：从 Qt Creator 运行时工作目录是构建目录，原先按相对路径找 `config/app.ini` 与 `charging.db` 会失败。改为可执行文件上溯两级定位项目根 | L1 | ✅ |
| 2026-09-02 | 全部文档 | 消除跨文件重复，按「每份内容只留一处」重排，总量减少约六成 | 全组 | ✅ |
| 2026-09-02 | `config/app.ini` | 线程池大小改为可配置项 `pool_size`，原先在 `main.cpp` 写死；每条连接占一个线程直到断开，该值即最大并发连接数 | L1 | ✅ |
| 2026-09-02 | `.gitignore` | `config/` 规则收紧为默认全挡、仅放行 `*.example`；原 `config/*.ini` 挡不住其他后缀的凭据文件 | SCML | ✅ |
| 2026-09-02 | `server/biz/` | 实现 1001 手机号免密登录（含自动注册、冻结拦截）、1002 用户信息、2001 管理员登录，作为 L2 的样板实现 | L2 | ✅ |
| 2026-09-03 | `server/biz/` | L2 服务批量合入（PR #3）：1003–1006 / 1101–1102 / 1201–1202 / 1206–1207 / 2101–2103 / 2111 / 2201–2202 / 2304，共 20 个命令字注册 | L2 | ✅ |
| 2026-09-03 | `admin-client/` | L3 六个界面 mock 完成，充电站管理与用户管理已切真实接口 | L3 | ✅ |
| 2026-09-03 | `docs/protocol.md`(→v1.1) / `server/net/session.*` / `server/biz/user_management_service.cpp` | **越权代改**：L1、L2 暂时无法操作，经用户明确指示由 L5 代为落地 CR-001+CR-003，非正常流程，详见第 3.2 节对应 CR 的越权记录 | 用户指示 | ✅ **L1/L2 均已追认**（L2 明确接受 CR-003 方案并确认 7200s 为无活动超时；CR-001 已并入冻结协议） |
| 2026-09-04 | `common/protocol.h` | 新增 `CMD_STAT_LOAD_FORECAST = 2305`，与协议 v1.1 的 2305 定义对齐 | L1 | ✅ |
| 2026-09-04 | `server/biz/wallet_service.cpp` | 1005/1006 补角色校验；1005 补冻结检查；事务由默认 `BEGIN` 改为 `BEGIN IMMEDIATE`，避免 SQLite 读锁升写锁竞态下的余额丢失更新 | L2 | ✅ |
| 2026-09-04 | `server/biz/statistics_service.cpp` | 实现 2301 营收 / 2302 趋势 / 2303 电桩状态统计 | L2 | ✅ |
| 2026-09-04 | `ml/gen_history.py` / `ml/export_snapshot.py` | 按 CR-002 批复三条意见改造生成器；并把快照导出的营收/趋势/状态口径对齐服务端 2301/2302/2303（含补零），此前大屏与管理端会显示成两个形状 | L5 | ✅ |
| 2026-09-02 | `docs/conventions.md` | 第 3 节拆为 3.1 已生效 / **3.2 待评审变更申请**；原先只有「改完之后」的记录，没有「提出到批准之间」的落点，CR 只能停在群聊里翻不到 | SCML | ⬜ |
| 2026-09-02 | `.gitignore` | 补 `*.db-shm` / `*.db-wal`；原规则只挡 `*.db` 和 `*.db-journal`，SQLite 走 WAL 模式时这两个边车文件会漏进仓库 | SCML | ⬜ |
| 2026-09-02 | `ml/CLAUDE.md` | 数据库权限改为「运行期只读 / 离线播种可写」两条并列规则，并挂 CR-002 未批前禁止 `--commit` | SCML | ⬜ |
| 2026-09-05 | `charging.db` | L5 按 CR-002 流程正式落库：8292 单 + 32 条设备日志 + 18 行预测；备份 `charging-bak-20260905-082255.db`，五张禁改表哈希逐字节未变 | L5 | ✅ |
| 2026-09-07 | `docs/expand/` | 扩展模块实施路径与认领看板定稿；B3（ext 建表自动执行）、B4（`t_sys_config` 功能开关）落地 | L5 代实现 | ✅ L3/L2 于 09-08 追认 |
| 2026-09-08 | `common/error_code.h` | 新增 `ExtMsgProvider` 函数指针挂钩，扩展错误码可回中文 `msg`；冻结文件**不 include** `error_code_ext.h`，未注册时行为与从前完全一致 | L1 授权 | ✅ |
| 2026-09-08 | `server/net/` | 网络层重构：移除「一连接一线程」阻塞模型，改为 epoll IO 线程 + Qt 主线事件循环；新增设备注册表与用户推送注册表。**`pool_size` 语义随之改变**——不再是最大并发连接数，而是最大并发业务处理数（上溯 09-02 那条记录） | L1 | ✅ |
| 2026-09-08 | `server/biz/` `common/protocol_ext.h` | 扩展模块 08 碳减排与能源报告全栈交付，3740–3747 八个命令字 + 管理端页 + 碳排放大屏 + 独立对拍 | L5 | ✅ L1/L2/L3 追认 |
| 2026-09-08 | `server/biz/order_flow_service.cpp` `pile_service.cpp` | R0 核心闭环补齐：1203–1205 计费结算三件套、2112→9003 远程重启、1208 充电推送、2305 预测查询 | L2/L4/L1 | ✅ |
| 2026-09-09 | 全部文档 | 第二次文档整理：新增 [RUNBOOK.md](RUNBOOK.md) 收拢分散在六份文档里的运行命令；清除各模块文档中已完成的 TODO 与过期状态；`docs/expand/08-实现规划.md` → `08-运行手册.md`，技术路线改写为运行文档 | SCML | ⬜ |
| 2026-09-10 | `server/biz/ext_08_carbon_service.cpp` `common/protocol_ext.h` `common/error_code_ext.h` | 修复 3747 撤销因子：时间线改为**只由启用因子构成**（停用行不再参与前驱判定，此前会被误判成「多个因子闭合在同一时刻」而返回 5001），对已停用因子改为幂等空操作；新增 3748 `CMD_EXT_FACTOR_PURGE` 彻底删除因子及其日聚合、报告与导出文件，新增错误码 6707。ext 头文件段内自治，非冻结契约 | L5 | ⬜ 待 L1 复核 |
| 2026-09-11 | `user-client/main_window.cpp` | **修复 [说明书] 1.4「用户端优先推荐低拥堵、高空闲率的充电站」实际未生效**：客户端确实发了 `sortBy=1`，但收到响应后又用 `pileIdle`（**当前**空闲数）本地重排，把服务端按 `t_load_forecast` **预测**拥堵度排好的顺序整个覆盖掉。本地重排改为同一口径（`congestion` 升序 → 无预测的 `-1` 排最后 → 退回 `pileIdle`/距离）；站点卡片开始读取并展示 `congestion`/`idleForecast`（此前两个字段一次都没用过），分档阈值 0.8 与管理端 `forecast_page` 的负荷预警判定一致；下拉项「空闲优先」改名「低拥堵优先」。用 `charging.db` 的 6 站真实预测实测：服务端序 `[4,1,3,6,5,2]`，修复前本地重排成 `[1,3,4,2,6,5]`（拥堵度最低的站 4 从第 1 位掉到第 3 位，最高的站 2 从末位提到第 4 位），修复后与服务端逐位一致 | 用户指示（越权代改 L4 目录） | ⬜ 待 L4 追认 |
| 2026-09-11 | `server/main.cpp` `server/net/session.h` `server/net/dispatcher.h` `tools/pile-simulator/main.cpp` `ml/selftest.py` `docs/RUNBOOK.md` `tools/CLAUDE.md` | 答辩前收尾四项：① 启动日志 `LOG_W「其余业务 handler 尚未注册」`早已过期（40 个入向命令字全部注册完毕），改为 `LOG_I` 汇总实际注册数，为此给 `Dispatcher` 加只读的 `handlerCount()`；② `session.h` 的 `token_ttl_sec` TODO 落地——启动时 `loadTokenTtl()` 从 `t_sys_config` 读入并在起线程池前设定，读不到或非法则记 `LOG_E` 后沿用默认 7200s，不因一条配置缺失拦住启动；③ 电桩模拟器新增 `--host/--port`，此前写死 `127.0.0.1:9527`，是充电闭环进不了自动化测试的直接原因（`test-admin-integration.sh` 里 2112 只能标 `[DETECTED]`）。顺带修掉 `RUNBOOK.md` 里 `ecp-pile-sim SZ002-03 127.0.0.1 9527` 这个从未实现过的位置参数写法——那三个值会被全部当成桩号；④ `ml/selftest.py` 增加建模依赖前置检查，用系统 python 误跑时直接指向 `.venv` 并以退出码 2 退出，不再散成十几条 `No module named` | 用户指示（越权代改 L1/L5 目录） | ⬜ 待 L1/L5 追认 |
| 2026-09-11 | `03测试用例.xlsx` | **全组测试用例集成文**，关掉第 8 节归档表里「客户端异常路径清单待补」那条 ◐。按模板「一表一模块」的结构拆 9 张模块表（对齐 [说明书] 1.4 功能条目）+ 1 张缺陷清单，共 106 条用例，正常路径约 35% / 异常与边界约 65%——配比依据是 [DIVISION-OF-LABOR.md](../DIVISION-OF-LABOR.md) 对 L4 的预警「异常路径几乎必漏」。其中 94 条协议层用例在隔离实例上真跑过一遍（隔离库 + `--port` 指向的模拟器，不碰 `charging.db`），「测试结果」列填的是实测 code/msg 原文，**91/91 全通过**；12 条 GUI 用例留空并在备注标「待人工执行」。第 9 张表单独覆盖 [说明书] 2.2 通信安全与 2.3 错误处理（粘包/半包/超长长度头/鉴权四边界/8 连接并发），这部分不属于任何业务模块，没有独立表则答辩时无处可指。缺陷清单录入 BUG-001（低拥堵推荐失效，已修复并回归） | 用户指示（L4 职责，代为落地） | ⬜ 待 L4 追认 |
| 2026-09-14 | `ARCHITECTURE.md` `DEFENSE-SCRIPT.md` `L5-DEFENSE-QA.md` `docs/conventions.md` `docs/expand/CLAIM.md` `server/biz/README.md` `admin-client/CLAUDE.md` | **第一阶段收口：临时文件清理 + 文档定稿**。① 清理未跟踪的运行期产物（`__pycache__`、数据库 `-shm/-wal` 边车、`export/carbon/*` 报告导出件、`dataviz/data/*.json` 大屏快照），均在 `.gitignore` 内、可由流水线重生成，三个库清理后 `PRAGMA integrity_check` 全为 `ok`；`build/`、`ml/data/` 中间数据、`charging.db` 与落库备份保留。② 订正 **09-10 新增 3748 后全仓未跟进**的过期事实：扩展 08 由「8 个命令字 3740–3747」改为「9 个 3740–3748」、错误码 6701–6704 改 6701–6707，涉及 6 份文档；`ARCHITECTURE.md` 头部命令字总数 43 → 44。③ 第 6 节 `pool_size` 仍写「= 最大并发连接数」，与本表 09-08 网络层重构记录矛盾，改为「最大并发业务处理数」并注明出处。④ 第 9 节「L4 测试用例集」已由 09-11 的 106 条用例关闭，移入已关闭；本表 09-09 那行原排在 09-11 之后，归位到时序位置。⑤ `L5-DEFENSE-QA.md` 的文件行数表按今日实测重算（合计 8,900 → 9,500 行）。**`server/biz/README.md`（L2）与 `admin-client/CLAUDE.md`（L3）属跨目录代改**，改动仅为 `3740–3747` → `3740–3748` 的事实订正，未触碰任何设计描述 | 用户指示（代改 L2/L3 目录） | ⬜ 待 L2/L3 追认 |
| 2026-09-14 | `PHASE2-PLAN.md`（新建）`README.md` | **第二阶段立项：大数据可视化大屏与机器学习智能分析子系统**。技术栈与第一阶段基本不重叠（Python 3.11+/Hadoop 3.x/PySpark/Flask/MySQL/Node 23+/Vue3/DataV/Spark MLlib），实测本机上述组件**全部未安装**、Python 仅 3.10.12。已决五项：DataV 取开源 Vue3 组件库；机器学习改用 Spark MLlib；两阶段**并存**（Qt 侧纯 Socket 不动）；用户数据暂不扩充；Flask 独立成大数据子系统的一层、只读 MySQL 分析结果。规划 10 个分析维度 + 3 组对比分析（要求 ≥8 与 ≥2），全部绕开用户粒度——实测 8292 单仅分布在 4 个用户上。**遗留一条待办：`CLAUDE.md` 第 2 节「大屏轮询静态文件」与 `ARCHITECTURE.md` 第 7 节「大屏不接入服务端」需按第 2 节流程修订**，该项属全组冻结契约，未评审前不得先按新架构写码 | 用户指示（L5 立项） | ⬜ 待全组评审 T0 契约变更 |
| 2026-09-14 | `CLAUDE.md`(→v2.0) `AGENTS.md` `ARCHITECTURE.md` `WORKFLOW.md` `docs/expand/00-*` | **T0 契约变更：第二阶段技术基线并入冻结契约**。① 第 2 节拆为 **2.1 业务平台 / 2.2 大数据子系统**两张表，二阶段十项基线（Python 3.11+、HDFS、PySpark、MLlib、MySQL、Flask、Node 23+、Vue3、DataV、维度≥8 且对比≥2、SOP 六阶段清洗）入表，新增 `[二阶段]` 标记；② **改掉与新架构对撞的那条推论**——原文「大屏不走服务端 HTTP」现表述为「**Qt 服务端始终不提供 HTTP**」：一阶段大屏维持轮询静态快照不变，二阶段 Flask 不挂在 Qt 服务端上而是大数据子系统自己的一层、只读 MySQL，说明书 1.6 的 Socket 考核点完好；`ARCHITECTURE.md` 第 7 节同一条决策同步改写；③ 第 4 节新增 `bigdata/{spark,mllib,api,web}` 四个目录，暂归 L5，**明确不属冻结契约**，待 MySQL 结果表定稿再议；④ 第 5 节拆为 **5.1（Qt/C++ 五条，原文未动）/ 5.2（大数据五条，新增）**，新五条为：ODS 只读、金额仍用整数分、Flask 只读、删除必须留痕、分析不得直连 `charging.db`；⑤ 第 7 节编码规范标注为一阶段口径，二阶段只沿用与语言无关的三条。**节号 2/3/4/5/7 一律保持不变**——全仓有十余处按节号引用，改号会一次性打断所有链接；因改名而失效的 4 处「五条硬性规则」引用已同步为 5.1 | 用户指示（全组冻结契约） | ⬜ 待全组知晓并重开 agent 会话 |
| 2026-09-14 | `bigdata/**`（新建）`scripts/{install-phase2-env,check-env-phase2,init-mysql-phase2}.sh` `scripts/smoke-{api,screen}-phase2.py` `PHASE2-PLAN.md` | **第二阶段 T1–T7 全部完成**。环境：JDK 17 / Python 3.11.15 / PySpark 3.5.9 / MySQL 8.0.46 / Node v24.1.0 / Vue 3.5.42 / DataV 1.7.4；**版本组合由实测元数据定**——PySpark 3.5.x 的 PyPI classifiers 只声明支持到 Python 3.11，据此锁定整条栈。① **ODS 只读层**：14 表 10524 行，源库 `mode=ro` 打开、产物 `chmod 444`（把只读从自觉变成文件权限）、`_manifest.json` 记源库指纹；② **SOP 六阶段清洗**：11 条质量问题、R001–R011 规则、**全程零删除零修改值**（1 条标记待核、剔除 2 列近乎全空字段）、14/14 校验通过、可重复性实测一致；③ **Spark 分析** 13 维度 + 3 组对比（要求 ≥8 与 ≥2）→ MySQL；④ **Flask 只读 API** 4 接口，冒烟 14/14 含 SQL 注入防御；⑤ **Vue3+DataV 大屏** 5 页 17 面板 / 9 类图表 + 3 张表格，渲染冒烟走 geckodriver 逐页断言并出真实截图；⑥ **Spark MLlib** 6 模型，3/6 跑赢基线 B（如实报告，未修饰）。**贯穿校验**：营收 53,936,279 分与电量 36,638,035 在清洗、分析、落库、特征面板四个环节逐项对账一致，电量并与一阶段碳排放对拍脚本独立结果相同。**唯一未达标项**：HDFS 未部署（老师原话允许「测试在本地」），代码已做成只认路径不认介质，换 `hdfs://` 前缀即可，业务代码零改动 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `bigdata/spark/{spark_session,export_ods,profiling,cleaning,validation}.py` `scripts/{install-hadoop-phase2,hdfs-ctl,ods-to-hdfs}.sh` `scripts/check-env-phase2.sh` `config/phase2-hdfs.env.example` `PHASE2-PLAN.md` | **T8：ODS 落 HDFS，第二阶段验收六条全部达标**。Hadoop **3.3.6** 伪分布式（仅 HDFS 不起 YARN；选 3.3 线是因为 PySpark 3.5.9 自带 `hadoop-client-api` 3.3.4），装在 `~/opt` **免 root**，JDK 17 靠 `--add-opens` 放开强封装后 NameNode/DataNode 正常。ODS 15 个文件（14 表 + `_manifest.json`）落 `hdfs://localhost:9000/ecp/ods`，**逐文件 MD5 与本地快照字节级一致**，留痕 `bigdata/quality/08_hdfs_deploy.json`。以 HDFS 为源重跑清洗 1+2 / 4 / 5，**14/14 PASS**，营收仍 53,936,279 分、电量 36,638,035，与本地基线逐项一致（唯一差异是「数据滞后天数 9→10」，跨了一天，与介质无关）。**订正上一条记录里的一句错话**——原文称「换 `hdfs://` 前缀即可，业务代码零改动」，**实测不成立**，三个坑各改了一处：① `ODS_ROOT` 当初包成 `pathlib.Path`，而 pathlib **折叠连续斜杠**，`hdfs://host:9000/x` 变成 `hdfs:/host:9000/x`，Spark 报「Path does not exist」看不出根因，改为 `str` + `ods_file()`；② `export_ods.py` 全程 Python 文件 I/O，对 HDFS 不可用，真按环境变量拼只会在本地建出名为 `hdfs:/localhost:9000` 的目录，改为显式拦截并与新增的 `ods-to-hdfs.sh` 分工（前者产出本地权威快照，后者搬运校验）；③ Hadoop `FileInputFormat` **默认过滤 `_`/`.` 开头的文件**，清单恰好叫 `_manifest.json`，`hdfs dfs -cat` 读得到而 Spark 读不到，改用 `FileSystem.open` 绕开 InputFormat。另：**HDFS 上 444 挡不住超级用户**（即启动 NameNode 的账号），照搬本地 `chmod 444` 会让 CLAUDE.md 5.2 第 6 条形同虚设——ODS 属主改为独立身份 `ecp_ods`，分析侧固定以 `ecp_analyst` 连（`_pin_hdfs_identity`），上传脚本末尾**以分析身份实际试写一次并期待失败**，把只读验出来而非声称。`check-env-phase2.sh` 的 Hadoop 段由「装没装」升级为「NameNode 可达 + ODS 在位且 444」。**待办：`CLAUDE.md` 第 4 节目录归属表未收录新增的三个 `scripts/*hadoop*|hdfs*` 脚本，该文件属全组冻结契约，需走第 2 节变更流程补记** | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `bigdata/README.md`（新建）`docs/RUNBOOK.md` `ARCHITECTURE.md` `README.md` `DIVISION-OF-LABOR.md` | **第二阶段文档同步**：此前除 `PHASE2-PLAN.md` 外，全仓文档几乎没有第二阶段的痕迹。① 新建 **`bigdata/README.md`** 顶层导览——该目录原先只有 `api/`、`web/` 两份 README，进目录没有入口，`spark/`、`mllib/` 无说明；新文件讲四层数据流、八个 job 的执行顺序与产物索引。② `docs/RUNBOOK.md` 新增**第 7 节**（首次准备 / HDFS 起停 / 数据源切换 / 全链路 / API 与大屏 / 二阶段故障）——该文件定位是「全项目唯一的怎么跑起来出处」，而第二阶段命令此前只存在于 PHASE2-PLAN 第 14 节，与定位矛盾；**沿用既有 1–6 节的节号不动**（全仓有 8 处按节号引用 RUNBOOK 的 1/3/4/5 节）。③ `ARCHITECTURE.md` 新增**第 8 节**：二阶段架构图（mermaid）、五层职责边界表、七条关键设计决策、贯穿全程的营收/电量对账；抬头注明 1–7 节为一阶段；§1 L5 范围与 §4 目录明细补 `bigdata/**` 与二阶段 scripts（实测 26 文件 3,568 行 + 8 文件 904 行）。④ `README.md` 补二阶段技术栈表、目录树 `bigdata/` 展开、两个环境自检脚本的区分、快速开始的二阶段入口。⑤ `DIVISION-OF-LABOR.md` 的 L5 条目追加第二阶段职责，并注明暂由 L5 一人承担的理由。**未动 `CLAUDE.md`**（冻结契约）——其第 4 节缺三个新脚本的归属，仍待走变更流程，沿用上一条记录里挂的待办。**答辩材料（`DEFENSE-SCRIPT.md` / `L5-DEFENSE-QA.md`，共 642 行，目前完全是一阶段内容）本轮未动**，已确认第二阶段答辩为**单独一场、PPT 在第一阶段基础上修改**，另开一轮处理 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `第二阶段测试用例.xlsx`（新建）`scripts/gen-testcases-phase2.py`（新建）`scripts/smoke-screen-phase2.py` `bigdata/web/README.md` `PHASE2-PLAN.md` `ARCHITECTURE.md` `docs/RUNBOOK.md` `DEFENSE-SCRIPT.md` | **第二阶段测试用例集**：格式对齐一阶段（6 行元信息 + 表头 + 用例，冻结第 7 行，同列宽），**9 张模块表 130 条用例 + 缺陷清单 8 条**——环境基线 12 / ODS 与血缘 11 / HDFS 部署与只读 15 / 清洗探查 12 / 清洗执行校验 16 / 多维分析 11 / Flask API 20 / DataV 大屏 19 / MLlib 14。正常路径 71（54%），异常 24 / 边界 18 / 回归 8 / 人工审核 12。**表格由脚本生成不手搓**（同 quality_report.py 的路子），用例定义与实测结论都在脚本里，改一处重跑即可。实测栏填的是本轮真实输出：接口冒烟 14/14、渲染冒烟 15/15、清洗校验 14/14、HDFS 逐文件 MD5 全一致、`ecp_analyst` 写入被拒的原始报文等；人工审核项按用户约定记「与预期一致」并在备注标明，不冒充自动化结果。**本轮由测试发现并修复 BUG-P2-001**：`smoke-screen-phase2.py` 的 `面板总数 == 17` 是写死常量，而 T7 新增「负荷预测」页后实际为 18——**该断言自 T7 提交起一直失败且无人重跑**；连带发现 `bigdata/web/README.md` 的面板对照表漏了 D12 与 3 张表格、计数写成 14，`PHASE2-PLAN`／`ARCHITECTURE` 则写成 17。断言改为下限式「覆盖全部维度（≥14）」不再写死会自然增长的数，对照表补齐并统一订正为 **18 面板（15 图表 + 3 表格）**，重跑冒烟 15/15 PASS。另记录 7 条开发期缺陷（Decimal 序列化成字符串、predict_time 三 horizon 相同、GBT 爆栈、pathlib 折叠斜杠、`_manifest.json` 被 InputFormat 过滤、export_ods 对 HDFS 静默失效、ODS 推送文件数口径不一致），均已由对应用例回归覆盖。**顺带修复失效链接**：`03测试用例.xlsx` 已被改名为 `第一阶段测试用例.xlsx`，`DEFENSE-SCRIPT.md` 与 `docs/RUNBOOK.md` 的引用同步更正（conventions 历史记录保留原名不改写） | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `bigdata/quality/03_rules.md` `bigdata/spark/{cleaning,validation,quality_report}.py` `scripts/gen-testcases-phase2.py` `第二阶段测试用例.xlsx` `PHASE2-PLAN.md` `docs/RUNBOOK.md` | **清洗规则清单与代码对齐，并补齐 R002 的失败捕获缺口**。回顾 SOP 六阶段时发现规则清单成文于清洗执行之前，六处表述与实现不符：待核清单实为 `.json` 而非 `.csv`；R005 越界只计数未落文件；R010 的 `pile_type_label` 实际未派生（日志 `新增列=7` 含 R005 的 `status_label`）；R007 的「4136 行」在 `cleaning.py` 里是**硬编码字面量**不随数据集更新；R009 声称「DWD 表数与 ODS 一致」而实际 DWD 11 张（三张运维表有意不导，非删除）。前五处**只回填文档不动代码**，并在规则清单末尾新增「文档与代码一致性核对」小节逐条留痕。**第六处是真缺口**：R002 声称覆盖 `t_order`+`t_pile`+`t_station`，实现只对 `t_order` 做了影子列比对，两张维表直接 `cast`，转换失败会**静默变 NULL**——正是 SOP 6.2 明令禁止的。修复：抽出 `cast_with_capture()`（先写影子列再替换，原地 `withColumn` 会盖掉原值导致待核清单拿不到原始值），覆盖 `t_order`/`t_pile`/`t_station`/`t_pile_log` **4 表 27 列**——比原表述多一张 `t_pile_log`，因 R003 明文点名其 `create_time`；`validation.py` 每表加两条断言（清单已落地且为空、转换列非空数逐列一致），**校验项 14 → 20 全过**；报告新增逐表转换失败对账行。重跑两遍 `cleaning` 日志逐条相同（幂等），订单仍 8292 → 8292，**营收仍 53,936,279 分、电量仍 36,638,035**，无数值漂移。唯一 schema 变化 `dwd_station.status` string→bigint（R002 本就点名该列，此前漏转），已确认 `analysis.py` 与 `mllib/` 只取 `station_id`/`name`/`lng`/`lat`，下游无影响。测试集同步：P4-12「类型转换失败不得静默变空」由人工审核升级为自动断言，新增 P5-17 覆盖三表捕获，**131 条用例 / 人工审核 11 条**。**注意**：`PHASE2-PLAN` 第 13 节 HDFS 那行的「14/14」是当时实测值，已补注说明而非改写——HDFS 侧本轮未重跑 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `scripts/pack-dataset-phase2.py`（新建）`export/第二阶段数据清洗数据集.zip`（产物，gitignore） | **结项数据集打包**：按 SOP 第 8 节的归档清单，把六阶段产物打成一个包——`00` 原始层只读快照（14 表 10524 行）→ `01` 探查评估 → `02` 规则 → `03` DWD 结果 → `04` 执行日志与待核 → `05` 校验 → `06` 质量报告，另附六个可重跑脚本与 SOP 原文，71 个文件 1.02 MB。**导览 README 与打包清单由产物现读现写**（同 `quality_report.py` 的路子），手填的数字迟早和产物对不上。**实测两个坑**：① DWD 的 parquet 按规范存 UTC，用 pyarrow 直接导 CSV 会让全表时间平移 8 小时（`reserve_time` 11:21 变 03:21，而同行 `order_hour` 仍是 11，自相矛盾），故 CSV 一律由 **Spark 按会话时区导出**；② 整数列含空值（取消单时长）走 pandas 会被提升成 float 写出 `48.0`，Spark 写空值为空，符合 CLAUDE.md 5.2 第 7 条整数口径。压缩包为可重跑产物、不入库。**待办并入既有项**：`CLAUDE.md` 第 4 节目录归属表仍未收录 `scripts/` 下新增的 `*hadoop*`/`hdfs*`/`gen-testcases-phase2.py`/`pack-dataset-phase2.py`，该文件属全组冻结契约，需走第 2 节变更流程一并补记 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `bigdata/spark/dwd_schema.py`（新建）`bigdata/spark/{cleaning,validation,profiling,quality_report}.py` `bigdata/quality/{03_rules.md,09_dwd_schema.json}` `scripts/{pack-dataset-phase2,gen-testcases-phase2}.py` `第二阶段测试用例.xlsx` `PHASE2-PLAN.md` `docs/RUNBOOK.md` | **结项数据集评审反馈，采纳三条**（评审共提 7 条，本轮做①③⑥，余下②⑤⑦待办）。① **DWD Schema 契约**：新建 `dwd_schema.py` 声明 11 表 132 列的列序/类型/可空/含义，类型口径以 `db-schema.sql` 为准；`cleaning` 按它转换与排列、`validation` 按它断言、`quality_report` 按它渲染数据字典，**一份声明三处共用**，另落机器可读的 `09_dwd_schema.json`。此前 `t_carbon_daily`/`t_wallet_tx`/`t_load_forecast` 等表只做伪缺失归一就落盘，`balance`、`amount`、`*_kwh_x100`、`emission_g` 全是字符串——与 CLAUDE.md 5.2 第 7 条的整数口径相抵，`analysis.py` 满屏 `.cast("long")` 自保正是契约缺位的代价。**契约落地当场抓出一个真实矛盾**：`avatar`(5 行) 与 `output_path`(2 行) 源库是 `TEXT NOT NULL DEFAULT ''`，而 R001 按 SOP 6.1 把空串归一为 NULL，照抄源库 NOT NULL 就与自己的清洗规则打架；处置是**契约描述 DWD 事实而非源库约束**，10 个 `DEFAULT ''` 列标为可空并写明出处。③ **合理缺失登记**：`EXPECTED_MISSING` 的理由原先只 `print` 到终端、且只在缺失率 >50% 时才查，像 `t_order.start_time`(21.89%，等于取消单数) 这条最该讲清楚的永远进不了登记表；改为**全量登记实测缺失率**并写入 `01_profile.json`，报告新增 3.1 节，过期条目自动标注。⑥ **归档细节**：README 改为先于打包清单生成并加断言（此前漏登）；包内副本重写 6 处相对链接（仓库原件不动）；`dwd_schema.py` 进包。校验项 **20 → 47 全过**，两遍 `cleaning` 日志逐条相同，营收仍 53,936,279 分、电量仍 36,638,035，无数值漂移；测试集增 P5-18/P5-19 两条回归，共 133 条 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `bigdata/spark/{dwd_schema,validation,profiling,quality_report}.py` `bigdata/quality/{03_rules.md,10_dwd_profile.json}` `scripts/{gen-testcases,pack-dataset}-phase2.py` `第二阶段测试用例.xlsx` `PHASE2-PLAN.md` `docs/RUNBOOK.md` | **结项数据集评审反馈第二轮，采纳②⑤**（七条已完成①②③⑤⑥，余⑦ Excel 副本）。② **校验覆盖面**：主键/外键/取值范围/时间连续性同样写进契约而非散落在校验脚本里——`KEYS` 11 组、`FOREIGN_KEYS` 12 条、`RANGE_RULES` 45 条（照抄源库 CHECK，含 `peak+flat+valley+unalloc = total` 等式）、`TIME_SERIES` 2 组；新增关键分布前后对比（计数/合计/极值/四分位/状态构成**逐项完全相等**，零删除下标准定到最严）与 `10_dwd_profile.json` 清洗后画像；抽样由「3 条已结算订单」改为**按状态分层 + 每条规则各取样本**。**校验项 47 → 82 全过**。⑤ **去硬编码**：六维结论按新增的 `维度事实` 现拼、完全重复合计实算、跨阶段电量取自校验 `关键指标`、**第 6 节遗留问题整节改为数据驱动**（条件不成立的条目不出现、待核条目现读 pending 样例）；`validation.py` 里 `t_user 行数 == 5` 的写死判据改为「与 ODS 行数相等」。**顺带修正一处事实错误**：原报告称「8292 笔订单金额与单价×电量逐笔吻合」，实际参与该校验的是 **6477 笔已结算单**（取消单金额为 0，本不在范围内）——写死结论的典型代价：数字看着精确，含义是错的。两遍 cleaning+validation 输出逐条相同（幂等），营收仍 53,936,279 分、电量仍 36,638,035；测试集增 P5-20~P5-24 五条回归，共 138 条 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `scripts/{pack-dataset,gen-testcases}-phase2.py` `bigdata/quality/03_rules.md` `第二阶段测试用例.xlsx` | **结项数据集评审反馈第三轮，采纳⑦，七条全部闭环**。CSV 打开乱码的根因是无 BOM 的 UTF-8 被中文 Windows 的 Excel 按 GBK 解；**处置是「加」不是「换」**——两份 CSV 都不动，另出 `ODS原始数据.xlsx`(14 表) 与 `DWD清洗结果.xlsx`(11 表)。不给 CSV 加 BOM 的理由：`00_原始层_ODS/` 那份是 Spark 的真实输入，BOM 会混进首列列名（`order_id` → `\ufefforder_id`）把流水线搞坏；DWD 那份要与重跑结果逐字节对拍。（`02_issues.csv` 本来就是 utf-8-sig，这个约定项目里早有，只是推广不到数据 CSV 上。）**两册取值口径有意不同**：ODS 全按文本，忠实于「原始层所有值都是字符串」；DWD 按 Schema 契约给类型，金额电量为数值可直接求和，而手机号/订单号仍按文本（SOP 6.2：避免前导零或大数精度丢失）。每册首两张表为「说明」（金额分/kwh_x100/时间格式/空值含义）与「表清单」，其余按业务顺序排。**过程中修掉两个自造 bug**：`move_sheet` 收的是相对偏移不是目标下标，导航表没排到最前；工作表原按文件名字母序，`t_admin` 排在 `t_order` 之前。包体积 1.05 → 2.46 MB / 84 个文件；测试集增 P5-25，共 139 条 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-15 | `README.md` `ARCHITECTURE.md` `PHASE2-PLAN.md` `L5-PLAN.md` `bigdata/README.md` `docs/{RUNBOOK,conventions}.md` | **第二阶段清洗文档同步**。三轮补强后各文档的数字与产物清单已脱节，逐处对齐：`bigdata/README.md` 的 `spark/` 规模 8 文件 1,594 行 → **9 文件 2,367 行**、新增 `dwd_schema.py` 行与 `09_dwd_schema.json`/`10_dwd_profile.json` 产物索引、待核清单标注 **14 份**；`ARCHITECTURE.md` 的 `bigdata/` 26 文件 3,568 行 → **28 文件 4,583 行**、二阶段脚本 8 个 904 行 → **9 个 1,857 行**、DWD 分层说明补 Schema 契约；`PHASE2-PLAN.md` 第 6 节删掉过时标题「当前完成度为零」、六阶段表补附加产出行与三轮补强要点、清洗脚本清单补 `dwd_schema.py`；`L5-PLAN.md` 校验 14/14 → **82/82**；`RUNBOOK.md` 补结项数据集打包命令；根 `README.md` 目录树补 `export/`。**顺带修一处失效链接**：第 8 节归档索引仍指向已改名的 `03测试用例.xlsx`（现 `第一阶段测试用例.xlsx`），同时补入第二阶段测试集。**核对方式**：从 `05_validation.json`/`04_clean_log.json`/`dwd_schema.py` 取实测值再回查文档，校验 82、契约 11 表 132 列、外键 12、范围规则 45、待核 14 份、用例 139 条逐项对得上。**已知缺口（未处理）**：`DEFENSE-SCRIPT.md` 与 `L5-DEFENSE-QA.md` 至今**没有任何第二阶段内容**，答辩前需补 | 用户指示（L5 承担） | ⬜ 待全组知晓 |
| 2026-09-16 | `scripts/gen-testcases-phase2.py` `第二阶段测试用例.xlsx` `PHASE2-PLAN.md` `DEFENSE-SCRIPT-PHASE2.md` `docs/{RUNBOOK,conventions}.md` | **第二阶段测试用例集补齐 T9 与归档环节，并复跑接口/渲染两表**。新增两张模块表：**10 智能运营建议 13 条**（`advice.py` 的 D16/D17：只用已有产物派生不重训模型、拥堵度口径与 `predict.py` 同源、分流阈值 0.8 与管理端 `forecast_page.cpp:29`／用户端 `main_window.cpp:52` 三处一致、无站点达阈值时退到「引导」级而非给空表、可信度取自 `eval.json` 的 `gain_vs_b_pct` 计数分档、连跑两次仅 `generated_at` 不同）；**11 结项数据集归档包 15 条**（一条命令导出、六阶段目录对齐 SOP 第 8 节、CSV 时间不平移 8 小时——8292 行逐行比对 `reserve_time` 小时与 `order_hour` 不符 0 行、整数列含空值不被提升为浮点、包内 444 恢复 644、`.crc` 剔除 `_SUCCESS` 保留、相对链接 7 条零断链、84 条指纹逐条复算一致、`unzip -t` 无错）。**复跑 07/08 两表**：接口冒烟 14/14、渲染冒烟 16/16 全 PASS；D16/D17 并入后维度数 13 → **15**、MySQL 结果表 18 → **20**、API 维度目录 18 → **20 份**（未改一行 API 代码）、大屏 5 页 18 面板 → **6 页 21 面板（16 图表 + 5 表格）**、图表形态 9 类 → **10 类**（第 6 页热力图）。**P8-08 的下限式断言经受住了第二次扩页**——BUG-P2-001 当初若仍写死常量，这轮会再次静默失效。表格机制同步补强：新增 `DATE2`/`p2`/`m2`/`ng2`，**只有当轮真跑过的用例才换日期**（HDFS 两表本轮未重跑，日期保持 09-15）；用例改为按编号出表，源码追加即可，不必手工插到中间（此前 P5-25 夹在 P5-15 后面）。**本轮由测试发现 BUG-P2-009（待修复）**：结项数据集把仓库占位文件 `00_原始层_ODS/.gitkeep` 一并打进包——正是 BUG-P2-002 里让本地与 HDFS 文件数对不上的那个文件；P11-15 如实记为**不通过**，待 `copy()` 加忽略规则后重新打包回归。**打包脚本本轮实测可直接导出**：`export/第二阶段数据清洗数据集.zip` 2.46 MB / 85 个文件，两次打包 81/84 文件逐字节一致，差异仅 `README.md`（含打包时间）与两册 xlsx（拆包后仅 `docProps/core.xml` 不同，即 Excel 内置创建时间）。用例合计 **139 → 169 条，缺陷 8 → 9 条** | 用户指示（L5 承担） | ⬜ 待全组知晓 |

### 3.2 待评审变更申请（CR）

> 一条 CR 走完第 2 节五步后，从本表移入第 3.1 节。**属主未改之前，任何人不得先按新协议写代码。**

| 编号 | 契约 | 摘要 | 提出人 | 属主 | 状态 |
| --- | --- | --- | --- | --- | --- |
| CR-001 | `docs/protocol.md` | 增加负荷预测结果通道（1101 扩字段 + 新增 2305） | L5 | **L1** | ✅ **已应用**（L1 暂时无法操作，2026-09-03 由 L5 代为改动，待归队复核） |
| CR-002 | `ml/` 规范 + `t_order` 等业务表 | 授权 `gen_history.py` 离线写入业务表播种历史数据 | L5 | **L2** | ✅ **已批复**（2026-09-04，三条意见已全部落地到脚本） |
| CR-003 | `docs/protocol.md` 第 6 节 + `server/net/session.h` | 冻结用户后会话立即失效（`invalidateSessions`） | L2 | **L1** | ✅ **已应用**（L1/L2 均暂时无法操作，2026-09-03 由 L5 代为改动并编译通过，待二人归队复核） |

---

#### CR-001 · 协议增加负荷预测结果通道

**提出人** L5(SCML)　**属主** L1　**日期** 2026-09-02　**影响** L1 定稿 · L2 取数实现 · L3 负荷预警界面 · L4 推荐排序 · L5 数据回写

**为什么要改**

`[说明书]` 1.4 机器学习子系统有两句明文要求：

> 「系统根据预测结果，在**用户端优先推荐低拥堵、高空闲率的充电站**，减少用户排队等待时间；同时为**运营端提供负荷预警**，辅助工作人员提前做好电力调配与运维值守安排。」

而 `docs/protocol.md` v1.0 第 4 节全表**没有任何字段或命令字承载预测结果**：1101 响应的 list 元素只有 `stationId / name / address / price / pileTotal / pileIdle / distance` 七个字段；管理端 2300 段只有 2301–2304 四个历史统计命令字。

**库里有、协议里没有** —— `docs/db-schema.sql` 第 10 张表 `t_load_forecast` 已冻结，`load_kw / idle_pile / is_peak / congestion` 字段齐备。预测算出来也送不到两端，说明书明文功能无法验收。

**建议改动**（三处，最终由 L1 定夺）

*改动 1 — 4.2 节 1101 响应 list 元素增加两个字段*（向后兼容，客户端可忽略）

| 字段 | 类型 | 来源 | 说明 |
| --- | --- | --- | --- |
| `congestion` | double | `t_load_forecast.congestion` | 0..1 拥堵度，取 `horizon=1` 的最新一条；**无预测数据填 `-1`** |
| `idleForecast` | int | `t_load_forecast.idle_pile` | 未来 1h 预测空闲桩数；**无预测数据填 `-1`** |

> **为什么缺省值是 -1 而不是 0**：在拥堵度语义里 `0` 表示「完全不拥堵」，是最优站点，会被排到推荐列表最前面。「没有预测数据」被当成「最好的站」是方向性错误，必须能与真实的 0 区分开。

*改动 2 — 4.2 节 1101 请求增加可选字段 `sortBy`*

| 值 | 含义 |
| --- | --- |
| `0`（缺省即 0） | 按距离升序 —— `[说明书]` 1.4「按距离由近及远展示充电站列表」 |
| `1` | 按拥堵度升序，同拥堵度再按距离 —— `[说明书]` 1.4「优先推荐低拥堵、高空闲率的充电站」 |

> 说明书这两句都是明文且并存，**不能二选一改默认值**，只能加开关。默认保持距离排序，L4 现有实现不受影响。

*改动 3 — 4.4 节管理端 2300 段新增命令字 2305*

| 命令字 | 名称 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- |
| 2305 | 站点负荷预测 / 负荷预警 `[说明书]` 1.4 | `{stationId, horizon}` | `{list:[{stationId, stationName, horizon, predictTime, loadKw, idlePile, isPeak, congestion, modelVersion}]}` |

- `stationId = 0` 表示全部站点；`horizon` 取 1 / 6 / 24，非法值返回 `ERR_PARAM`
- `loadKw` 为 double，单位 kW。**这是物理量不是金额**，不受「金额一律整数分」规则约束
- 数据来源：`t_load_forecast` 中最新一批（同一 `model_version` 下 `create_time` 最大的那批）
- **预警阈值不进协议**：`congestion ≥ 0.8` 判为预警，由管理端本地判定。阈值将来要调不必动协议

**错误码：不需要新增。** 无预测数据时返回 `code = 0` + 空 `list`，管理端显示「暂无预测数据」。预测缺失是正常状态而非错误 —— 这条明确写出来，避免 L2 自行造一个 `ERR_NO_FORECAST`，那样会同时污染 `docs/protocol.md` 第 5 节和 `common/error_code.h` 两处冻结契约。

**明确不做**（防止范围膨胀）

- 不为用户端单独增加预测查询命令字，扩 1101 字段即可，少一个命令字少一处联调
- 不做预测结果服务端推送，大屏走 JSON 快照、两端按需拉取
- **不涉及 `docs/db-schema.sql`**，`t_load_forecast` 表结构不动

**L5 侧承诺**（协议批准后）

保证 `t_load_forecast` 中每个 `station_id × horizon` 至少有一条当前有效预测；`model_version` + `create_time` 可供 L2 稳定取到「最新一批」。

> ⚠ **越权记录**：本规则第 4 节「非属主只读……不要自己动手改了再说」在此处被打破。L1 2026-09-03 暂时无法操作，经用户明确指示由 L5 代为直接改动 `docs/protocol.md` 并已应用（v1.1），**非正常变更流程**（跳过了「属主亲自落笔」一步）。以下是实际已落地的内容，L1 归队后请逐条复核，如有异议可随时改回或调整——这不是最终定论，是应急处理。

已应用的补丁：

*补丁 1 — 替换 4.2 节整段*

```markdown
### 4.2 用户端 · 充电站与电桩（1100–1199）

| 命令字 | 名称 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- |
| 1101 | 附近充电站列表 `[说明书]` 1.4 | `{lng, lat, keyword, sortBy}` | `{list:[{stationId, name, address, price, pileTotal, pileIdle, distance, congestion, idleForecast}]}` |
| 1102 | 充电站内电桩详情 `[说明书]` 1.4 | `{stationId}` | `{list:[{pileId, code, type, status, power}]}` |

`price` 单位为**分/度**；`distance` 单位为**米**（显示层换算为公里）。

`sortBy`（`[本组自定]` CR-001，可选，缺省 `0`）：
- `0` 按距离升序 `[说明书]` 1.4「按距离由近及远展示充电站列表」
- `1` 按拥堵度升序，同拥堵度再按距离 `[说明书]` 1.4「优先推荐低拥堵、高空闲率的充电站」

`congestion`（double，0..1，`[本组自定]` CR-001）/ `idleForecast`（int，`[本组自定]` CR-001）：取 `t_load_forecast` 中 `horizon=1` 的最新一条。**无预测数据时两字段均填 `-1`**——`0` 在拥堵度语义中代表「最不拥堵」，不能兼职表示「没有数据」，否则新站或模型未跑到的站会被排到推荐最前面。
```

*补丁 2 — 4.4 节 2304 行之后新增一行 + 表后加一段说明*

```markdown
| 2305 | 站点负荷预测 / 负荷预警 `[说明书]` 1.4（CR-001） | `{stationId, horizon}` | `{list:[{stationId, stationName, horizon, predictTime, loadKw, idlePile, isPeak, congestion, modelVersion}]}` |
```
```markdown
`2305`：`stationId=0` 表示全部站点；`horizon` 取 `1`/`6`/`24`，非法值返回 `ERR_PARAM`。取 `t_load_forecast` 中同一 `model_version` 下 `create_time` 最大的一批。**无预测数据返回 `code=0` + 空 `list`**，不新增错误码——预测缺失是正常状态，不是错误，新增错误码要同时改协议第 5 节和 `common/error_code.h` 两处冻结契约，不值得。管理端按 `congestion ≥ 0.8` 本地判定预警，阈值不进协议，将来要调不必再走变更流程。`loadKw` 单位 kW，是物理量不是金额，不受「金额整数分」规则约束。
```

*补丁 3 — 第 8 节变更记录加一行*

```markdown
| 2026-09-03 | v1.1 | CR-001：1101 增加 congestion/idleForecast/sortBy；新增 2305 站点负荷预测 | L5 | 待评审 |
```

---

#### CR-002 · 授权 `gen_history.py` 离线写入业务表

**提出人** L5　**属主** L2（`t_order` / `t_pile_log` / `t_pile` 属主）；规范条文由 SCML 落地　**日期** 2026-09-02
**影响** L2 授权 · L5 实现 · L3/L4 演示数据 · 全组答辩

**为什么要改**

`ml/CLAUDE.md` 现规定 ml/ **只读业务表**，但历史数据生成器必须向 `t_order` 写入模拟历史。实测当前 `charging.db`：

```
t_station 6　t_pile 24　t_user 4
t_order 0　　t_pile_log 0　　t_wallet_tx 0　　t_load_forecast 0
```

后果不止 L5 一条线：大屏「营收趋势」「站点排行」两张图全 0；时序模型没有任何训练数据；管理端 `[说明书]` 1.4 要求的「近 7 / 30 日营收趋势」演示时也是一条零线。

**冲突点在哪**：「只读业务表」这条规则本身是对的 —— 防的是运行期 Python 脚本与服务端争抢、污染业务数据。**不能简单删掉**。

**建议方案**：把「运行期只读」和「离线播种可写」拆成两条**并列**规则，而不是给只读规则开一个口子。

| 项 | 约定 |
| --- | --- |
| 定位 | **离线种子脚本**，与服务端进程无关，不随服务端启动，不进任何自动流程 |
| 可写表 | 仅 `t_order`（INSERT）、`t_pile_log`（INSERT）、`t_pile` 的 `charge_count` / `charge_duration`（UPDATE，按生成订单聚合回填） |
| 禁止 | 不碰 `t_user` / `t_station` / `t_admin` / `t_wallet_tx` / `t_sys_config`；不改任何表结构 |
| 默认行为 | **默认 dry-run**，只打印将生成多少条；必须显式 `--commit` 才落库 |
| 重跑 | 提供 `--reset` 先清空 `t_order` / `t_pile_log` 再生成，仅限开发库 |
| 红线自检 | 执行前检查：若 `t_order` 中存在 `settle_time` 晚于本次生成区间上界的记录，判定为「库里已有真实联调数据」，**拒绝执行**并提示 |
| 运行期 | ml/ 其余脚本（`export_snapshot.py`、预测脚本）维持 `mode=ro` 只读；预测结果只写 `t_load_forecast` |

> **为什么 `t_pile` 的两个累计字段也要写**：不回填的话，库里有几千条历史订单，而管理端 2111 电桩列表里每个桩「累计充电次数 = 0」「累计充电时长 = 0」，`[说明书]` 1.4 明文要求的这两列一眼就是假数据，答辩必被追问。因此列入可写范围，但**严格限于这两个聚合字段**。

**请 L2 确认三点** → **L2 已于 2026-09-04 逐条批复，结论如下**

| # | 问题 | L2 结论 | L5 落地情况 |
| --- | --- | --- | --- |
| 1 | 授权范围 | **同意**限定为 `t_order` INSERT、`t_pile_log` INSERT、`t_pile.charge_count/charge_duration` UPDATE；不增加其他表写权限，不改表结构。**金额须与 1204 后续的整数分结算规则一致** | ✅ 已改为 `(price*kwh_x100+50)//100` 纯整数运算。原先的 `round(x/100)` 有两个坑：Python 的 round 是四舍六入五成双，且中间转浮点——与服务端整数运算对账会差分 |
| 2 | `--reset` | **接受保留**，但只能在可丢弃的演示副本上执行，不得对共享开发库/正式库/含成员测试数据的 `charging.db` 执行；执行人限 L5/SCML，执行前停服务、备份、先 dry-run | ✅ 脚本硬性拒绝对 `charging.db` 执行 `--reset`（退出码 4）。**并且加了第二道**：即便在副本上执行，也只删播种记录里的批次，成员留在历史区间内的测试数据不会被误伤（已实测验证） |
| 3 | `data_seed_version` | **暂不加**。单一版本号既分不清哪些订单是脚本生成的，也保证不了 reset 安全。建议先由 ML 侧维护播种记录文件，日后需要库级批次追踪再另提 CR | ✅ 改为 `ml/data/seed_manifest.json`，记录每批次的 `order_no` 全量列表与 `pile_log` 的 id 闭区间，`--reset` 据此精确删除 |

> **实施中发现并修掉的一个 bug**：`pile_log` 最初按 `log_id >= 起点` 删除，会把本批次之后别人插入的日志一并删掉。已改为记录 id 闭区间、用 `BETWEEN` 删除。这个坑正是 L2 第 3 条担心的「保证不了 reset 安全」，验证时才暴露出来。

---

#### CR-003 · 冻结用户后会话立即失效

**提出人** L2　**属主** L1（`docs/protocol.md` 第 6 节 + `server/net/session.h`）　**日期** 2026-09-03
**影响** L1 实现接口 · L2 调用 · **L4 客户端要处理 `ERR_TOKEN_INVALID`** · L3 无感

**问题**

2202 冻结/解冻已实现（`server/biz/user_management_service.cpp`），冻结只 `UPDATE t_user.status`。1001 登录会拦截冻结账号（`user_service.cpp:82`），但**冻结前已签发的 token 不受影响**，会话表在 `server/net/session.h`，L2 无权改，也不应该改。

**⚠ 风险比提出时描述的更严重。** L2 原话是「最长可能继续有效 2 小时」，这个说法不成立：

`SessionTable::validate()` 每次校验成功都会 `it->lastActive = now`（`session.cpp`），`docs/protocol.md` 第 6 节也明文「**每次成功请求刷新最后活跃时间**」。所以 7200 秒是**空闲超时**，不是签发后的绝对有效期。

> **结论：一个被冻结但仍在操作客户端的用户，token 永不过期，风控措施等于没生效。** 上限不是 2 小时，是无限。

实测当前已注册的 20 个命令字中，冻结用户凭旧 token 仍可调用的包括 **1005 钱包充值**（`wallet_service.cpp` 全程未查 `t_user.status`）、1003 改昵称、1004 改头像。1202 预约有复查（`reservation_service.cpp:78`），是唯一挡住的。`[说明书]` 1.4 明文「管理员可手动冻结用户账号（用于风控场景）」——现状不满足。

**建议实现**（L1 定夺）

L2 建议的 `void invalidateUserSessions(int userId)` 方向正确，建议签名调整为：

```cpp
// 踢掉指定身份的全部会话，返回被清除的会话数（供日志与审计）。
// 必须按 role 过滤：user_id 与 admin_id 是两套独立自增序列，同一个 id 值
// 在两种角色下都存在，不过滤会误踢管理员。
int invalidateSessions(int id, Role role);
```

两点理由：

1. **返回值不要 void** —— L2 提交事务后要 `LOG_I` 记一句「踢掉 N 个会话」，否则联调时无法判断到底生效没有
2. **带 `Role` 参数而非写死 ROLE_USER** —— `docs/db-schema.sql` 的 `t_admin.status` 已有 `1=停用`，管理员停用迟早要同样的能力，一次做好省得再改一遍接口

实现上是一次写锁下的全表扫描（`m_map` 量级为在线连接数，`pool_size = 8`），成本可忽略。

**协议第 6 节需同步加一条**（这才是本条必须走变更流程的原因）：

> - 管理员冻结用户（2202）后，该用户**全部在线会话立即失效**，后续请求返回 `ERR_TOKEN_INVALID`

**L4 必须知道的连带影响**

会话被踢后，客户端下一个请求收到的是 `ERR_TOKEN_INVALID` →「登录已过期，请重新登录」，**不是**「该账号已被冻结」。用户重新用手机号登录时 1001 才返回 `ERR_USER_FROZEN` 显示真实原因。

所以 **L4 收到 `ERR_TOKEN_INVALID` 必须清空本地 token 并跳转登录页**，不能静默重试——否则会拿着已失效的 token 死循环。这条不写清楚，联调周必踩。

**错误码：不新增。** 复用 `ERR_TOKEN_INVALID`，理由同 CR-001：新增错误码要同步改 `docs/protocol.md` 第 5 节和 `common/error_code.h` 两处冻结契约，为一个能靠两步流程表达清楚的语义不值得。

**附带建议（属 L2，不入本 CR）**：1005 充值补一次 `t_user.status` 复查。本 CR 落地后冻结用户已拿不到有效 token，该检查属纵深防御，但资金接口值得多一道，且能覆盖「冻结事务提交」与「踢会话」之间的瞬时窗口。

> ⚠ **越权记录**：同上，本条同样打破「非属主只读」规则。L1、L2 2026-09-03 均暂时无法操作，经用户明确指示由 L5 代为直接改动 `server/net/session.h` / `session.cpp`（L1 的文件）与 `server/biz/user_management_service.cpp`（L2 的文件）并已应用，增量编译通过（0 error 0 warning），并已用真实 `charging.db` + 实际 TCP 协议跑通完整链路（13800138002 登录取 token → 2202 冻结 → 旧 token 查 1002 返回 `ERR_TOKEN_INVALID`(1003) → 重新登录返回 `ERR_USER_FROZEN`(2003) → 解冻恢复数据库原状），7 项断言全部通过。两位属主归队后仍请复核代码本身，测试通过不代表设计取舍没有异议空间。

已应用的补丁：

*补丁 1（L1）— `server/net/session.h`，`remove()` 声明之后新增*

```cpp
    // 按角色踢除指定身份的全部会话；返回被清除的数量。
    // 必须按 role 过滤——user_id 与 admin_id 是两套独立自增序列，
    // 同一个 id 值在两种角色下都存在，不过滤会误踢管理员会话。
    int invalidateSessions(int id, Role role);
```

*补丁 2（L1）— `server/net/session.cpp`，`remove()` 实现之后新增*

```cpp
int SessionTable::invalidateSessions(int id, Role role)
{
    int n = 0;
    pthread_rwlock_wrlock(&m_lock);
    for (auto it = m_map.begin(); it != m_map.end(); ) {
        if (it->id == id && it->role == role) { it = m_map.erase(it); ++n; }
        else ++it;
    }
    pthread_rwlock_unlock(&m_lock);
    return n;
}
```

*补丁 3（L1）— `docs/protocol.md` 第 6 节末尾追加一条*

```markdown
- 管理员冻结用户（2202）后，该用户**全部在线会话立即失效**，后续请求返回 `ERR_TOKEN_INVALID`（客户端应清空本地 token 并跳转登录页，而非静默重试）
```

*补丁 4（L2）— `server/biz/user_management_service.cpp`，`handleUserStatus` 内 `db.commit()` 成功之后、`return ERR_OK` 之前插入*（`net/session.h` 的 include 方式与文件里已有的 `net/dispatcher.h` 一致，不必新加 include 路径写法）：

```cpp
    if (status == USER_FROZEN) {
        const int kicked = SessionTable::instance().invalidateSessions(static_cast<int>(userId), ROLE_USER);
        LOG_I(QStringLiteral("冻结用户已踢下线: userId=%1 会话数=%2").arg(userId).arg(kicked));
    }
```

需要在文件顶部 `#include "net/dispatcher.h"` 之后加一行 `#include "net/session.h"`。**L1 的补丁 1/2 落地之后 L2 补丁 4 才能编译通过**，两人需要协调落地顺序（L1 先合，L2 再合，或 L2 先在本地叠加 L1 的分支自测）。

---

## 4. 评审记录

| 日期 | 对象 | 结论 |
| --- | --- | --- |
| 2026-09-02 | 四项冻结契约 v1.0 | **通过**。会上定案：Qt 统一 6.2.4；运行参数维持原设定（线程池 8 / token 7200s / 端口 9527 / 充值上限 100000 分，均在 `t_sys_config` 表，改表不改代码）；地图 Key 每人各申请，见第 6 节 |

## 5. 环境基线

`[说明书]` 1.5：Ubuntu 22.04+，Qt Creator 6.2+。

| 项 | 版本 |
| --- | --- |
| 操作系统 | Ubuntu 22.04 LTS（VMware 17）`[说明书]` |
| Qt | **6.2.4**，全组一致 `[本组自定]` |
| 编译器 | g++，C++17 |
| 数据库 | QSQLite（Qt 自带驱动，无需额外装）`[说明书]` |
| Python | 3.10（L5） |

### 自检

**全员 W1 第一天跑一次，把「结论」一行贴群里。**

```bash
bash scripts/check-env.sh        # 全部
bash scripts/check-env.sh L3     # 只查自己这条线
```

脚本检查基础工具、Qt 版本、模块头文件、QSQLite 驱动，并对 QtCharts / QtWebEngineWidgets **实际试编译**；未通过时直接打印该装哪些包。退出码等于未通过项数。

> **为什么试编译而不只看包名**：这两个模块各有「运行库」与「开发包」两个 deb。`libqt6charts6` 是运行库，`libqt6charts6-dev` 才带头文件。若因其他软件依赖装上了运行库，`dpkg -l | grep charts` 能看到东西，容易误判已装好，但编译仍报找不到头文件。**试编译是唯一可靠判定。**

L3 需要 QtCharts（`[说明书]` 1.4 营收趋势用 QChart），L4 需要 QtWebEngineWidgets（`[说明书]` 1.4 一键导航用 QWebEngineView），两者都不在 `qt6-base-dev` 里。这是 agent 完全帮不上的环境问题，拖到联调周会连累全组。

### 运行与排障

构建、启动、预测流水线、测试入口与常见故障（含 SSH 会话里 GUI 程序报
`could not connect to display` 的三种解法）统一见 [RUNBOOK.md](RUNBOOK.md)，本文不重复。

## 6. 配置与密钥

`[说明书]` 2.2：合理考虑数据安全问题。

**凭据不进仓库。** `config/` 默认整个被 `.gitignore` 挡下，只放行 `*.example` 模板——这样以后往里放任何凭据文件（`map_key.txt`、`secrets.json` 等）都不会误提交。仓库只留模板：

```bash
cp config/app.ini.example config/app.ini   # 首次克隆后执行，填入自己的值
```

模板改了要提交，实际配置永不提交。已验证 `git add config/` 只会加入 `.example`。

`app.ini` 可配置项：`server/host`、`server/port`、`server/pool_size`、`map/key`、`dataviz/*`。

> `pool_size` 是**最大并发业务处理数**，不是最大并发连接数——2026-09-08 网络层改为 epoll reactor 后，
> 连接由 IO 线程统一管理，线程池只处理已收完整的报文（见第 3.1 节该日记录）。

### 腾讯地图 Key

`[说明书]` 1.4 两处功能依赖：地址转经纬度、路线规划。Key 是腾讯位置服务发给开发者账号的调用凭据，绑定微信/QQ，**按 Key 计算免费额度**（地理编码约 1 万次/日）。

**每人各申请一个**，三个理由：

1. **额度共享**——五人共用一个，联调时容易同一天跑满；如果那天正好答辩，导航当场演示不了
2. **进了 git 撤不干净**——后来删掉仍留在提交历史里，仓库交老师、推 GitHub、答辩投屏 `git log` 都会暴露，别人能消耗你的额度，腾讯检测到异常还可能封禁
3. **是「数据安全」的具体落点**——答辩被问时，除密码 SHA-256、SQL 参数绑定外的第三条

**申请**：<https://lbs.qq.com/> 注册实名 → 应用管理创建应用 → 添加 Key，勾选 **WebServiceAPI** 与 **JavaScript API** → IP 白名单本机调试可留空，**答辩机出口 IP 要提前加** → 填入 `config/app.ini` 的 `[map] key=`。

代码从 `QSettings` 读，禁止硬编码进 `.cpp` / `.h` / `.pro` / 提交信息，也不要编造假 Key 占位。

## 7. 分支与提交

- `main` 为集成分支；各线在 `feat/L1-xxx` 分支开发，自测通过后合入
- 合入前确认只改了自己归属目录下的文件
- 小步提交，禁止一次上千行。信息写人话：

  ```
  ✅ feat(L2): 订单结算扣减钱包余额，余额不足返回 ERR_BALANCE_NOT_ENOUGH
  ❌ 完成管理端 / update
  ```
- 禁止提交 `config/app.ini`、`*.db`、构建产物、`.pro.user`（均已在 `.gitignore`）
- 推送由本人确认后手动执行，不要让 agent 跑 `git push`

## 8. 过程成果物归档

| 阶段 | 成果物 | 责任人 | 状态 |
| --- | --- | --- | --- |
| 需求 | 需求理解与工作线拆解 | 全组 | ✅ [DIVISION-OF-LABOR.md](../DIVISION-OF-LABOR.md) |
| 设计 | 通信协议 | L1 | ✅ [protocol.md](protocol.md) |
| 设计 | 数据库设计 | L2 | ✅ [db-schema.sql](db-schema.sql) |
| 设计 | 技术选型说明（含推论项理由） | 全组 | ✅ CLAUDE.md 第 2 节 |
| 设计 | 系统架构与责任归属 | 全组 | ✅ [ARCHITECTURE.md](../ARCHITECTURE.md) |
| 开发 | 各模块源码与模块级 CLAUDE.md | 各线 | ✅ 核心 35 个命令字全部落地 |
| 开发 | 负荷预测建模与精度评估 | L5 | ✅ [../ml/reports/forecast_eval.md](../ml/reports/forecast_eval.md)，结论见 8.1 |
| 开发 | 扩展模块 08 碳减排与能源报告 | L5 | ✅ [expand/08-运行手册.md](expand/08-运行手册.md) |
| 测试 | 自动化测试与冒烟脚本 | L3 / L5 | ✅ 清单见 [RUNBOOK.md 第 5 节](RUNBOOK.md) |
| 测试 | 人工测试流程与回归清单 | L4 / L5 | ✅ 数据端 [../ml/TESTING.md](../ml/TESTING.md)；一阶段客户端与全链路 [../第一阶段测试用例.xlsx](../第一阶段测试用例.xlsx)（9 张模块表 106 条用例 + 缺陷清单，94 条已实测，12 条 GUI 待人工执行）；二阶段 [../第二阶段测试用例.xlsx](../第二阶段测试用例.xlsx)（11 张模块表 169 条用例 + 缺陷 9 条，人工审核 11 条，未通过 1 条 P11-15/BUG-P2-009，由 `scripts/gen-testcases-phase2.py` 生成）|
| 测试 | 交叉试读评审记录 | L2 | ⬜ 每周一次，记入第 4 节 |
| 发布 | 部署与运行说明 | L3 | ✅ [RUNBOOK.md](RUNBOOK.md) |
| 发布 | 答辩演示动线（大屏部分） | L5 | ✅ [../dataviz/DEMO.md](../dataviz/DEMO.md) |
| 发布 | 答辩 PPT 大纲与讲稿 | 全组 | ✅ [../DEFENSE-SCRIPT.md](../DEFENSE-SCRIPT.md) |
| **二阶段** | 大数据子系统任务清单与收尾 | L5 | ✅ [../PHASE2-PLAN.md](../PHASE2-PLAN.md) |
| **二阶段** | 数据质量报告（SOP 六阶段） | L5 | ✅ [../bigdata/quality/06_quality_report.md](../bigdata/quality/06_quality_report.md) |
| **二阶段** | 负荷预测评估报告（MLlib） | L5 | ✅ [../bigdata/quality/07_forecast_eval.md](../bigdata/quality/07_forecast_eval.md) |

### 8.1 负荷预测精度评估结论　`[说明书]` 1.4

完整报告 [../ml/reports/forecast_eval.md](../ml/reports/forecast_eval.md)（随每次训练自动重生成，
下表为定稿时的数值，量级稳定，小数点后会随重训微动）。

**做法**：`HistGradientBoostingRegressor`，全局单模型 + station 特征（6 站各自建模样本太少），
2 个目标 × 3 个 horizon = 6 个模型，直接多步不用递归（避免误差累积）。
**按时间切分**，末 12 天作测试段；超参在训练段末尾再切 8 天验证段上选，**绝不用测试段调参**。

**结果**（相对基线「分工作日/周末的同小时均值」的 MAE 降幅）：

| 目标 | 1h | 6h | 24h |
| --- | --- | --- | --- |
| 充电负荷 kW | +9.2% | +5.3% | +2.5% |
| 并发会话数 | +31.1% | +0.1% | −1.0% |

**三条必须一并说明的限制，答辩时主动讲，别等被问：**

1. **基线是模型的一个输入。** 特征里含扩展窗口口径的同小时均值（只用严格早于当前样本的
   同键观测，构造上无穿越）。所以上表不应读作「模型比基线聪明多少」，而是
   **「在季节均值之上还能再榨出多少」**。这样安排是有依据的：不给这个特征时，
   h=6 的模型测试 MAE 反而输给基线（30.05 vs 29.77）——滞后特征在该 horizon 基本是噪声，
   模型会去学噪声。
2. **h≥6 的增量小是数据决定的，不是模型缺陷。** 训练数据的跨日持续性来自 AR(1) 过程，
   去季节后的 t−24h 自相关只有 0.078，理论上可榨取的增量本就接近零。
   佐证：只用日历特征（完全不给滞后）训练，h=24 的测试 MAE 与基线几乎相同。
   继续调参把并发数 h=24 做成正数，只会是在测试集上过拟合。
3. **绝对精度不宜外推到真实部署。** 训练数据由 `ml/gen_history.py` 合成；
   天气虽是可预报的外生变量（真实场景也会用天气预报），但合成数据上这个特征比现实更干净。

**数据侧的前置工作**（同样是评估结论的一部分）：原始生成器六个站共用一条小时曲线、
日与日相互独立，实测 t−24h 自相关仅 0.052，「各站点预测」在数据层面是空的。
经站点画像分化 + AR(1) 需求水平 + 天气马尔可夫持续性 + 电桩占用约束改造后，
t−24h 自相关 0.345、分站曲线两两 L1 距离 0.475（噪声底实测 0.149）、
站点峰值负荷 ≤ 装机 85%。验收由 `ml/check_signal.py` 把关，四项全过才允许进入建模。

## 9. 待办

**已关闭**：四项契约评审冻结 · Qt 版本写死 6.2.4 · 运行参数定案 · 地图 Key 方案 ·
模块级 `CLAUDE.md`/`AGENTS.md` · 工程骨架与一键构建 · CR-001/002/003 三条全部落地 ·
R0 核心闭环（1203–1205 / 9001–9006 / 2112 / 2305 / 1208）· 底座 B2/B3/B4 · 扩展模块 08 ·
**测试用例集与异常路径回归清单**（2026-09-11，106 条，第 8 节归档表最后一个 ◐ 已消除）。

**尚未关闭**：

- [ ] **L1、L2 逐行复核越权改动**：`docs/protocol.md`(v1.1)、`server/net/session.h/.cpp`、
      `server/biz/user_management_service.cpp` 在二人不在时由 L5 代改（CR-001 + CR-003，
      第 3.2 节有完整越权记录）。二人均已追认，但**代码本身的逐行复核尚未做**——
      编译通过与实测通过不代表设计取舍没有异议空间。
- [ ] **L1：B1 通用定时任务框架。** 会话过期清理与离线判定已由 `tcp_server.cpp` 的 `QTimer` 每 60s 触发，
      但那是写死的两件事；扩展模块 03/04/06 需要的是可注册 `(周期, 回调)` 列表的框架。
      **这是目前唯一未完成的底座**，见 [expand/00 第 4.8 节](expand/00-扩展功能模块实施路径推荐.md)。
- [ ] **L2：每周交叉试读评审记录**，结论记入第 4 节。
- [ ] 答辩前按 [../DEFENSE-SCRIPT.md](../DEFENSE-SCRIPT.md) 的检查清单走一遍，截图与计时在投影环境下实测。

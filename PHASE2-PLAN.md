# 第二阶段任务清单 · 大数据可视化大屏与机器学习智能分析子系统

> 立项 **2026-09-14**。范围、技术栈与验收口径以本文件为准；第一阶段成果见 [ARCHITECTURE.md](ARCHITECTURE.md)。
> 规则仍适用 [CLAUDE.md](CLAUDE.md)，但**第 2 节技术基线需先走变更流程**（见第 2 节）。

## 0. 一句话定位

第二阶段**不是给第一阶段补功能，而是用另一套技术栈把同类分析重做一遍**，并与第一阶段**并存**：
Qt 平台继续作为业务系统（纯 Socket 不变），新建的大数据子系统只共用同一份数据源。

## 1. 已决事项（2026-09-14 确认，不再讨论）

| # | 事项 | 结论 |
| --- | --- | --- |
| 1 | DataV 指哪个 | **开源 Vue3 组件库**（`@kjgl77/datav-vue3` 一类），不是阿里云收费 SaaS |
| 2 | 机器学习框架 | **Spark MLlib**，不沿用第一阶段的 scikit-learn |
| 3 | 两阶段关系 | **并存**。Qt 业务系统保持纯 Socket，大数据子系统独立分层 |
| 4 | 用户数据 | **暂不扩充**。因此分析维度不得依赖用户粒度，见第 6 节与第 11 节 |
| 5 | Flask 归属 | Flask **不挂在 Qt 服务端上**，是大数据子系统自己的一层，只读 MySQL 分析结果 |

## 2. 开工前必办：一次契约变更

[CLAUDE.md](CLAUDE.md) 第 2 节「大屏取数」写的是：

> L5 只读 SQLite 导出 JSON 快照，大屏轮询静态文件

[ARCHITECTURE.md](ARCHITECTURE.md) 第 7 节把「大屏不接入服务端」列为关键设计决策，理由是「若为大屏单开 HTTP 服务，服务端就不再是纯 Socket，会破坏说明书考核点」。

第二阶段要求用 Flask 处理 web 请求，与上述约定**表面冲突，实则不冲突**——Flask 服务的是大数据子系统，Qt 服务端一行不改，纯 Socket 考核点完好。但这个新理由必须落到文档上，否则后续代码踩在一条作废的约定上。

**T0 任务**：修订 `CLAUDE.md` 与 `ARCHITECTURE.md`，并在 conventions 3.1 记一条。

> ✅ **已于 2026-09-14 完成。** `CLAUDE.md` 升 v2.0：第 2 节拆为 2.1 业务平台 / 2.2 大数据子系统两张基线表，新增 `[二阶段]` 标记；那条推论改写为「**Qt 服务端始终不提供 HTTP**」，一阶段大屏维持轮询快照不变、二阶段 Flask 独立分层；第 4 节新增 `bigdata/{spark,mllib,api,web}`；第 5 节拆为 5.1（Qt/C++ 原五条）/ 5.2（大数据新五条）。`ARCHITECTURE.md` 第 7 节同步改写。**节号 2/3/4/5/7 未变**，全仓按节号的引用不受影响。
> ⬜ 剩余动作：**全组知晓并重开各自 agent 会话**——旧会话缓存的是 v1.0 基线。

## 3. 目标架构与数据流向

```text
第一阶段（不动）                    第二阶段（新建）
┌──────────────┐
│ Qt 客户端 ×2  │                   ┌─────────────────────────────┐
│   ↕ 自定义TCP │                   │ Vue3 + DataV 大屏（Node 23+）│
│  ecp-server  │                   └──────────────┬──────────────┘
│  纯 Socket    │                                  │ HTTP/JSON
└──────┬───────┘                   ┌──────────────┴──────────────┐
       │ 写                         │ Flask API（只读）            │
       ↓                           └──────────────┬──────────────┘
  charging.db                                     │ SQL
   (SQLite)                                       ↓
       │  ① 导出                              MySQL（分析结果层）
       └──────────→ HDFS / ODS 只读原始层           ↑
                          │                        │ ③ 回写
                          │ ② 读                    │
                          └──→ Spark 清洗 + 分析 + MLlib ┘
```

四个环节的职责边界：

- **ODS 原始层（HDFS）**：`charging.db` 导出的只读快照，**任何清洗都不在此层做**（SOP 第 2 节硬性要求）
- **Spark**：清洗（DWD）→ 分析（DWS）→ MLlib 建模，全部输出到 MySQL
- **MySQL**：只存**分析结果**，不存明细。Flask 查询压力才可控
- **Flask**：只读接口，不承载业务写入

## 4. 环境搭建（T1，全新，无任何现成基础）

实测：`node` `npm` `java` `hadoop` `hdfs` `spark-submit` `pyspark` `mysql` **全部未安装**，Python 为系统自带 **3.10.12**。

| 组件 | 目标版本 | 备注 |
| --- | --- | --- |
| Python | **3.11 或 3.12** | Ubuntu 22.04 自带 3.10 不达标；apt 里 `python3.11` 候选是 `3.11.0~rc1`（RC 版，不建议）。走 deadsnakes PPA 或 pyenv 装稳定版 |
| JDK | **17**（或 11） | Hadoop 与 Spark 的共同前置，**当前完全没装**，最先装 |
| Hadoop | **3.x** | 伪分布式即可，答辩时数据放 HDFS |
| Spark / PySpark | 与 Python 版本配套 | **先定 Python 再定 Spark**，见下方风险 |
| MySQL | 不限 | 分析结果层 |
| Node.js | **23+** | 走 NodeSource |

> ⚠ **版本联动风险**：PySpark 对 Python 版本有下限要求（较新的 Python 需要较新的 PySpark）。三者顺序必须是 **JDK → Python → PySpark**，装完立刻用一个最小 job 验证 `SparkSession` 能起来再往下走，不要等写完分析代码才发现版本不兼容。具体版本组合以安装当天官方兼容矩阵为准，**不要凭记忆定版本**。

**验收**：`scripts/check-env-phase2.sh`（新建，仿第一阶段 `check-env.sh` 的试编译思路）跑通，输出各组件实际版本号。

## 5. 数据层搭建（T2，依赖 T1）

| 任务 | 内容 | 产出 |
| --- | --- | --- |
| T2.1 | `charging.db` 导出为 CSV/Parquet | 只读原始快照 + 导出脚本 |
| T2.2 | 上传 HDFS，建 ODS 目录结构 | HDFS 路径规范文档 |
| T2.3 | 建 MySQL 库与结果表 | `phase2-schema.sql` |
| T2.4 | 数据血缘与版本约定 | 输入版本 → 规则版本 → 输出版本的记录方式 |

> ✅ **T2 已于 2026-09-14 完成。** `bigdata/spark/export_ods.py`：源库 `mode=ro` 只读打开，14 张表 10524 行导出为 CSV，产物 **chmod 444**（把「ODS 只读」从自觉变成文件权限），`_manifest.json` 记录源库指纹 `2eff8f6c1537b4eb`、导出时刻、每表行数与列名。重跑幂等，源库零改动。

**可用数据实测**（决定了维度怎么设计）：

| 表 | 行数 | 可用性 |
| --- | --- | --- |
| `t_order` | **8292** | ✅ 主表，60 天跨度 |
| `t_pile_log` | **1800** | ✅ 设备事件 |
| `t_carbon_daily` | **360** | ✅ 60 天 × 6 站 |
| `t_pile` / `t_station` | 24 / 6 | ✅ 维表，`t_station` **带经纬度**（可做地图） |
| `t_load_forecast` | 18 | ◐ 偏少，够做拥堵度但不够做趋势 |
| `t_user` | 5（仅 4 个有订单） | ❌ **不做用户维度**（已决事项 4） |
| `t_wallet_tx` | 1 | ❌ 数据量不足 |
| `t_station_review` | 0 | ❌ 空表 |
| `t_admin_oplog` | 3 | ❌ 数据量不足 |

## 6. 数据清洗（T3，依赖 T2）—— 当前完成度为零

必须按 [数据清洗基本流程-操作SOP.md](数据清洗基本流程-操作SOP.md) 的六阶段走，**每阶段都有规定的必须产出**：

| 阶段 | 必须产出 | 现状 |
| --- | --- | --- |
| 1 数据探查 | 数据概况与初始统计 | ✅ `quality/01_profile.json` |
| 2 质量评估 | 六维度（完整/准确/一致/时效/唯一/有效）问题清单 | ✅ `quality/02_issues.csv`，**11 条** |
| 3 制定规则 | 规则清单，每条含编号/作用对象/检测条件/处理动作/处理依据/影响评估/异常去向/验证方式 **八个字段** | ✅ `quality/03_rules.md`，**R001–R011** |
| 4 执行清洗 | 清洗数据 + 异常记录 + 执行日志 | ✅ `dwd/*.parquet` + `quality/pending/` + `04_clean_log.json` |
| 5 清洗校验 | 前后指标对比 + 抽样复核 + 业务断言对账 | ✅ `quality/05_validation.json`，**14/14 PASS** |
| 6 报告存档 | 数据质量报告 + 脚本/规则/日志/血缘归档 | ✅ `quality/06_quality_report.md`（由前五份产物程序生成） |

> ✅ **T3 已于 2026-09-14 完成。** 全程**零删除、零修改值**：1 条标记待核（非法手机号）、剔除 2 列（近乎全空）、
> 订单 8292 → 8292 不变。可重复性已实测——重跑后行数/营收/总时长逐项一致。

> ⚠ **不要拿第一阶段的 `ml/check_signal.py` 顶替阶段 2。** 它做的是「信号体检」（判断特征对模型有无信息量），与 SOP 要的「六维度数据质量评估」是两件事，答辩时会被问穿。

**已知可作为真实样例的质量问题**（实测，正好用来填问题清单）：

- **有效性**：`t_user` 存在 `12345678901`，不符合 `1[3-9]` 开头的手机号规则
- **一致性**：user 3 / user 4 状态为「冻结」，却分别贡献了 2004 / 2132 笔订单，业务逻辑冲突
- **唯一性**：`t_order.order_no` 有唯一约束，需实测验证无重复
- **完整性**：`t_order` 的 `start_time` / `end_time` / `settle_time` 对已取消订单（1815 笔）必然为空，需区分**合理缺失**与**异常缺失**，不可一刀切填充

## 7. Spark 分析维度设计（T4，依赖 T3）

要求：**维度 ≥ 8 个**，**其中至少 2 个维度做对比分析**。下表设计 **10 个维度**（留余量）+ **3 组对比分析**（要求 2 组）。全部不依赖用户粒度。

### 7.1 十个分析维度

| # | 维度 | 数据来源 | 口径要点 |
| --- | --- | --- | --- |
| D1 | 营收时间趋势 | `t_order` status=3 | 按 `settle_time` 聚合到日/周，60 天 |
| D2 | 站点营收与订单量排行 | `t_order` + `t_station` | 6 站横向排名 |
| D3 | 电桩利用率 | `t_order` + `t_pile` | 充电时长占比、单桩订单数，24 桩 |
| D4 | 时段负荷分布 | `t_order` | 按 `start_time` 的 24 小时分布 |
| D5 | 充电量与度均价分布 | `t_order` | `kwh_x100`、`amount/kwh` 分布与分位数 |
| D6 | 订单终态构成 | `t_order` | 已结算 6477 / 已取消 1815，取消率 21.9% |
| D7 | 电桩状态与在线率 | `t_pile` | 在用/闲置/故障三态 + `online` |
| D8 | ~~设备故障事件~~ → **设备事件时序分布** | `t_pile_log` | ⚠ **T3 实测推翻原设计**：`event=4` 故障上报 **0 行**，1763 条「状态变更」的前后状态字段全空，1800 行中仅 **37 条**有效事件（上线 27 / 离线 2 / 远程重启 8）。改做事件类型与时序分布，不做故障分析；**若嫌单薄需另补维度** |
| D9 | 碳减排指标 | `t_carbon_daily` | 排放量、峰平谷构成、排放强度（g/度） |
| D10 | 站点地理分布 | `t_station` | 经纬度打点 + 营收气泡，供大屏地图 |

> ✅ **T4 已于 2026-09-14 完成**（`bigdata/spark/analysis.py`）。实际产出 **11 个维度 + 3 组对比**，
> 15 份结果集落 `bigdata/analysis/*.json`。**7 个含营收的维度逐分对账到 53,936,279 分**，
> 与 T3 阶段 5 的校验基准完全一致。新增 **D11 充电时长分布**，用于补 D8 降级后的余量。

### 7.2 三组对比分析

| # | 对比 | 对比指标 | 已实测的素材 |
| --- | --- | --- | --- |
| **C1** | **快充 vs 慢充** | 订单量、单均价、充电时长、度均价、取消率 | 快充 4829 单均价 **97.58 元**；慢充 1648 单均价 **41.34 元** |
| **C2** | **工作日 vs 周末** | 日均订单量、24 小时曲线形态、站点间差异方向 | 第一阶段已验证各站周末效应**方向相反**（办公型跌、休闲型涨） |
| **C3** | **站点横向对标** | 营收 / 利用率 / 取消率 / 排放强度 多指标同框 | 6 站，适合雷达图 |

**T4 实测结论**（可直接用于答辩）：

| 对比 | 实测发现 |
| --- | --- |
| C1 | 慢充均时长 **282.5 分钟**是快充 38.5 分钟的 **7.3 倍**；慢充取消率 **32.76%** 接近快充 17.33% 的两倍。
两者**度均价同为 147 分**——单价按站点定而非按桩型，差异全体现在充电量与时长上 |
| C2 | 工作日日均 **112.5 单**高于周末 **95.4 单**（+18%），但单均金额几乎持平（83.38 vs 82.93 元），
说明周末是**来的人少**而非**单笔变小** |
| C3 | 深圳湾公园单量/时长均为满分但取消率最高（归一后低取消得分 0）；
市民中心营收与单价双第一。**取消率已取反归一**，否则雷达图上「面积大」会变成贬义 |

> 清洗前后指标对比（SOP 阶段 5 要求）天然构成第四组对比，可一并放上大屏，强化「数据清洗」这一考核点的可见度。

## 8. Flask API 层（T5，依赖 T4）

> ✅ **T5 已于 2026-09-14 完成。** 实现 [bigdata/api/app.py](bigdata/api/app.py)，
> 接口清单 [bigdata/api/README.md](bigdata/api/README.md)，冒烟 `scripts/smoke-api-phase2.py` **14/14 PASS**。

- 4 个接口：`/api/health`（会真探库）、`/api/dimensions`（目录）、`/api/dimension/<name>`、`/api/overview`
- 维度目录**不硬编码**，启动时从 `information_schema` 读，`analysis.py` 增删维度后 API 无需改
- 统一响应 `{code, msg, data}` + 错误码 1001/1002/1003，沿用一阶段「不用裸 bool」口径
- 只监听 `127.0.0.1`：本服务无鉴权，不应暴露到局域网（`[说明书]` 2.2）
- **白名单是 SQL 注入的唯一防线**（表名会拼进 SQL），冒烟含一条注入尝试验证

### T5 踩到并已修复的坑

MySQL 的 `SUM()` 经 pymysql 返回 `Decimal`，Flask 默认序列化成**字符串**（`"78.11"`）。
前端图表库拿到字符串会**静默画不出来**——浏览器里只表现为「图是空的」，极难定位。
已在序列化层统一转 `float`，并在冒烟里加了一条专门防回归的断言。

## 9. Vue3 + DataV 大屏（T6，依赖 T5）

现状：`dataviz/index.html` 是**单文件静态 HTML + CDN 引 ECharts**，无 `package.json`、无构建工具，**无法演进**，需重建前端工程。

> ✅ **T6 已于 2026-09-14 完成。** 实现 [bigdata/web/](bigdata/web/)，说明 [bigdata/web/README.md](bigdata/web/README.md)。
> **14 个面板 / 9 类图表形态**，渲染冒烟 `scripts/smoke-screen-phase2.py` **10/10 PASS**（含真实截图）。

| 任务 | 内容 | 状态 |
| --- | --- | --- |
| T6.1 | Vite 6.4.3 + Vue 3.5.42 工程，Node v24.1.0 | ✅ |
| T6.2 | DataV `@kjgl77/datav-vue3` 1.7.4：边框 / 装饰 / 数字翻牌 / 胶囊图 / 活动环图 | ✅ |
| T6.3 | 11 维度 + 3 组对比全部上屏，面板带维度编号便于答辩对照 | ✅ |
| T6.4 | 9 类形态：折线面积 / 柱 / 横向条形 / 堆叠柱 / 分组柱 / 环图 / 仪表盘 / 雷达 / 散点 | ✅ |

**实际落地的图表形态**（要求「不要过于单一」）：折线+渐变面积、柱、横向条形、堆叠柱、
分组柱、DataV 活动环图、DataV 胶囊图、仪表盘、雷达、散点 —— 共 **9 类**。

> D10 用**经纬度散点**而非地图打点：ECharts 的 `geo` 需要额外的深圳 GeoJSON 资产，
> 引入它增加外部依赖且有授权问题。散点同样表达地理分布，点径随营收，且坐标轴标注了经纬度，
> 不冒充地图。答辩若要求真地图，补一份 GeoJSON 即可，改动局限在 `charts.js` 的 `d10StationGeo`。

> 第一阶段 7 张运营图 + 4 张碳排放图的**统计口径可直接复用**，重写的只是渲染层。

## 10. Spark MLlib 建模（T7，可与 T5/T6 并行）

第一阶段已有 scikit-learn 的 6 个模型与完整评估报告 [ml/reports/forecast_eval.md](ml/reports/forecast_eval.md)。按已决事项 2，**改用 Spark MLlib 重做**。

> ✅ **T7 已于 2026-09-14 完成。** 评估报告 [bigdata/quality/07_forecast_eval.md](bigdata/quality/07_forecast_eval.md)，
> 模型版本 `gbt-20260914-233309`。

| 任务 | 内容 | 状态 |
| --- | --- | --- |
| T7.1 | 特征工程迁移到 Spark | ✅ `features.py`，站-小时面板 8640 行 × **24 特征**，**电量守恒自检偏差 0.0000 度** |
| T7.2 | MLlib 回归训练（负荷 / 并发数 × 1h / 6h / 24h） | ✅ `train.py`，6 个 `GBTRegressor`，并集网格 5 组 |
| T7.3 | 模型评估：MAE / RMSE / R² + 双基线 | ✅ `report.py` 由 `eval.json` 程序生成，不手写 |
| T7.4 | 预测回写 MySQL | ✅ `predict.py` → `d12_load_forecast`（18 行），大屏第 4 页 |

**最终结果：6 个模型 3 个跑赢基线 B**（负荷 1h +1.0%、负荷 24h +0.2%、并发 1h +7.9%），
其余三个为负（−2.1% ~ −3.9%）。这是如实结果，报告第 4 节主动说明了原因，未作修饰。

> **验证段的选择出现了有意义的分化**：负荷类三个 horizon 全选无正则档，并发数三个全选正则化档。
> 这是数据自己给出的，不是人为指定——网格里两档并存，由验证段各自挑。

> **方法论务必沿用第一阶段**，这是第二阶段最省力也最值钱的继承项：按时间切分（绝不随机切）、双基线对照、验证段选超参（绝不用测试段调参）、主动说明三条限制。这套东西比模型分数本身更能扛住答辩提问。

### T7 踩到并已修复的四个坑

| # | 问题 | 根因与修法 |
| --- | --- | --- |
| 1 | 训练跑了 3.5 小时「没动静」 | **等待器 `pgrep -f "mllib/train.py"` 匹配到了自己**——它的命令行里就含这个字符串，条件永远不成立。训练其实早已崩溃。**不要用 pgrep 模式判断自己启动的进程状态**，用任务通知或 PID 文件 |
| 2 | JVM 启动即崩（`Py4JNetworkError`） | 用 `nohup ... &` 在普通 Bash 调用里起的进程，随该次调用一起被回收。改用工具自带的后台机制 |
| 3 | `java.lang.StackOverflowError` | GBT 每轮在上一轮 RDD 上叠加，血缘线性增长，任务序列化递归爆栈。`checkpointInterval` **不设检查点目录就静默不生效**——`maxIter≤120` 能过、500 必崩。已加 `setCheckpointDir` |
| 4 | 三个 horizon 的 `predict_time` 全是同一时刻 | 起报样本取自监督学习样本，而那份数据**要求标签存在**，于是目标都落在数据末尾。**那不是预测，是报一个已有观测值的时刻**。改为从特征面板末尾起报 |

> 第 1 条最值得记：它让我在三个半小时里持续报告「训练在跑」，而实际早已死亡。
> **自建的状态检测若会匹配自身，得到的永远是假阳性。**

## 11. 已知限制（答辩时主动讲，别等被问）

1. **不做用户维度分析。** 8292 笔订单只分布在 **4 个用户**上（各约 2000 笔），用户分层、复购率、用户画像等维度在数据层面是空的。已决定暂不扩充数据，因此 10 个维度全部绕开用户粒度。
2. **`t_wallet_tx`（1 行）、`t_station_review`（0 行）、`t_admin_oplog`（3 行）数据量不足**，不纳入分析。
3. **`t_load_forecast` 仅 18 行**，够做拥堵度快照，不够做预测趋势对比。
4. 数据为 `ml/gen_history.py` 合成，绝对精度不宜外推到真实部署（第一阶段评估报告已有同样声明）。
5. **D8 设备事件维度被实测推翻后降级。** `t_pile_log` 1800 行中，`event=4` 故障上报 **0 行**、
   1763 条「状态变更」的前后状态字段全空，真正有内容的仅 **37 条**。原设计的「故障分析」不成立，
   已改为事件类型与时序分布。
6. **MLlib 模型 6 个中只有 3 个跑赢基线**，且相对第一阶段全面偏低。两个真实原因：
   少了天气特征（按 5.2 第 10 条只用 ODS 层数据）、框架差异（sklearn HistGB 在小样本上更抗过拟合）。
   详见 [07_forecast_eval.md](bigdata/quality/07_forecast_eval.md) 第 4 节。
7. **Flask 无鉴权，只监听回环地址。** 答辩演示足够，但不可暴露到局域网（`[说明书]` 2.2）。

## 12. 任务依赖与建议顺序

```text
T0 契约变更 ─→ T1 环境 ─→ T2 数据层 ─→ T3 清洗 ─→ T4 Spark 分析 ─┬─→ T5 Flask ─→ T6 大屏
                                                              └─→ T7 MLlib ────────↗
```

**T0–T7 全部完成（2026-09-14）。T8 HDFS 部署完成（2026-09-15）**，见第 13 节。

> 回头看，T1 的版本兼容性确实是最大的不确定项——`PySpark 3.5.x` 只支持到 Python 3.11 这一条
> 决定了整条技术栈的版本组合。**先跑通最小 `SparkSession` 再写业务代码**这个纪律值得保留。

## 13. 验收对照表（老师六条要求 → 任务 → 证据）

| # | 要求 | 达标情况（实测） | 证据 |
| --- | --- | --- | --- |
| 1 | Python **3.11 或 3.12** | ✅ **3.11.15** @ `.venv-phase2` | `bash scripts/check-env-phase2.sh` |
| 2 | 文件存储 **Hadoop 3.x** | ✅ **Hadoop 3.3.6 伪分布式，ODS 已落 HDFS** | `bash scripts/check-env-phase2.sh`；`bigdata/quality/08_hdfs_deploy.json` 逐文件 MD5 |
| 3 | **PySpark** 数据清洗 | ✅ SOP 六阶段全部产出 | `bigdata/quality/01`~`06` |
| 3 | 分析维度 **≥ 8** | ✅ **13 个**（D1–D14，D12 为预测） | `/api/dimensions` 返回 `dimension_count` |
| 3 | **≥ 2 组**对比分析 | ✅ **3 组**（C1 快慢充 / C2 工作日周末 / C3 站点对标） | 大屏第 3 页；`comparison_count` |
| 3 | **Flask** 处理 web 请求 | ✅ 4 个只读接口，冒烟 14/14 | `bigdata/api/README.md`、`scripts/smoke-api-phase2.py` |
| 4 | **Node 23+** / **Vue 3** | ✅ Node **v24.1.0**（LTS）/ Vue **3.5.42** / Vite 6.4.3 | `bigdata/web/package.json` |
| 5 | **DataV** 大屏，图表不单一 | ✅ **5 页 18 面板**，DataV 5 类组件 + ECharts **9 类图表** + 3 张表格 | `bigdata/web/README.md`；渲染冒烟 15/15 PASS |
| 6 | 机器学习预测 + **模型评估** | ✅ 6 个 MLlib 模型，MAE/RMSE/R² + **双基线对照** | `bigdata/quality/07_forecast_eval.md` |

**六条要求全部达标。** 第 2 条原先只完成一半（老师原话是「代码测试过程在本地，
答辩尽量放到 hadoop 上存储」），已于 2026-09-15 补齐，见下节。

## 13.5 T8 · HDFS 部署（2026-09-15 完成）

> **先更正上一版的一句错话。** 本节原先写着「把 `ECP_ODS_ROOT` 改成 `hdfs://` 前缀即可，
> **业务代码一行不用改**」——实测不成立，见下面的坑表第 1、2 条。
> 结论方向是对的（数据源确实靠一个环境变量切换），但**代价不是零**：
> 改了 5 个文件才让这条通路真正成立。这种「看起来已经准备好了」的判断，
> 不实际跑一次就写进验收文档，是会误导后续排期的。

### 部署结论

| 项 | 实测 |
| --- | --- |
| Hadoop | **3.3.6** 伪分布式（仅 HDFS，不起 YARN——Spark 跑 `local[*]`，用不上资源调度） |
| 选版理由 | PySpark 3.5.9 自带 `hadoop-client-api` **3.3.4**，服务端取同一条 3.3 线，不去赌跨版本 |
| JDK | 沿用系统的 **17**。Hadoop 3.3.x 官方支持到 11，靠 `--add-opens` 放开强封装后 NameNode/DataNode 正常 |
| 安装位置 | `~/opt/hadoop-3.3.6`，**不需要 root**（本机 sudo 需密码，单节点也无跨用户需求） |
| ODS 路径 | `hdfs://localhost:9000/ecp/ods`，15 个文件（14 表 + `_manifest.json`） |
| 搬运校验 | **逐文件 MD5** 与本地快照字节级一致（比内容，不比 HDFS 的块级 CRC） |
| 只读保证 | 属主 `ecp_ods`、目录 555 / 文件 444，分析侧以 `ecp_analyst` 身份连 |
| 下游验证 | HDFS 为源重跑 profiling → cleaning → validation，**14/14 PASS**，营收仍为 **53,936,279 分** |

### 只读这件事在 HDFS 上会打折，得单独处理

本地靠 `chmod 444` 就够了——属主自己也写不进去。**HDFS 上 444 挡不住超级用户**，
而超级用户就是启动 NameNode 的那个系统账号，正是我们自己。照搬本地做法，
CLAUDE.md 5.2 第 6 条在 HDFS 上就成了一句自觉。

做法：ODS 交给独立的 HDFS 身份 `ecp_ods`，分析侧固定以 `ecp_analyst` 连
（`spark_session._pin_hdfs_identity`，与 `_pin_worker_python` 同一套「不靠人记得」的思路）。
`scripts/ods-to-hdfs.sh` 末尾会**以分析身份实际试写一次并期待它失败**——
把这条保证验出来，而不是声称。

### T8 踩到并已修复的三个坑

| # | 问题 | 根因与修法 |
| --- | --- | --- |
| 1 | `hdfs://` 路径被悄悄改成 `hdfs:/` | `ODS_ROOT` 当初包成了 `pathlib.Path`，而 **pathlib 会折叠连续斜杠**。Spark 拿到 `hdfs:/localhost:9000/...` 当本地相对路径找，报「Path does not exist」，完全看不出根因。改为 `str` + `ods_file()` 拼接 |
| 2 | `export_ods.py` 对 HDFS 根本不可用 | 它全程是 Python 文件 I/O（`mkdir`/`open`/`chmod`）。真按环境变量拼，只会在本地建出一个名叫 `hdfs:/localhost:9000` 的目录且无人察觉。改为显式拦截 + 分工：该脚本产出**本地权威快照**，`ods-to-hdfs.sh` 负责搬运与校验 |
| 3 | `_manifest.json` 在 HDFS 上「不存在」 | Hadoop 的 `FileInputFormat` **默认过滤 `_` 与 `.` 开头的文件**（`_SUCCESS` 就是这么被忽略的），而清单恰好叫 `_manifest.json`。`hdfs dfs -cat` 读得到、Spark 读不到。改用 Hadoop `FileSystem.open` 直读，绕开 InputFormat 这层过滤 |

> 三个坑有个共同点：**本地那条路径从来不经过 Hadoop 的抽象层**，所以问题一直藏着。
> 「代码只认路径不认介质」是个好目标，但它不会自动成立——得真换一次介质才知道哪里不成立。

> 部署顺序仍是**有意留到最后**的：先用本地目录把清洗、分析、API、大屏、建模全链路跑通，
> 再换存储介质。这个判断经受住了检验——上面三个坑都只在换介质时暴露，
> 一开始就背着 Hadoop 调试，它们会混在业务 bug 里一起出现。

### 日常使用

```bash
bash scripts/install-hadoop-phase2.sh    # 一次性：下载、配置、格式化 NameNode
bash scripts/hdfs-ctl.sh start           # 起 NameNode + DataNode（不走 ssh）
bash scripts/ods-to-hdfs.sh              # 推 ODS 并校验，生成 config/phase2-hdfs.env

set -a; . config/phase2-hdfs.env; set +a # 数据源切到 HDFS
.venv-phase2/bin/python bigdata/spark/validation.py
unset ECP_ODS_ROOT                       # 切回本地目录
```

每个 job 启动时会打印本次数据源，便于事后分辨某次结果跑的是哪一边：

```text
[ODS] HDFS　hdfs://localhost:9000/ecp/ods　身份 ecp_analyst
[ODS] 本地　/home/bit/projects/ev-charging-platform/bigdata/ods
```

## 14. 阶段性收尾（2026-09-14）

### 交付物清单

| 类别 | 位置 | 说明 |
| --- | --- | --- |
| 环境 | `scripts/install-phase2-env.sh`　`scripts/check-env-phase2.sh` | 一个装（需 root），一个自检（**实际建一次 SparkSession**、**实际连一次 NameNode**，不只看 `pip list`） |
| HDFS | `scripts/install-hadoop-phase2.sh`　`hdfs-ctl.sh`　`ods-to-hdfs.sh` | 装 / 起停 / 推 ODS 并校验；留痕 `bigdata/quality/08_hdfs_deploy.json` |
| 契约 | `CLAUDE.md` v2.0 | 第 2.2 节二阶段基线、第 5.2 节五条硬性规则 |
| ODS | `bigdata/spark/export_ods.py` → `bigdata/ods/` → `hdfs:///ecp/ods` | 源库 `mode=ro` 只读、产物 `chmod 444`、`_manifest.json` 血缘；HDFS 侧属主 `ecp_ods`、444 |
| 清洗 | `bigdata/spark/{profiling,cleaning,validation,quality_report}.py` | SOP 六阶段，产出 `bigdata/quality/01`~`06` |
| 分析 | `bigdata/spark/analysis.py` → `bigdata/dwd/`、MySQL | 13 维度 + 3 组对比 |
| API | `bigdata/api/app.py` + `README.md` | 4 个只读接口 |
| 大屏 | `bigdata/web/` + `README.md` | Vue3 + DataV，5 页 18 面板 |
| 建模 | `bigdata/mllib/{features,train,predict,report}.py` | 6 个 GBT 模型 + `quality/07_forecast_eval.md` |
| 测试 | `scripts/smoke-api-phase2.py`　`scripts/smoke-screen-phase2.py` | 接口 14 项、渲染 15 项逐页断言，**含 SQL 注入与真实截图** |
| 用例集 | `第二阶段测试用例.xlsx` ← `scripts/gen-testcases-phase2.py` | 9 表 **130 条用例** + 缺陷 8 条；表格由脚本生成，不手工维护 |

### 全链路复现

```bash
bash scripts/install-phase2-env.sh          # 需 root：JDK / Python 3.11 / MySQL
bash scripts/init-mysql-phase2.sh           # 需 root：建库建账号 → config/phase2.ini
python3.11 -m venv .venv-phase2 && .venv-phase2/bin/pip install -r bigdata/requirements.txt
bash scripts/check-env-phase2.sh            # 应全绿

bash scripts/install-hadoop-phase2.sh       # Hadoop 3.3.6，免 root，装到 ~/opt
bash scripts/hdfs-ctl.sh start              # 起 NameNode + DataNode

.venv-phase2/bin/python bigdata/spark/export_ods.py        # ODS 本地权威快照
bash scripts/ods-to-hdfs.sh                                # 推 HDFS + 逐文件 MD5 校验
set -a; . config/phase2-hdfs.env; set +a                   # 下游数据源切到 HDFS
.venv-phase2/bin/python bigdata/spark/profiling.py         # 清洗 1+2
.venv-phase2/bin/python bigdata/spark/cleaning.py          # 清洗 4
.venv-phase2/bin/python bigdata/spark/validation.py        # 清洗 5（14/14 必须全过）
.venv-phase2/bin/python bigdata/spark/quality_report.py    # 清洗 6
.venv-phase2/bin/python bigdata/spark/analysis.py          # 分析 → MySQL
.venv-phase2/bin/python bigdata/mllib/features.py          # 特征（守恒自检必须 0.0000）
.venv-phase2/bin/python bigdata/mllib/train.py             # 训练，约 80 分钟
.venv-phase2/bin/python bigdata/mllib/report.py            # 评估报告
.venv-phase2/bin/python bigdata/mllib/predict.py           # 预测 → MySQL

.venv-phase2/bin/python bigdata/api/app.py &               # API :5000
cd bigdata/web && npm install && npm run dev                # 大屏 :5173
```

> **境内安装务必加 `-i https://pypi.tuna.tsinghua.edu.cn/simple`**，
> 官方源下 PySpark（318MB sdist）实测会断流。npm 同理，`bigdata/web/.npmrc` 已配 npmmirror。

### 贯穿全程的一条硬校验

营收 **53,936,279 分**、电量 **36,638,035**（×100 度）这两个数字，
在 T3 校验、T4 的 7 个维度、MySQL 落库后、T7 特征面板守恒自检**逐环节对账一致**，
其中电量还与第一阶段碳排放对拍脚本独立算出的数字相同。

口径在环节间漂移是这类项目最常见的暗伤（第一阶段就踩过：大屏与管理端显示成两个形状），
所以每一环都显式做了对账，而不是"应该没问题"。

### 答辩时建议主动讲的三件事

1. **数据清洗全程零删除。** SOP 把删除列为最后手段；实测发现的问题要么可标记、
   要么属正常业务、要么是分析范围问题而非数据错误——都不需要动行。
2. **D8 维度被实测推翻。** 原设计的「设备故障分析」在数据层面是空的（故障事件 0 行），
   如实降级并在报告中记录，比硬凑一个图有说服力。
3. **模型 6 个只赢 3 个，原因讲得清。** 少天气特征 + 框架差异，都是真实差异不是没调好；
   并发 24h 两个阶段收敛到同一数量级，佐证该 horizon 信号本就接近零。

## 15. 归属与更新

本文件属 **L5（SCML，配置管理与文档归档）** 维护。任务状态变化时更新本文件，重大变更另在 [docs/conventions.md](docs/conventions.md) 3.1 记一条。

# `bigdata/` —— 第二阶段大数据子系统　属主 L5

> 任务清单与验收对照见 [../PHASE2-PLAN.md](../PHASE2-PLAN.md)，运行命令见 [../docs/RUNBOOK.md](../docs/RUNBOOK.md) 第 7 节，
> 硬性规则见 [../CLAUDE.md](../CLAUDE.md) 第 5.2 节。本文件只讲**这个目录里有什么、彼此怎么串**。

## 与第一阶段的关系

**两阶段并存，不是替代。** Qt 业务平台一行不改（仍是纯 Socket），
本子系统只共用同一份数据源 `charging.db`，用另一套技术栈把同类分析重做一遍。

```text
charging.db ──① 只读导出──> ODS 原始层 ──② 读──> Spark 清洗 → 分析 → MLlib
 (SQLite,                   (本地目录 /            │
  一阶段业务库)               HDFS，只读)          └──③ 结果回写──> MySQL
                                                                    │
                                        Vue3 + DataV 大屏 <──④ HTTP── Flask（只读）
```

四层的边界是硬的，越界即违规（对应 CLAUDE.md 5.2）：

- **ODS 只读**——清洗、纠错、填充一律在 DWD 做，原始快照一个字节不改（第 6 条）
- **分析不直连 `charging.db`**——一律走 ODS（第 10 条）
- **MySQL 只存分析结果**，不存明细
- **Flask 只读**——不写任何业务库，不现场触发 Spark job（第 8 条）

## 目录

| 目录 | 内容 | 规模 |
| --- | --- | --- |
| `spark/` | 导出、清洗、分析五个 job + 统一 SparkSession 入口 | 8 文件 · 1,594 行 |
| `mllib/` | Spark MLlib 特征、训练、评估、预测 | 4 文件 · 730 行 |
| `api/` | Flask 只读接口（[api/README.md](api/README.md)） | 1 文件 · 238 行 |
| `web/` | Vue3 + DataV 大屏（[web/README.md](web/README.md)） | 13 文件 · 1,006 行 |
| `ods/` | **ODS 原始层**：14 表 10524 行 CSV，`chmod 444`，`_manifest.json` 记血缘 |
| `dwd/` | 清洗后的加工层，Parquet |
| `analysis/` | 分析结果 JSON 快照（同时落 MySQL） |
| `quality/` | 数据清洗六阶段产物 + 模型评估 + HDFS 部署留痕 |

> `ods/` `dwd/` `analysis/` 是**产物**不是源码，由上面的 job 生成，可整个删掉重跑。

## `spark/` 里各个脚本

按执行顺序，前一个的产出是后一个的输入：

| 脚本 | 阶段 | 产出 |
| --- | --- | --- |
| `spark_session.py` | —— | **所有 job 的统一入口**，不要自己 `SparkSession.builder` |
| `export_ods.py` | T2 | `charging.db` → `ods/`，源库 `mode=ro` 只读打开，产物 444 |
| `profiling.py` | T3 阶段 1+2 | `quality/01_profile.json`、`02_issues.csv` |
| `cleaning.py` | T3 阶段 4 | `dwd/*.parquet`、`quality/04_clean_log.json`、`quality/pending/` |
| `validation.py` | T3 阶段 5 | `quality/05_validation.json`，**不过就非零退出** |
| `quality_report.py` | T3 阶段 6 | `quality/06_quality_report.md`，由前五份程序生成，不手写 |
| `analysis.py` | T4 | `analysis/*.json` + MySQL，13 维度 + 3 组对比 |
| `smoke_spark.py` | —— | 最小 SparkSession 冒烟，装完环境先跑它 |

`mllib/` 同理：`features.py` → `train.py` → `report.py` → `predict.py`。

## 两件容易踩的事

**① `spark_session.py` 是入口，有它的道理。** 它在建 session 前钉死三样东西：
worker 与 driver 的 Python 解释器（否则报 `PYTHON_VERSION_MISMATCH`）、
`JAVA_HOME`、以及 ODS 在 HDFS 上时的访问身份。这些都是「shell 里 export 也能修，
但总有人会忘」的东西，集中在一处才不会漏。

**② ODS 的介质可切换，代码只认路径。**

```bash
set -a; . ../config/phase2-hdfs.env; set +a   # 切到 HDFS
unset ECP_ODS_ROOT                            # 切回本地目录
```

每个 job 启动时打印本次数据源，事后能分辨某份结果是哪一边跑的：

```text
[ODS] HDFS　hdfs://localhost:9000/ecp/ods　身份 ecp_analyst
[ODS] 本地　/home/bit/projects/ev-charging-platform/bigdata/ods
```

> ⚠ `export_ods.py` **只写本地目录**，即使 `ECP_ODS_ROOT` 指向 HDFS 也不例外（会直接报错拦住）。
> 它产出的是本地权威快照，推 HDFS 是 `scripts/ods-to-hdfs.sh` 的事——理由见 PHASE2-PLAN 第 13.5 节。

## 产物索引（答辩要指的东西都在这）

| 文件 | 是什么 |
| --- | --- |
| `quality/01_profile.json` ~ `06_quality_report.md` | 数据清洗 SOP 六阶段的规定产出，一阶段一份 |
| `quality/03_rules.md` | R001–R011 清洗规则，每条八个字段 |
| `quality/pending/` | 待核清单——**无法判定的记录进这里，不静默丢弃**（5.2 第 9 条） |
| `quality/07_forecast_eval.md` | MLlib 模型评估，MAE/RMSE/R² + 双基线对照 |
| `quality/08_hdfs_deploy.json` | HDFS 部署留痕，逐文件 MD5 |
| `ods/_manifest.json` | ODS 血缘：源库指纹、导出时刻、每表行数与列名 |

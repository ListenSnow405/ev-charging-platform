# ml/ —— 机器学习智能分析

> **归属 L5**。只写本模块规则；全组共识见根 [CLAUDE.md](../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 说明

[说明书] 1.4：基于时序机器学习算法，预测未来 **1h / 6h / 24h** 各站点的充电负荷、空闲桩数量、高峰时段；用户端据此优先推荐低拥堵高空闲率站点，运营端提前负荷预警。

## 数据库权限

两条**并列**规则，按脚本性质区分，不是「只读 + 例外」：

| 脚本性质 | 权限 | 适用 |
| --- | --- | --- |
| **运行期脚本** | 业务表 `mode=ro` 只读；只写 `t_load_forecast` | `export_snapshot.py`、预测脚本 |
| **离线种子脚本** | 可 INSERT 业务表，见下方约束 | `gen_history.py` |

> **为什么要分开**：只读规则防的是运行期 Python 与服务端争抢、污染业务数据，这个方向是对的。但历史数据生成器必须往 `t_order` 灌数据，否则大屏和模型都没有燃料。所以拆成两条并列规则，而不是给只读规则开口子。

### 离线种子脚本约束（`gen_history.py`）

✅ **CR-002 已于 2026-09-04 获 L2 批复**，以下为批复后的最终约束（见 [docs/conventions.md 第 3.2 节](../docs/conventions.md)）：

- 与服务端进程无关，不随服务端启动，不进任何自动流程，只在开发/演示准备阶段手动跑
- 可写：`t_order`（INSERT）、`t_pile_log`（INSERT）、`t_pile.charge_count` / `charge_duration`（UPDATE 聚合回填）
- 禁止：`t_user` / `t_station` / `t_admin` / `t_wallet_tx` / `t_sys_config`，以及任何表结构变更
- **金额一律整数分**，用 `(price*kwh_x100+50)//100`，与 1204 结算规则一致（批复第 1 条）。
  不能用 `round(x/100)`——Python 的 round 是四舍六入五成双且中间转浮点，对账会差分
- **默认 dry-run**，必须显式 `--commit` 才落库
- **`--reset` 脚本硬性拒绝 `charging.db`**（批复第 2 条），只能对可丢弃副本执行；
  且只删 `ml/data/seed_manifest.json` 记录过的批次，**绝不按时间区间盲删**——
  成员留在历史区间里的测试数据不会被误伤
- **播种批次由 ML 侧的 `seed_manifest.json` 维护**，不写 `t_sys_config`（批复第 3 条）
- 对 `charging.db` 落库前的流程：停服务 → 备份 → 先 dry-run，执行人限 L5/SCML
- **红线自检**：若 `t_order` 存在落在今日或之后的记录，视为库里已有真实联调数据，拒绝执行

## 现有文件

| 文件 | 用途 |
| --- | --- |
| `export_snapshot.py` | 大屏数据快照导出，已可运行 |
| `gen_history.py` | 历史订单/设备日志生成器，已实现并测试，CR-002 已批复 |
| `check_signal.py` | 时序信号体检，生成器改造的验收闸（自相关 / 分站可分性 / 峰谷比 / 物理合理性）|
| `build_features.py` | 站-小时特征面板 → `data/features.csv`，按 horizon 分层，内置滞后穿越自检 |
| `train_forecast.py` | 6 个模型（2 目标 × 3 horizon）+ 时序切分评估 → `reports/forecast_eval.md` |
| `reports/forecast_eval.md` | 精度评估报告，答辩材料，随训练自动重生成 |
| `predict.py` | 推理并回写 `t_load_forecast`；authorizer 锁死只可写这一张表 |
| `selftest.py` | 全链路自动化自检，41 项断言；**全程在临时目录里跑，不碰真实库** |
| `carbon_crosscheck.py` | 扩展 08 对拍：不复用 C++ 逻辑独立重算分摊与排放，与 `t_carbon_daily` 逐格比对；只读，不一致退出码 2 |
| `export_carbon_snapshot.py` | 扩展 08 碳排放大屏快照 → `dataviz/data/carbon.json`；只读 `t_carbon_daily`，**不自己算分摊**（否则会与管理端显示成两个形状） |
| `TESTING.md` | 人工测试流程：视觉、交互、跨模块联调、答辩前检查清单 |
| `data/dev.db` | 私有开发副本（gitignored），全部建模工作在它上面做，不碰 `charging.db` |
| `data/seed_manifest.json` | 播种批次记录，`--reset` 据此精确删除 |
| `requirements.txt` | pandas / numpy / scikit-learn |

## 已落地的建模决策

计划阶段的推导过程已归档，这里只留**改代码时必须知道的口径**。精度数字一律以
[../docs/conventions.md 第 8.1 节](../docs/conventions.md) 为准——同一个指标不要在仓库里两处各写一个数。

| 决策 | 为什么 |
| --- | --- |
| 全局单模型带 station 特征，不分站建模 | 6 站 × 1440 小时拆开后每站样本太少 |
| 三个 horizon 各训一个模型（直接多步），不递归 | 避免误差累积，也好解释 |
| 面板**按 horizon 分层**，不是一张宽表 | 日历/天气取 target 时刻（起报时已知），滞后不得越过 `origin_ts = target_ts − horizon`。计划里的 `t−1h` 相应推广成 `lag_h`——`t−1h` 只在 h=1 成立 |
| 特征含 `*_seas_mean`（扩展窗口同小时均值） | 不给它时 h=6 反而输给基线：滞后在该 horizon 基本是噪声。**报告里必须写明「基线是模型的输入」**，「vs 基线」要读作「在季节均值之上还能再榨出多少」。无穿越性依赖 horizon ≤ 24，`build_features.py` 已断言 |
| 基线选「分工作日/周末的同小时均值」，不选「上周同时」 | 上周同时的相关系数只有 0.04，拿它当基线是刷分 |
| 超参在训练段末尾切 8 天验证集上选，**绝不用测试段** | 固定 400 轮时 h=6 训练 MAE 19.3 / 测试 30.0，纯在学噪声 |
| `is_peak` 阈值取**验证段样本外预测分布**的 P75，随模型存进 `meta.json` | 模型预测是平滑的，拿真值 P75 卡预测实测只触发 7.8%~12.8%（目标 25%），对负荷预警是「该报的不报」这个错误方向 |
| `congestion` 用**取整前**的空闲数算 | 取整后 4 根桩只有 5 个取值，六站排序大量并列，1101 的 `sortBy=1` 失去区分度 |
| 无预测数据时 `congestion` 填 **−1 不是 0** | 0 在拥堵度语义里是「最不拥堵」，会把没数据的站排到推荐第一位 |
| 整批预测共用一个 `create_time` | 协议 2305 明文「取同一 model_version 下 create_time 最大的一批」，逐行取当前时刻会让服务端只捞到最后几行 |
| 推理时面板向后延长的 y 填 **NaN 不是 0** | 0 是「负荷为零」的真实取值，会被 expanding/rolling 当成观测算进均值 |

`predict.py --check-skew` 会逐列比对推理路径造的 21 个特征与 `features.csv`（最大差 1.4e-14），
这个训练/推理偏斜自检已固化进脚本，不是只跑过一次。

**已知限制，答辩时主动讲**：h≥6 的增量小是数据决定的（去季节 t−24h 自相关仅 0.078），不是模型缺陷；
`is_holiday` 在默认 60 天窗口（7/6–9/3）内恒为 0，是零方差特征，要让它有信息量得把 `--days` 拉到覆盖国庆。

> 统计口径约束：`export_snapshot.py` 的营收/趋势/状态口径必须与 `server/biz/statistics_service.cpp`
> 的 2301/2302/2303 保持一致（含「补零」行为），否则大屏与管理端并排会显示成两个形状。
> 改 SQL 前先看服务端怎么写的。

## 运行与测试

流水线命令、顺序约束与回滚方式见 [../docs/RUNBOOK.md 第 4、5 节](../docs/RUNBOOK.md)。
机器验不了的部分（视觉、交互、跨模块联调、演示动线）见 [TESTING.md](TESTING.md)。

`selftest.py` 验的是**契约与不变量**，不是「跑起来不报错」：CR-002 的每条护栏（dry-run 只读、
`--reset` 拒绝 `charging.db`、只删播种批次、红线自检、金额整数分）、占用约束、滞后不穿越、
时序切分不重叠、authorizer 权限边界、快照与服务端 2301/2302/2303 口径对拍、大屏七张图渲染。

⚠ 自检用 `docs/db-schema.sql` 现建测试库，并以临时目录为 cwd 调用各脚本，
因此它们内部的相对路径产物全部落在临时目录 —— **不会覆盖 `ml/data/` 下的真实产物**。

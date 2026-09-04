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
| `data/dev.db` | 私有开发副本（gitignored），全部建模工作在它上面做，不碰 `charging.db` |
| `data/seed_manifest.json` | 播种批次记录，`--reset` 据此精确删除 |
| `requirements.txt` | pandas / numpy / scikit-learn |

## TODO

- [x] **`gen_history.py` 历史数据生成器**——60 天约 2000 单，CR-002 已批复，落库路径已验证
- [x] **生成器补时序结构**——站点画像分化（办公/住宅/休闲/混合）+ AR(1) 潜在需求水平
      + 天气马尔可夫持续性 + 增长趋势 + 电桩占用约束。
      验收由 `check_signal.py` 把关，四项全过：
      t−24h 自相关 0.052 → **0.345**，分站可分性 0.262 → **0.475**（噪声底实测 0.149），
      日内峰谷比 7.3x（≥5），站点峰值负荷 ≤ 装机 85%。
      订单密度同步提到 10 单/快充桩/天、2.5 单/慢充桩/天——原先 1.6 的统一速率下
      快充桩利用率只有 4%，每站每小时期望不到 0.25 单，泊松噪声压过全部结构信号
- [x] **特征工程**——`build_features.py` → `data/features.csv`，22728 行 × 33 列。
      按 horizon 分层：日历/天气取 **target 时刻**（起报时已知），滞后只回看到
      `origin_ts = target_ts − horizon`。计划里的 `t−1h` 推广成 `lag_h`——
      t−1h 只在 h=1 成立，h=6 时它还没发生，直接喂就是穿越
- [x] **时序建模与评估**——`HistGradientBoostingRegressor`，全局单模型 + station 特征，
      2 目标 × 3 horizon = 6 个模型，直接多步。按时间切（末 12 天测试），
      超参在训练段末尾切 8 天验证集选，绝不用测试段调参。
      相对「分工作日/周末的同小时均值」基线：负荷 +10.0% / +4.1% / +2.6%（1h/6h/24h），
      并发数 +33.5% / +2.6% / −0.2%。h≥6 的增量小是数据决定的，
      去季节 t−24h 自相关仅 0.078，详见 `reports/forecast_eval.md`
- [ ] 预测结果回写 `t_load_forecast`
- [ ] 拥堵度计算，供用户端站点推荐排序
- [ ] 精度评估与分析结论成文（答辩材料）

### 阻塞状态：已全部解除

| 事项 | 状态 |
| --- | --- |
| 生成器落库（CR-002） | ✅ L2 已批复 2026-09-04 |
| 预测结果送到两端（CR-001） | ✅ 已并入冻结协议 v1.1，`common/protocol.h` 也已有 `CMD_STAT_LOAD_FORECAST = 2305` |

**下游仍未就绪**（不阻塞 L5 自己的工作）：2305 的服务端 handler 尚未实现，L2 合入后 L3 会立即接管理端预测/预警页。
L5 侧的职责是**先把 `t_load_forecast` 填满**，让 L2 的 handler 落地时只剩一个 query。

> 统计口径约束：`export_snapshot.py` 的营收/趋势/状态口径必须与 `server/biz/statistics_service.cpp`
> 的 2301/2302/2303 保持一致（含「补零」行为），否则大屏与管理端并排会显示成两个形状。

## 环境

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r ml/requirements.txt
```

`.venv/` 已在 `.gitignore` 中。

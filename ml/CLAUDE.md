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

⚠ **待 CR-002 批准后方可执行落库**——见 [docs/conventions.md 第 3.2 节](../docs/conventions.md)。`t_order` / `t_pile_log` / `t_pile` 属主是 L2，未获授权前只能写代码、跑 dry-run，**不得 `--commit`**。

- 与服务端进程无关，不随服务端启动，不进任何自动流程，只在开发/演示准备阶段手动跑
- 可写：`t_order`（INSERT）、`t_pile_log`（INSERT）、`t_pile.charge_count` / `charge_duration`（UPDATE 聚合回填）
- 禁止：`t_user` / `t_station` / `t_admin` / `t_wallet_tx` / `t_sys_config`，以及任何表结构变更
- **默认 dry-run**，只打印将生成多少条；必须显式 `--commit` 才落库
- `--reset` 先清空 `t_order` / `t_pile_log` 再生成，仅限开发库
- **红线自检**：若 `t_order` 存在 `settle_time` 晚于生成区间上界的记录，视为库里已有真实联调数据，拒绝执行

## 现有文件

| 文件 | 用途 |
| --- | --- |
| `export_snapshot.py` | 大屏数据快照导出，已可运行 |
| `gen_history.py` | 历史订单/设备日志生成器，已实现并测试；落库需 CR-002 批准 |
| `requirements.txt` | pandas / numpy / scikit-learn |

## TODO

- [x] **`gen_history.py` 历史数据生成器**——含时段、时长、电量、天气、节假日特征。dry-run 已验证（60 天约 2000 单，见脚本输出）；天气/节假日无对应表列，写入独立的 `ml/data/day_features.csv`。**落库（`--commit`）待 CR-002 批准**
- [ ] 特征工程与时序建模，输出 1h / 6h / 24h 预测
- [ ] 预测结果回写 `t_load_forecast`
- [ ] 拥堵度计算，供用户端站点推荐排序
- [ ] 精度评估与分析结论成文（答辩材料）

### 阻塞中

| 事项 | 卡在哪 | 属主 |
| --- | --- | --- |
| 生成器落库 | CR-002 未批 —— 业务表写入授权 | L2 |
| 预测结果送到两端 | CR-001 未批 —— 协议无字段承载 `congestion` / `idle_pile`，1101 与 2300 段都没有通道 | L1 |

两条 CR 全文见 [docs/conventions.md 第 3.2 节](../docs/conventions.md)。**属主改完协议之前不要先按新字段写代码。**

> 大屏不受 CR-001 影响：它直读 SQLite 的 `t_load_forecast`，不经过服务端协议。只有用户端推荐排序和管理端负荷预警要等。

## 环境

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r ml/requirements.txt
```

`.venv/` 已在 `.gitignore` 中。

# ml/ —— 机器学习智能分析

> **归属 L5**。只写本模块规则；全组共识见根 [CLAUDE.md](../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 说明

[说明书] 1.4：基于时序机器学习算法，预测未来 **1h / 6h / 24h** 各站点的充电负荷、空闲桩数量、高峰时段；用户端据此优先推荐低拥堵高空闲率站点，运营端提前负荷预警。

## 数据库权限

**只读业务表**（`mode=ro` 打开）。预测结果写入独立的 `t_load_forecast` 表，不碰任何业务表。

## 现有文件

| 文件 | 用途 |
| --- | --- |
| `export_snapshot.py` | 大屏数据快照导出，已可运行 |
| `requirements.txt` | pandas / numpy / scikit-learn |

## TODO

- [ ] **`gen_history.py` 历史数据生成器**——含时段、时长、电量、天气、节假日特征。这是大屏和模型共同的燃料，**W1 就要交付**，优先级最高
- [ ] 特征工程与时序建模，输出 1h / 6h / 24h 预测
- [ ] 预测结果回写 `t_load_forecast`
- [ ] 拥堵度计算，供用户端站点推荐排序
- [ ] 精度评估与分析结论成文（答辩材料）

## 环境

```bash
python3 -m venv .venv && source .venv/bin/activate
pip install -r ml/requirements.txt
```

`.venv/` 已在 `.gitignore` 中。

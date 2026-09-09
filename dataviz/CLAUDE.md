# dataviz/ —— 大数据可视化大屏

> **归属 L5**。只写本模块规则；全组共识见根 [CLAUDE.md](../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 说明

[说明书] 1.4：以 Web 页面形式呈现，采用 **ECharts** 图表库构建。

## 数据源（技术基线，根 CLAUDE.md 第 2 节）

**不走服务端 HTTP**。由 `ml/export_snapshot.py` 只读 SQLite 定时导出 `data/snapshot.json`，本页轮询该静态文件。

这样服务端保持纯 Socket（[说明书] 1.6 考核点），L2 也不必额外写一套 HTTP 服务。此项是说明书未规定处的推论，**需在设计文档中写明理由**。

## 页面

| 文件 | 内容 | 快照 |
| --- | --- | --- |
| `index.html` | 运营大屏，七块图表：营收三指标 / 全网充电负荷（实时 + 预测双层）/ 站点拥堵度预测 / 营收趋势 / 电桩状态 / 站点排行 / 用户行为 | `data/snapshot.json` |
| `carbon.html` | 扩展 08 碳排放屏，`index.html` 只加了一行入口链接 | `data/carbon.json` |

轮询间隔由快照里的 `pollIntervalSec` 控制，**不放 `t_sys_config`**——那张表归 L2，
而轮询间隔纯粹是大屏与导出器之间的约定，两端都归 L5，放 JSON 里不动冻结契约，前端也不必额外读一次库。

两层负荷曲线必须同口径（kWh 按重叠时长摊进小时桶，数值即平均 kW），否则画在同一张图上是两个量纲；
曲线右端锚在**最后一个有观测的小时**而非「现在」，实线终点才接得上虚线起点——那也正是 `predict.py` 的 origin。

## 运行

启动命令见 [../docs/RUNBOOK.md 第 3 节](../docs/RUNBOOK.md)。答辩现场动线见 [DEMO.md](DEMO.md)。

**数据源自 2026-09-05 起是 `charging.db`**（正式落库 8292 单，见 [../docs/conventions.md](../docs/conventions.md) 第 8.1 节）。
大屏与管理端同源，现场充电会直接反映到「今日营收」。

⚠ 金额字段单位是**分**，前端显示需除以 100。
⚠ 必须经 http 访问；不要对 `charging.db` 跑 `ml/gen_history.py`。两条的理由都在 RUNBOOK 里。

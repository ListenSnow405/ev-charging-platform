# dataviz/ —— 大数据可视化大屏

> **归属 L5**。只写本模块规则；全组共识见根 [CLAUDE.md](../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 说明

[说明书] 1.4：以 Web 页面形式呈现，采用 **ECharts** 图表库构建。

## 数据源（技术基线，根 CLAUDE.md 第 2 节）

**不走服务端 HTTP**。由 `ml/export_snapshot.py` 只读 SQLite 定时导出 `data/snapshot.json`，本页轮询该静态文件。

这样服务端保持纯 Socket（[说明书] 1.6 考核点），L2 也不必额外写一套 HTTP 服务。此项是说明书未规定处的推论，**需在设计文档中写明理由**。

## 本地预览

```bash
python3 ml/export_snapshot.py          # 生成 data/snapshot.json
python3 -m http.server 8080 -d dataviz # 浏览器打开 http://127.0.0.1:8080
```

## TODO

- [x] `index.html` 改为 `fetch("data/snapshot.json")` 轮询。
      间隔取快照里的 `pollIntervalSec`，**不放 `t_sys_config`**——那张表归 L2，
      而轮询间隔纯粹是大屏与导出器之间的约定，两端都归 L5，放 JSON 里不动冻结契约
- [x] 补充充电负荷、用户行为两块图表（[说明书] 1.4 大屏功能）
- [x] 接入 `t_load_forecast`，形成「实时 + 预测」双层看板。
      两层必须同口径（kWh 按重叠时长摊进小时桶，数值即平均 kW），否则画在同一张图上是两个量纲；
      负荷曲线右端锚在**最后一个有观测的小时**而非「现在」，实线终点才接得上虚线起点
- [x] 分辨率适配（栅格 `auto-fit minmax` + ECharts `resize`）与答辩演示动线 → [DEMO.md](DEMO.md)

## 演示

答辩现场照 [DEMO.md](DEMO.md) 跑，里面有启动命令、数据刷新流水线、讲解顺序，
以及三个必须提前知道的现场问题（今日营收必为 0、默认起报点全落低谷、投影分辨率）。

⚠ 金额字段单位是**分**，前端显示需除以 100。

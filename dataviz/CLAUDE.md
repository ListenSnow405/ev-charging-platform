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

- [ ] 把 `index.html` 里的 `demo` 常量换成 `fetch("data/snapshot.json")` 并按 `snapshot_interval` 轮询
- [ ] 补充充电负荷、用户行为两块图表（[说明书] 1.4 大屏功能）
- [ ] 接入 `t_load_forecast` 预测结果，形成「实时 + 预测」双层看板
- [ ] 大屏分辨率适配与答辩演示脚本

⚠ 金额字段单位是**分**，前端显示需除以 100。

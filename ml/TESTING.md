# L5 测试流程　归属 L5

> 自动化部分见 `ml/selftest.py`（一条命令跑完，覆盖契约与不变量）。
> 本文是**人工测试流程**：机器验不了的东西——视觉、交互、跨模块联调、演示动线。
> 模块说明见 [CLAUDE.md](CLAUDE.md) 与 [../dataviz/CLAUDE.md](../dataviz/CLAUDE.md)。

---

## 0. 先跑自动化，通过了再动手

```bash
.venv/bin/python ml/selftest.py          # 约 1 分钟，复用现有模型
.venv/bin/python ml/selftest.py --full   # 约 4 分钟，含完整训练
```

全部 PASS 才继续往下。**自检全程在临时目录里跑，不会碰 `charging.db` 和 `ml/data/dev.db`。**
有 FAIL 先修，人工测试不是用来兜自动化的底的。

---

## 1. 环境（5 分钟）

| # | 操作 | 预期 |
| --- | --- | --- |
| 1.1 | `python3 --version` | ≥ 3.10 |
| 1.2 | `.venv/bin/python -c "import pandas,sklearn,joblib;print('ok')"` | 输出 `ok`。没有 venv 就 `python3 -m venv .venv && .venv/bin/pip install -r ml/requirements.txt` |
| 1.3 | `ls ml/data/models/meta.json` | 存在。不存在则 `.venv/bin/python ml/train_forecast.py` |
| 1.4 | `python3 -c "import sqlite3;print(sqlite3.connect('file:charging.db?mode=ro',uri=True).execute('SELECT COUNT(*) FROM t_order').fetchone())"` | 8292 左右（2026-09-05 落库） |

> `gen_history.py` / `export_snapshot.py` / `check_signal.py` 只用标准库，不需要 venv；
> `build_features.py` / `train_forecast.py` / `predict.py` / `selftest.py` 需要。

---

## 2. 数据链路（15 分钟）

### 2.1 生成器的安全护栏——**每一条都要亲手试一次**

这几条是防事故的，光看代码不算数。

| # | 操作 | 预期 |
| --- | --- | --- |
| 2.1.1 | `python3 ml/gen_history.py charging.db` | 打印统计后明确写「dry-run：未写入数据库」。**再查一次订单数，必须没变** |
| 2.1.2 | `python3 ml/gen_history.py charging.db --commit --reset` | **拒绝**，退出码 4，提示 CR-002 批复第 2 条。`echo $?` 确认 |
| 2.1.3 | `python3 ml/gen_history.py charging.db --commit` | **失败回滚**，退出码 3，提示「多半是重跑没加 --reset」。订单数不变 |

> 2.1.3 是预期行为：历史数据已落库，同一 `--seed` 会生成同一批 `order_no`，撞唯一约束。
> **不要为了让它「成功」而去加 `--reset`——那对 `charging.db` 本来就是拒绝的。**

### 2.2 推理与回写

| # | 操作 | 预期 |
| --- | --- | --- |
| 2.2.1 | `.venv/bin/python ml/predict.py charging.db` | dry-run，打印 3 个 horizon × 6 站的表格，末尾「未写入数据库」 |
| 2.2.2 | 看输出里的 `origin` | 等于「最后一个有观测的小时」。服务端跑过就是今天，没跑过是昨天 23:00 |
| 2.2.3 | `.venv/bin/python ml/predict.py charging.db --commit --prune` | 写入 18 行，并清除旧批次 |
| 2.2.4 | 查 `SELECT COUNT(DISTINCT create_time) FROM t_load_forecast` | **必须是 1**。多于 1 说明批次语义坏了，L2 的 2305 只能捞到部分行 |

### 2.3 快照导出

| # | 操作 | 预期 |
| --- | --- | --- |
| 2.3.1 | `python3 ml/export_snapshot.py` | 打印电桩/站点/预测条数、数据截止时间 |
| 2.3.2 | `python3 -m json.tool dataviz/data/snapshot.json \| head -20` | 合法 JSON，`revenue` 三个字段都是**整数**（单位分） |
| 2.3.3 | 若打印「⚠ t_load_forecast 为空」 | 说明 2.2.3 没做，回去补 |

---

## 3. 大屏人工验收（20 分钟）——**自动化验不了的部分**

`selftest.py` 只验证了「七张图都拿到了非空数据」，**布局、配色、字号、交互手感一概没验**。

```bash
python3 ml/export_snapshot.py
python3 -m http.server 8080 -d dataviz
# 浏览器打开 http://127.0.0.1:8080
```

⚠ **必须走 http://**。直接双击 `file://` 打开会被同源策略拦下，页面只会显示读取失败。

### 3.1 首屏

- [ ] 页面无横向滚动条
- [ ] 顶部三个 KPI 数字带千分位，单位「元」（**不是分**——3000 分要显示成 30）
- [ ] 顶栏「数据截止」：服务端没跑过时应是**黄色**（提示数据非当天）
- [ ] 顶栏「下次刷新」每秒递减，说明页面是活的

### 3.2 七张图逐个看

| 图 | 检查点 |
| --- | --- |
| 全网充电负荷 | 蓝色实线（实测）右端**接上**橙色虚线（预测），中间不断开、不重叠、无突跳 |
| 站点拥堵度预测 | 切 1/6/24 小时，柱子长度跟着变；红色虚线在 0.8 处；橙色柱表示高峰 |
| 营收趋势 | 切近 7 / 近 30 日，点数分别是 7 和 30；**无订单的日期是 0 而不是断点** |
| 电桩状态分布 | 三块合计 = 电桩总数；在用绿、闲置蓝、故障红 |
| 各站点营收排行 | 横向条形，站名完整不截断，按金额降序 |
| 用户下单时段分布 | 24 根柱，能看出早晚双高峰形状 |
| 快慢充与订单终态 | 两个环形图并排，百分比合计 100% |

### 3.3 交互与自适应

- [ ] 悬停每张图都有 tooltip，数值和单位正确
- [ ] 缓慢拖动窗口从最宽到最窄：栅格由三列 → 两列 → 一列，**图表跟着重绘不变形**
- [ ] 浏览器缩放到 50% 和 150%，布局不错位
- [ ] **用答辩现场的实际分辨率/投影仪打开一次**——自适应保证不错位，不保证字号合适

### 3.4 轮询

- [ ] 后台起 `while true; do python3 ml/export_snapshot.py >/dev/null; sleep 30; done &`
- [ ] 等一个刷新周期，顶栏「快照」时间跟着变
- [ ] 停掉 http 服务，页面应显示红色错误提示而不是白屏或静默卡住

---

## 4. 跨模块联调（30 分钟，需要 L2/L3/L4 配合）

L5 只负责把数据备好，**下面这些是别人的活，但要一起验**，否则「数据就绪」是句空话。

| # | 场景 | 预期 | 属主 |
| --- | --- | --- | --- |
| 4.1 | 管理端登录后看销售业绩 | 今日/本月/总营收与大屏 KPI **完全一致**（同一个 `charging.db`） | L3 |
| 4.2 | 管理端近 7/30 日趋势 | 与大屏营收趋势图逐点一致 | L3 |
| 4.3 | 管理端电桩状态 | 与大屏饼图一致 | L3 |
| 4.4 | 2305 站点负荷预测 | **handler 尚未实现**（`docs/conventions.md` 第 9 节已点名 L2）。实现后：`stationId=0` 返回全部站点；`horizon` 传 2 返回 `ERR_PARAM`；表为空时返回 `code=0` + 空 list 而非报错 | L2 |
| 4.5 | 1101 附近充电站 | **取数逻辑尚未实现**。实现后：`congestion` / `idleForecast` 取 `horizon=1` 最新一条；**无预测数据时填 −1 不是 0** | L2 |
| 4.6 | 用户端按 `sortBy=1` 推荐排序 | 低拥堵站排前面；六个站的拥堵度**不应大量并列** | L4 |
| 4.7 | 现场完成一次完整充电 | 结算后重跑 2.3.1，大屏「今日营收」**从 0 变成非 0**，顶栏「数据截止」由黄转正常 | 全组 |

> 4.7 是整条链路最有说服力的一步：真实订单 → SQLite → 快照 → 大屏。答辩时值得现场做一次。

---

## 5. 异常与边界（10 分钟）

| # | 场景 | 预期 |
| --- | --- | --- |
| 5.1 | 删掉 `dataviz/data/snapshot.json` 后刷新页面 | 红色错误提示，写明怎么生成快照；不白屏 |
| 5.2 | 把 `snapshot.json` 改成非法 JSON | 同上，错误提示可读 |
| 5.3 | 清空 `t_load_forecast` 后导出快照 | 导出器打印「⚠ t_load_forecast 为空」；大屏其余六张图正常，拥堵度图为空 |
| 5.4 | `.venv/bin/python ml/predict.py charging.db --origin '2020-01-01 00:00:00'` | 明确报错「超出面板范围」，不产生垃圾数据 |
| 5.5 | `.venv/bin/python ml/build_features.py charging.db --horizons 1,48` | 拒绝执行并说明 `seas_mean` 的无穿越性依赖 horizon ≤ 24 |

---

## 6. 答辩前 30 分钟检查清单

- [ ] `.venv/bin/python ml/selftest.py` 全 PASS
- [ ] `.venv/bin/python ml/predict.py charging.db --commit --prune` 跑一次（让 origin 贴近现在）
- [ ] `python3 ml/export_snapshot.py` 跑一次
- [ ] 后台起导出循环（见 [../dataviz/DEMO.md](../dataviz/DEMO.md)）
- [ ] 用**投影仪实际分辨率**打开大屏看一眼
- [ ] 翻一遍 [reports/forecast_eval.md](reports/forecast_eval.md) 第 6 节，把三条限制说法记熟
- [ ] 确认 `charging-bak-*.db` 备份还在（万一要回滚）

### 三个必答题，提前想好话术

1. **「今日营收怎么是 0？」**　服务端没跑时，库里只有历史数据；生成器刻意不写当天订单
   （红线自检靠这个分辨真实联调数据）。现场充一次电就会变。
2. **「你的模型学到了什么？」**　h=1 主要靠最近一次观测（`load_lag_h` 重要性第一）；
   h≥6 主要是复现季节均值。这是数据决定的——去季节后的 t−24h 自相关只有 0.078。
   详见评估报告第 5、6 节。
3. **「为什么并发数 24 小时预测跑输基线？」**　可榨取的增量本就接近零，
   继续调参把它做成正数只会是在测试集上过拟合。报告里原样写着，没有藏。

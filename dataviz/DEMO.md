# 大屏演示动线　归属 L5

> 答辩现场照着这份跑。数据链路见 [CLAUDE.md](CLAUDE.md)，模型精度见 [../ml/reports/forecast_eval.md](../ml/reports/forecast_eval.md)。

## 1. 启动

```bash
# 大屏只读静态快照，必须经 http 服务访问——直接双击 file:// 会被同源策略拦下，fetch 全挂
python3 ml/export_snapshot.py              # 默认读 charging.db，写 dataviz/data/snapshot.json
python3 -m http.server 8080 -d dataviz     # 浏览器打开 http://127.0.0.1:8080
```

数据源是 **`charging.db`**（2026-09-05 正式落库，8292 单）。大屏与管理端同源，
现场充电产生的订单会直接反映到「今日营收」上。

页面每 30 秒自动重新 `fetch` 快照（间隔由快照里的 `pollIntervalSec` 控制）。
顶栏有倒计时，现场能一眼确认页面是活的而不是卡住了。
**演示期间让 `export_snapshot.py` 在后台每 30 秒跑一次**，数字才会真的动：

```bash
while true; do python3 ml/export_snapshot.py >/dev/null; sleep 30; done &
```

## 2. 刷新预测（演示前跑一次即可）

```bash
.venv/bin/python ml/predict.py charging.db --commit --prune   # 重新推理并回写 t_load_forecast
python3          ml/export_snapshot.py                        # 重新导出快照
```

`predict.py` 挂了 SQLite authorizer，对 `charging.db` **只可能写 `t_load_forecast` 一张表**，
其余表的写入与所有 DDL 在连接层就被拒绝，跑错参数也伤不到组员的联调数据。

## 3. 重建模型（只有改了生成器或特征后才需要）

⚠ **不要对 `charging.db` 跑 `gen_history.py`**。历史数据已于 2026-09-05 正式落库，
且 `--reset` 对 `charging.db` 是硬性拒绝的（CR-002 批复第 2 条），重复执行只会撞 `order_no` 唯一约束。
要重播先在副本上做：

```bash
python3      ml/gen_history.py   ml/data/dev.db --commit --reset   # 1 重播历史（仅副本）
python3      ml/check_signal.py                                    # 2 信号体检，四项须全过
.venv/bin/python ml/build_features.py charging.db                  # 3 特征面板（只读真库）
.venv/bin/python ml/train_forecast.py                              # 4 训练 + 评估报告
.venv/bin/python ml/predict.py charging.db --commit --prune        # 5 推理回写
python3      ml/export_snapshot.py                                 # 6 导出快照
```

顺序不能乱：5 依赖 4 训出的模型与 `meta.json` 里的 is_peak 阈值，6 依赖 5 的回写。

**万一 `charging.db` 需要回滚**：落库前的备份是 `charging-bak-20260905-082255.db`，
`cp` 覆盖回去即可。`--reset` 帮不了你——它硬性拒绝碰这个库。

## 4. 讲解顺序

| # | 看板 | 讲什么 |
| --- | --- | --- |
| 1 | 顶部三指标 | 今日 / 本月 / 累计营收。口径与服务端 2301 完全一致（同一段 SQL 语义），管理端并排打开数字对得上 |
| 2 | 全网充电负荷（实时 + 预测） | 实线是真实观测按重叠时长摊进小时桶算的平均功率；虚线是模型预测，在实线终点接续。两层同口径，不是两个量纲 |
| 3 | 站点拥堵度预测 | 切 1 / 6 / 24 小时。橙色柱是模型判定的高峰时段，红色虚线 0.8 是管理端预警阈值（协议 2305 注明阈值不进协议，由管理端本地判定，大屏保持同一套语言）|
| 4 | 营收趋势 | 切近 7 / 30 日，与服务端 2302 同口径，含无订单日期补零 |
| 5 | 电桩状态 / 站点排行 | 与 2303 同口径 |
| 6 | 用户行为 | 下单时段分布能看出早晚双高峰；快慢充与订单终态占比 |

被问到「模型学到了什么」，翻 [forecast_eval.md](../ml/reports/forecast_eval.md) 第 5、6 节——
里面写明了 h=1 靠最近观测、h≥6 基本是复现季节均值，以及为什么这是数据决定的而不是模型缺陷。

## 5. 三个必须提前知道的现场问题

**① 服务端没跑之前，「今日营收」必然是 0。**
`gen_history.py` 刻意不写当天订单——红线自检正是靠「今日无订单」来分辨库里有没有真实联调数据。
顶栏「数据截止」会标成黄色提示这一点。答辩时服务端在跑，现场充电产生的订单就是今天的，
大屏与管理端读同一个 `charging.db`，这个数会自己填上。
**别在现场才发现，也别临时去改生成器。**

**② 默认起报点会让三个预测都落在低谷。**
`predict.py` 的 origin 取「最后一个有观测的小时」。纯演示数据止于昨天 23:00，
于是 1/6/24 小时后正好是 00:00 / 05:00 / 23:00，全是低谷，`is_peak` 全 0，拥堵度也都很低。
现场有实时订单时 origin 会自然前移到当前小时，预测就落在有意义的时段。
彩排时想看高峰效果，用 `--origin` 指定：

```bash
.venv/bin/python ml/predict.py charging.db --origin '2026-09-04 19:00:00' --commit --prune
```

⚠ 这样 `predictTime` 会落在过去，负荷图上虚线会插进实线中间。**只用于彩排，正式演示别加。**

**③ 分辨率。**
KPI 与图表栅格用 `auto-fit minmax`，1920 宽三列、笔记本两列、竖屏一列，接什么屏都不会错位，
窗口缩放时 ECharts 会跟着 `resize`。但**投影前务必用实际分辨率打开一次**——
自适应保证的是不错位，不保证在 4:3 投影仪上字号合适。

# Flask 只读 API　接口清单

> T5 产出，归属 L5。实现见 [app.py](app.py)，冒烟见 [../../scripts/smoke-api-phase2.py](../../scripts/smoke-api-phase2.py)。

## 职责边界

本服务**只读 MySQL 的分析结果表**，不碰 `charging.db`，不写任何业务库，不现场触发 Spark job
（CLAUDE.md 5.2 第 8 条）。它与 Qt 服务端毫无关系——后者仍是纯 Socket，一行未改。

## 启动

```bash
.venv-phase2/bin/python bigdata/api/app.py       # 监听 127.0.0.1:5000
.venv-phase2/bin/python scripts/smoke-api-phase2.py   # 冒烟（需先启动）
```

只监听回环地址：本服务**无鉴权**，不应暴露到局域网（`[说明书]` 2.2 数据安全）。
生产部署需加反向代理与鉴权。

## 统一响应

```json
{ "code": 0, "msg": "ok", "data": ... }
```

| code | 含义 | HTTP |
| --- | --- | --- |
| 0 | 成功 | 200 |
| 1001 | 入参非法 | 400 |
| 1002 | 维度或接口不存在 | 404 |
| 1003 | 数据库不可用 | 503 |

沿用第一阶段「错误先记日志再返回错误码，不用裸 `bool`」的口径（CLAUDE.md 第 7 节）。

## 接口

| 方法 | 路径 | 说明 |
| --- | --- | --- |
| GET | `/api/health` | 健康检查。**会真的探一次库**，避免「服务活着但库连不上」的假健康 |
| GET | `/api/dimensions` | 维度目录，含 `dimension_count` / `comparison_count` |
| GET | `/api/dimension/<name>` | 取某维度全部数据 |
| GET | `/api/overview` | 大屏头部 KPI |

### `/api/dimension/<name>` 可用的 name

| name | 说明 | 行数 |
| --- | --- | ---: |
| `d1_revenue_trend` | 按日营收 / 订单量 / 电量 | 60 |
| `d2_station_rank` | 6 站营收横向排行 | 6 |
| `d3_pile_utilization` | 24 桩利用率 | 24 |
| `d4_hourly_load` | 24 小时下单分布 | 24 |
| `d5_kwh_distribution` | 充电量分桶 | 6 |
| `d6_order_status` | 订单终态构成 | 2 |
| `d7_pile_status` | 电桩三态与在线率 | 3 |
| `d8_device_events` | 设备事件时序 | 11 |
| `d9_carbon_daily` | 每日碳排放与峰平谷 | 60 |
| `d10_station_geo` | 站点经纬度 + 营收气泡 | 6 |
| `d11_duration_distribution` | 充电时长分桶 × 快慢充 | 18 |
| `c1_fast_vs_slow` | **对比**：快充 vs 慢充 | 2 |
| `c2_weekday_vs_weekend` | **对比**：工作日 vs 周末 | 2 |
| `c2_weekday_weekend_hourly` | **对比**：两者 24 小时曲线 | 48 |
| `c3_station_radar` | **对比**：站点多指标对标 | 6 |

目录不是硬编码的——`app.py` 启动时从 `information_schema` 读，
`analysis.py` 增删维度后本服务无需改代码。

## 前端必须知道的三件事

1. **金额单位是「分」，电量是「度×100」。** `revenue_fen`、`amount_fen`、`kwh_x100`
   一律为整数，**除以 100 是前端的事**（CLAUDE.md 5.2 第 7 条）。服务端不做这一步，
   是为了避免浮点误差在八千余笔订单上累积。
2. **百分比字段是数字不是字符串。** MySQL 的 `SUM()` 经 pymysql 返回 `Decimal`，
   Flask 默认序列化成字符串（`"78.11"`），图表库拿到会**静默画不出来**。
   `app.py` 里已在序列化层统一转 `float`，冒烟有一条专门防这个回归。
3. **`c3_station_radar` 的取消率已取反。** `cancel_score` 越高代表取消率越低，
   这样雷达图上「面积大 = 表现好」才成立。直接用原始 `cancel_rate_pct` 画会反着读。

## 安全

`/api/dimension/<name>` 的 `name` 会拼进 SQL 的表名位置，**白名单是唯一防线**：
只接受启动时从 `information_schema` 读到的表名，其余一律 404。
冒烟里有一条注入尝试（`d1_revenue_trend; DROP TABLE ...`）验证它被挡下且目录完好。

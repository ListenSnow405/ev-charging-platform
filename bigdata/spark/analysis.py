"""T4：Spark 多维分析。读 DWD，产出 11 个分析维度 + 3 组对比分析。

要求（CLAUDE.md 2.2）：维度 ≥ 8，其中 ≥ 2 组对比分析。这里做 11 + 3，留余量——
D8 因 T3 实测发现设备日志近乎全空而被降级，砍掉它仍有 10 个。

输出两处：
  bigdata/analysis/<name>.json   始终写，便于离线核对与前端联调
  MySQL ecp_bigdata.<name>       config/phase2.ini 存在时写，供 Flask 取数

**金额一律整数分**（CLAUDE.md 5.2 第 7 条），除以 100 是展示层的事。
"""
from __future__ import annotations

import configparser
import json
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spark_session import build_spark, REPO_ROOT              # noqa: E402

from pyspark.sql import functions as F                        # noqa: E402
from pyspark.sql import Window                                # noqa: E402

DWD = REPO_ROOT / "bigdata" / "dwd"
OUT = REPO_ROOT / "bigdata" / "analysis"
CFG = REPO_ROOT / "config" / "phase2.ini"

#  这两个条件要惰性构造：F.col() 需要活着的 SparkContext，
#  写成模块级常量会在 import 阶段就求值，那时 session 还没建起来。
def SETTLED():
    return F.col("status") == 3       # 只有已结算订单计入营收/电量


def CANCELLED():
    return F.col("status") == 4

registry: dict[str, list[dict]] = {}
meta: dict[str, str] = {}


def emit(name: str, df, desc: str) -> None:
    """收集一个维度的结果。结果集都很小（最多几百行），直接 collect 到 driver。"""
    rows = [r.asDict() for r in df.collect()]
    registry[name] = rows
    meta[name] = desc
    print(f"  {name:<28} {len(rows):>4} 行　{desc}")


def main() -> int:
    spark = build_spark("ecp-analysis")
    OUT.mkdir(parents=True, exist_ok=True)

    o = spark.read.parquet(str(DWD / "dwd_order.parquet")).cache()
    pile = spark.read.parquet(str(DWD / "dwd_pile.parquet")).cache()
    stn = spark.read.parquet(str(DWD / "dwd_station.parquet")).cache()
    plog = spark.read.parquet(str(DWD / "dwd_pile_log.parquet"))
    carbon = spark.read.parquet(str(DWD / "dwd_carbon_daily.parquet"))

    # 订单 × 电桩 × 站点，后面多数维度都基于它
    oj = (o.join(pile.select("pile_id", "pile_code", "type", "type_label", "power"),
                 "pile_id", "left")
           .join(stn.select("station_id", F.col("name").alias("station_name"),
                            "lng", "lat"), "station_id", "left")).cache()

    print("== T4 分析维度 ==\n")

    # ---------- D1 营收时间趋势 ----------
    emit("d1_revenue_trend",
         (oj.filter(SETTLED()).groupBy("order_date")
            .agg(F.count("*").alias("order_cnt"),
                 F.sum("amount").alias("revenue_fen"),
                 F.sum("kwh_x100").alias("kwh_x100"))
            .orderBy("order_date")),
         "按日营收 / 订单量 / 电量")

    # ---------- D2 站点营收与订单量排行 ----------
    emit("d2_station_rank",
         (oj.filter(SETTLED()).groupBy("station_id", "station_name")
            .agg(F.count("*").alias("order_cnt"),
                 F.sum("amount").alias("revenue_fen"),
                 F.sum("kwh_x100").alias("kwh_x100"),
                 F.round(F.avg("amount")).cast("long").alias("avg_amount_fen"))
            .orderBy(F.desc("revenue_fen"))),
         "6 站营收横向排行")

    # ---------- D3 电桩利用率 ----------
    #  利用率 = 累计充电分钟 / 统计期总分钟。统计期取订单实际跨度，不写死 60 天。
    span = o.agg(((F.max("reserve_time").cast("long")
                   - F.min("reserve_time").cast("long")) / 60).alias("m")).collect()[0]["m"]
    emit("d3_pile_utilization",
         (oj.filter(SETTLED()).groupBy("pile_id", "pile_code", "type_label")
            .agg(F.count("*").alias("order_cnt"),
                 F.sum("charge_minutes").alias("charge_minutes"),
                 F.sum("amount").alias("revenue_fen"))
            .withColumn("utilization_pct",
                        F.round(F.col("charge_minutes") / F.lit(span) * 100, 2))
            .orderBy(F.desc("utilization_pct"))),
         f"24 桩利用率（统计期 {int(span)} 分钟）")

    # ---------- D4 时段负荷分布 ----------
    emit("d4_hourly_load",
         (oj.filter(SETTLED()).groupBy("order_hour")
            .agg(F.count("*").alias("order_cnt"),
                 F.sum("kwh_x100").alias("kwh_x100"),
                 F.sum("amount").alias("revenue_fen"))
            .orderBy("order_hour")),
         "24 小时下单分布")

    # ---------- D5 充电量分布 ----------
    emit("d5_kwh_distribution",
         (oj.filter(SETTLED())
            .withColumn("bucket", (F.col("kwh_x100") / 2000).cast("int") * 20)
            .groupBy("bucket")
            .agg(F.count("*").alias("order_cnt"),
                 F.round(F.avg("unit_price_fen")).cast("long").alias("avg_unit_price_fen"))
            .withColumnRenamed("bucket", "kwh_from")
            .withColumn("kwh_to", F.col("kwh_from") + 20)
            .orderBy("kwh_from")),
         "充电量分桶（每 20 度一档）")

    # ---------- D6 订单终态构成 ----------
    n_all = o.count()
    emit("d6_order_status",
         (o.groupBy("status", "status_label").agg(F.count("*").alias("order_cnt"))
           .withColumn("ratio_pct", F.round(F.col("order_cnt") / F.lit(n_all) * 100, 2))
           .orderBy("status")),
         f"订单终态（总 {n_all} 单）")

    # ---------- D7 电桩状态与在线率 ----------
    emit("d7_pile_status",
         (pile.groupBy("status_label")
              .agg(F.count("*").alias("pile_cnt"),
                   F.sum("online").alias("online_cnt"))
              .withColumn("online_pct",
                          F.round(F.col("online_cnt") / F.col("pile_cnt") * 100, 2))),
         "电桩三态分布与在线率")

    # ---------- D8 设备事件时序（T3 降级后的维度）----------
    #  原设计是「故障事件分析」，T3 实测 event=4 为 0 行、状态变更字段全空，
    #  只能降级为事件类型与时序分布。仅 37 条有效事件，大屏上按小图处理。
    emit("d8_device_events",
         (plog.filter(F.col("event") != 2)          # 剔除无信息的「状态变更」
              .withColumn("event_date", F.to_date("create_time"))
              .groupBy("event_date", "event_label")
              .agg(F.count("*").alias("event_cnt"))
              .orderBy("event_date")),
         "设备事件时序（已剔除无信息的状态变更）")

    # ---------- D9 碳减排 ----------
    emit("d9_carbon_daily",
         (carbon.groupBy("stat_date")
                .agg(F.sum(F.col("emission_g").cast("long")).alias("emission_g"),
                     F.sum(F.col("total_kwh_x100").cast("long")).alias("kwh_x100"),
                     F.sum(F.col("peak_kwh_x100").cast("long")).alias("peak_kwh_x100"),
                     F.sum(F.col("flat_kwh_x100").cast("long")).alias("flat_kwh_x100"),
                     F.sum(F.col("valley_kwh_x100").cast("long")).alias("valley_kwh_x100"))
                .orderBy("stat_date")),
         "每日碳排放与峰平谷构成")

    # ---------- D10 站点地理分布 ----------
    emit("d10_station_geo",
         (oj.filter(SETTLED()).groupBy("station_id", "station_name", "lng", "lat")
            .agg(F.count("*").alias("order_cnt"),
                 F.sum("amount").alias("revenue_fen"))),
         "站点经纬度 + 营收气泡（供地图）")

    # ---------- D11 充电时长分布 ----------
    emit("d11_duration_distribution",
         (oj.filter(SETTLED() & F.col("charge_minutes").isNotNull())
            .withColumn("bucket", (F.col("charge_minutes") / 30).cast("int") * 30)
            .groupBy("bucket", "type_label")
            .agg(F.count("*").alias("order_cnt"))
            .withColumnRenamed("bucket", "minutes_from")
            .withColumn("minutes_to", F.col("minutes_from") + 30)
            .orderBy("minutes_from", "type_label")),
         "充电时长分桶 × 快慢充")

    # ---------- D13 分站碳排放汇总（表格用）----------
    #  D9 按日汇总看趋势，这里按站汇总看横向差异——两者是同一份数据的两个切面。
    #  排放强度 g/度 = 总排放 / 总电量，**不能对各日强度取平均**（那是加权错误）。
    emi = F.sum(F.col("emission_g").cast("double"))
    kwh = F.sum(F.col("total_kwh_x100").cast("double")) / 100.0
    emit("d13_carbon_station",
         (carbon.groupBy("station_id")
                .agg(emi.alias("_emi"), kwh.alias("_kwh"),
                     F.sum(F.col("order_cnt").cast("long")).alias("order_cnt"),
                     F.sum(F.col("peak_kwh_x100").cast("double")).alias("_peak"),
                     F.sum(F.col("valley_kwh_x100").cast("double")).alias("_valley"),
                     F.min(F.col("completeness").cast("double")).alias("min_completeness"))
                .join(stn.select("station_id", F.col("name").alias("station_name")),
                      "station_id", "left")
                .withColumn("emission_kg", F.round(F.col("_emi") / 1000, 1))
                .withColumn("kwh", F.round(F.col("_kwh"), 1))
                .withColumn("intensity_g_per_kwh",
                            F.round(F.col("_emi") / (F.col("_kwh")), 1))
                .withColumn("peak_pct",
                            F.round(F.col("_peak") / (F.col("_kwh") * 100) * 100, 1))
                .withColumn("valley_pct",
                            F.round(F.col("_valley") / (F.col("_kwh") * 100) * 100, 1))
                .select("station_id", "station_name", "emission_kg", "kwh",
                        "intensity_g_per_kwh", "order_cnt", "peak_pct", "valley_pct",
                        "min_completeness")
                .orderBy(F.desc("emission_kg"))),
         "分站碳排放汇总（表格）")

    # ---------- D14 最近 15 日碳排放明细（表格用）----------
    #  带上因子版本与数据质量标记：答辩被问「这个排放量怎么来的」时，
    #  能当场指出用的是哪个版本的排放因子、完整度多少。
    recent = (carbon.groupBy("stat_date")
              .agg(emi.alias("_emi"), kwh.alias("_kwh"),
                   F.sum(F.col("order_cnt").cast("long")).alias("order_cnt"),
                   F.min(F.col("completeness").cast("double")).alias("completeness"),
                   F.first("factor_version").alias("factor_version"))
              .withColumn("emission_kg", F.round(F.col("_emi") / 1000, 1))
              .withColumn("kwh", F.round(F.col("_kwh"), 1))
              .withColumn("intensity_g_per_kwh", F.round(F.col("_emi") / F.col("_kwh"), 1))
              .select("stat_date", "kwh", "emission_kg", "intensity_g_per_kwh",
                      "order_cnt", "completeness", "factor_version")
              .orderBy(F.desc("stat_date")).limit(15))
    emit("d14_carbon_recent", recent, "最近 15 日碳排放明细（表格）")

    print("\n== 对比分析 ==\n")

    # ---------- C1 快充 vs 慢充 ----------
    cancel_by_type = (oj.filter(CANCELLED()).groupBy("type_label")
                        .agg(F.count("*").alias("cancel_cnt")))
    emit("c1_fast_vs_slow",
         (oj.filter(SETTLED()).groupBy("type_label")
            .agg(F.count("*").alias("order_cnt"),
                 F.round(F.avg("amount")).cast("long").alias("avg_amount_fen"),
                 F.round(F.avg("unit_price_fen")).cast("long").alias("avg_unit_price_fen"),
                 F.round(F.avg("charge_minutes"), 1).alias("avg_minutes"),
                 F.sum("amount").alias("revenue_fen"),
                 F.sum("kwh_x100").alias("kwh_x100"))
            .join(cancel_by_type, "type_label", "left")
            .withColumn("cancel_rate_pct",
                        F.round(F.col("cancel_cnt")
                                / (F.col("cancel_cnt") + F.col("order_cnt")) * 100, 2))),
         "快充 vs 慢充：量价时长与取消率")

    # ---------- C2 工作日 vs 周末 ----------
    emit("c2_weekday_vs_weekend",
         (oj.filter(SETTLED()).groupBy("is_weekend")
            .agg(F.count("*").alias("order_cnt"),
                 F.countDistinct("order_date").alias("day_cnt"),
                 F.sum("amount").alias("revenue_fen"),
                 F.round(F.avg("amount")).cast("long").alias("avg_amount_fen"))
            .withColumn("orders_per_day",
                        F.round(F.col("order_cnt") / F.col("day_cnt"), 1))
            .withColumn("revenue_per_day_fen",
                        F.round(F.col("revenue_fen") / F.col("day_cnt")).cast("long"))),
         "工作日 vs 周末：日均单量与营收")

    emit("c2_weekday_weekend_hourly",
         (oj.filter(SETTLED()).groupBy("is_weekend", "order_hour")
            .agg(F.count("*").alias("order_cnt"),
                 F.countDistinct("order_date").alias("day_cnt"))
            .withColumn("orders_per_day",
                        F.round(F.col("order_cnt") / F.col("day_cnt"), 2))
            .orderBy("is_weekend", "order_hour")),
         "工作日 vs 周末：24 小时曲线形态")

    # ---------- C3 站点横向对标（雷达图用，归一到 0-100）----------
    base = (oj.filter(SETTLED()).groupBy("station_id", "station_name")
              .agg(F.sum("amount").alias("revenue_fen"),
                   F.count("*").alias("order_cnt"),
                   F.sum("charge_minutes").alias("charge_minutes"),
                   F.round(F.avg("unit_price_fen")).cast("long").alias("avg_unit_price_fen")))
    cancel_by_stn = (oj.filter(CANCELLED()).groupBy("station_id")
                       .agg(F.count("*").alias("cancel_cnt")))
    base = (base.join(cancel_by_stn, "station_id", "left")
                .withColumn("cancel_rate_pct",
                            F.round(F.col("cancel_cnt")
                                    / (F.col("cancel_cnt") + F.col("order_cnt")) * 100, 2)))
    #  归一化：每个指标除以该指标的全站最大值 ×100，使雷达图各轴可比
    w = Window.partitionBy()
    for col in ["revenue_fen", "order_cnt", "charge_minutes", "avg_unit_price_fen"]:
        base = base.withColumn(f"{col}_score",
                               F.round(F.col(col) / F.max(col).over(w) * 100, 1))
    #  取消率是**越低越好**，归一化时取反，否则雷达图上「面积大」会变成贬义
    base = base.withColumn("cancel_score",
                           F.round((1 - F.col("cancel_rate_pct")
                                    / F.max("cancel_rate_pct").over(w)) * 100, 1))
    emit("c3_station_radar", base.orderBy(F.desc("revenue_fen")),
         "站点多指标对标（归一 0-100，取消率已取反）")

    # ---- 落地 ----
    for name, rows in registry.items():
        (OUT / f"{name}.json").write_text(
            json.dumps({"dimension": name, "description": meta[name],
                        "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                        "row_count": len(rows), "rows": rows},
                       ensure_ascii=False, indent=2, default=str), encoding="utf-8")

    dims = [k for k in registry if k.startswith("d")]
    cmps = sorted({k.split("_")[0] for k in registry if k.startswith("c")})
    summary = {"generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
               "dimension_count": len(dims), "comparison_count": len(cmps),
               "dimensions": {k: meta[k] for k in registry}}
    (OUT / "_summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"\n维度 {len(dims)} 个（要求 ≥8）　对比分析 {len(cmps)} 组（要求 ≥2）")
    print(f"结果集 {len(registry)} 份 → {OUT}")

    # ---- MySQL 装载（有配置才做，没有则跳过，不让 T4 卡在建库上）----
    if CFG.exists():
        load_mysql()
    else:
        print(f"\n[跳过 MySQL] 未找到 {CFG.relative_to(REPO_ROOT)}"
              f"　先跑 bash scripts/init-mysql-phase2.sh")
    spark.stop()
    return 0


def load_mysql() -> None:
    """把结果集写进 MySQL。

    结果集最大也就几百行，用 pymysql 从 driver 端直接写即可；
    为了几百行去挂 JDBC 驱动 jar 反而给部署多加一个外部依赖。
    Spark 负责的是**计算**，这里只是落库。
    """
    import pymysql
    cfg = configparser.ConfigParser()
    cfg.read(CFG, encoding="utf-8")
    m = cfg["mysql"]
    conn = pymysql.connect(host=m["host"], port=int(m["port"]), user=m["user"],
                           password=m["password"], database=m["database"],
                           charset="utf8mb4")
    print(f"\n== 装载 MySQL {m['database']} ==\n")
    try:
        with conn.cursor() as cur:
            for name, rows in registry.items():
                if not rows:
                    print(f"  {name:<28} 空结果集，跳过")
                    continue
                cols = list(rows[0].keys())
                # 类型按首个非空值推断：整数→BIGINT，浮点→DOUBLE，其余→VARCHAR
                defs = []
                for c in cols:
                    v = next((r[c] for r in rows if r[c] is not None), None)
                    if isinstance(v, bool):
                        t = "TINYINT(1)"
                    elif isinstance(v, int):
                        t = "BIGINT"
                    elif isinstance(v, float):
                        t = "DOUBLE"
                    else:
                        t = "VARCHAR(128)"
                    defs.append(f"`{c}` {t}")
                cur.execute(f"DROP TABLE IF EXISTS `{name}`")
                cur.execute(f"CREATE TABLE `{name}` ({', '.join(defs)}) "
                            f"ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 "
                            f"COMMENT='{meta[name][:80]}'")
                ph = ", ".join(["%s"] * len(cols))
                cur.executemany(
                    f"INSERT INTO `{name}` ({', '.join(f'`{c}`' for c in cols)}) VALUES ({ph})",
                    [tuple(None if r[c] is None else
                           (r[c] if isinstance(r[c], (int, float, bool)) else str(r[c]))
                           for c in cols) for r in rows])
                print(f"  {name:<28} {len(rows):>4} 行 → MySQL")
        conn.commit()
        print("\n✓ 全部装载完成")
    finally:
        conn.close()


if __name__ == "__main__":
    raise SystemExit(main())

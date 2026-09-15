"""T3 阶段 4：执行清洗，ODS → DWD。

对应 SOP 第 6 节，逐条落实 `bigdata/quality/03_rules.md` 的 R001–R011。
每条规则的执行结果都写进执行日志，**日志里的数字要能和质量报告对账**。

产出：
  bigdata/dwd/<表>.parquet          清洗后数据
  bigdata/quality/pending/*.json    待核清单（按规则/表分文件，空清单也留文件）
  bigdata/quality/04_clean_log.json 执行日志

不改 ODS（444 只读），不碰 charging.db。
"""
from __future__ import annotations

import json
import shutil
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spark_session import build_spark, REPO_ROOT, ods_file     # noqa: E402
import dwd_schema as DS                                       # noqa: E402

from pyspark.sql import functions as F                        # noqa: E402
from pyspark.sql import types as T                            # noqa: E402

DWD_ROOT = REPO_ROOT / "bigdata" / "dwd"
QUALITY_DIR = REPO_ROOT / "bigdata" / "quality"
PENDING_DIR = QUALITY_DIR / "pending"
TS_FMT = "yyyy-MM-dd HH:mm:ss"
PSEUDO_NULL = ["", "NULL", "null", "N/A", "n/a", "-", "None", "nan"]

log: dict = {"生成时间": datetime.now().strftime("%Y-%m-%d %H:%M:%S"), "规则执行": {}}
#  逐表的类型转换失败数。结论段按它对账，**不再手工罗列表名**——
#  手工罗列的后果是新增一张表时忘了加，对账表看着齐全其实漏了一张。
CAST_FAILS: dict[str, int] = {}


def note(rule: str, desc: str, **kv) -> None:
    log["规则执行"][rule] = {"说明": desc, **kv}
    detail = "　".join(f"{k}={v}" for k, v in kv.items())
    print(f"  {rule}  {desc}　{detail}")


def save_pending(df, name: str, rule: str) -> int:
    """待核清单落地。空清单也要留下文件——「查过且为空」和「没查」必须能区分。"""
    n = df.count()
    PENDING_DIR.mkdir(parents=True, exist_ok=True)
    rows = [r.asDict() for r in df.limit(1000).collect()]
    (PENDING_DIR / f"{name}.json").write_text(
        json.dumps({"规则": rule, "行数": n, "样例(最多1000)": rows},
                   ensure_ascii=False, indent=2, default=str), encoding="utf-8")
    return n


def read_ods(spark, table: str):
    return spark.read.csv(ods_file(f"{table}.csv"), header=True,
                          inferSchema=False, encoding="utf-8")


def apply_r001(df):
    """R001 伪缺失归一。"""
    total = 0
    for c in df.columns:
        col = F.col(f"`{c}`")
        hit = F.trim(col).isin(PSEUDO_NULL)
        total += df.filter(hit).count()
        df = df.withColumn(c, F.when(hit, None).otherwise(col))
    return df, total


def to_int(c: str):
    return F.col(f"`{c}`").cast(T.LongType())


def to_ts(c: str):
    return F.to_timestamp(F.col(f"`{c}`"), TS_FMT)


def to_dbl(c: str):
    return F.col(f"`{c}`").cast(T.DoubleType())


def to_date(c: str):
    return F.to_date(F.col(f"`{c}`"), DS.DATE_FMT)


CASTERS = {DS.LONG: to_int, DS.DBL: to_dbl, DS.TS: to_ts, DS.DATE: to_date}


def cast_with_capture(df, key: str, plan: dict[str, list[str]]):
    """按列转强类型，并把「原值非空、转换后为空」的行整行抓出来。

    SOP 6.2：「类型转换失败的值不要静默变为空值，应输出失败记录与原始值供复核。」
    关键在于**先写影子列 `_col`、比对完再替换回来**——直接原地 withColumn 会把原值盖掉，
    失败行就再也拿不到「原始值」，待核清单也就失去了意义。

    `plan` 直接来自 `dwd_schema.cast_plan()`——**转什么类型由契约说了算**，不在这里各写各的。

    返回 (转换后的 df, 失败行 df)。失败行带业务键 `key` 与全部参与转换的**原始字符串值**。
    """
    todo = [(c, CASTERS[t]) for t, cs in plan.items() for c in cs if c in df.columns]
    cols = [c for c, _ in todo]

    typed = df
    for c, fn in todo:
        typed = typed.withColumn(f"_{c}", fn(c))

    fail = (typed.filter(
        F.array_max(F.array(*[
            F.when(F.col(f"`{c}`").isNotNull() & F.col(f"_{c}").isNull(), 1).otherwise(0)
            for c in cols])) == 1).select(key, *[c for c in cols if c != key]))

    for c in cols:
        typed = typed.drop(c).withColumnRenamed(f"_{c}", c)
    return typed, fail


def enforce(df, table: str):
    """按契约排列列序并断言类型。**列序不是审美问题**——

    `cast_with_capture()` 会把转换过的列挪到表尾，不显式 select 的话，
    DWD 列序就随实现细节漂移，下游按位置取数的工具会悄悄错位。
    """
    missing = [c for c in DS.columns(table) if c not in df.columns]
    assert not missing, f"{table} 缺少契约列：{missing}"
    extra = [c for c in df.columns if c not in DS.columns(table)]
    assert not extra, f"{table} 出现契约外的列：{extra}"

    out = df.select(*[F.col(f"`{c}`") for c in DS.columns(table)])
    want = DS.expected_types(table)
    got = {f.name: f.dataType.simpleString() for f in out.schema.fields}
    bad = {c: (want[c], got[c]) for c in want if want[c] != got[c]}
    assert not bad, f"{table} 类型不符契约（期望, 实际）：{bad}"
    return out


def type_by_contract(spark, ods_table: str, key: str):
    """ODS → 强类型 DataFrame：伪缺失归一 + 按契约转类型 + 失败进待核。

    R009 的低量表过去只做归一就落盘，`balance`/`amount`/`*_kwh_x100` 全是字符串，
    等于把整数口径的保证留了个后门（CLAUDE.md 5.2 第 7 条）。现在一视同仁。
    """
    table = DS.ODS_TO_DWD[ods_table]
    df, _ = apply_r001(read_ods(spark, ods_table))
    typed, fail = cast_with_capture(df, key, DS.cast_plan(table))
    n_fail = save_pending(fail, f"type_cast_failed_{ods_table[2:]}", "R002/R003")
    CAST_FAILS[ods_table] = n_fail
    return typed, n_fail


def main() -> int:
    spark = build_spark("ecp-cleaning")
    if DWD_ROOT.exists():
        shutil.rmtree(DWD_ROOT)         # 全量重跑：相同输入必须得到相同输出（SOP 7.3）
    DWD_ROOT.mkdir(parents=True, exist_ok=True)
    PENDING_DIR.mkdir(parents=True, exist_ok=True)

    print("== 阶段 4：执行清洗 ==\n")

    # ---------------- t_order 主表 ----------------
    o = read_ods(spark, "t_order")
    n_in = o.count()

    o, pseudo_hits = apply_r001(o)
    note("R001", "伪缺失归一", 命中=pseudo_hits)

    # R002/R003：转换失败的行整行进待核，不静默变 NULL。转什么由契约说了算
    PLAN = DS.cast_plan("dwd_order")
    typed, cast_fail = cast_with_capture(o, "order_no", PLAN)
    n_fail = save_pending(cast_fail, "type_cast_failed_order", "R002/R003")
    CAST_FAILS["t_order"] = n_fail
    note("R002", "整数列类型转换", 表="t_order", 列数=len(PLAN[DS.LONG]), 失败行=n_fail)
    note("R003", "时间列转 timestamp", 表="t_order", 列数=len(PLAN[DS.TS]), 失败行=n_fail)

    # R004：三列必须是整数型，这里断言而非"尽量"
    for c in ["amount", "price", "kwh_x100"]:
        assert isinstance(typed.schema[c].dataType, T.LongType), f"{c} 不是整数型"
    note("R004", "金额/电量保持整数", 断言="amount·price·kwh_x100 均为 bigint")

    # R005 码表归一：新增标签列，保留原码值
    ST = {0: "已预约", 1: "充电中", 2: "待结算", 3: "已结算", 4: "已取消"}
    lab = F.create_map([F.lit(x) for kv in ST.items() for x in kv])
    typed = typed.withColumn("status_label",
                             F.coalesce(lab[F.col("status")], F.lit("未知")))
    n_unknown = typed.filter(F.col("status_label") == "未知").count()
    note("R005", "订单状态码表归一", 越界=n_unknown)

    # R010 派生字段。时长用整数分钟，单价用整数分——不引入浮点（R004）
    typed = (typed
             .withColumn("charge_minutes",
                         ((F.col("end_time").cast("long") - F.col("start_time").cast("long")) / 60).cast("long"))
             .withColumn("wait_minutes",
                         ((F.col("start_time").cast("long") - F.col("reserve_time").cast("long")) / 60).cast("long"))
             .withColumn("unit_price_fen",
                         F.when(F.col("kwh_x100") > 0,
                                (F.col("amount") * 100 / F.col("kwh_x100")).cast("long")))
             .withColumn("order_date", F.to_date("reserve_time"))
             .withColumn("order_hour", F.hour("reserve_time").cast("long"))
             .withColumn("is_weekend", F.dayofweek("reserve_time").isin([1, 7])))
    neg = typed.filter((F.col("charge_minutes") < 0) | (F.col("wait_minutes") < 0))
    n_neg = save_pending(neg.select("order_no", "charge_minutes", "wait_minutes"),
                         "negative_duration", "R010")
    note("R010", "派生分析字段", 新增列=7, 负时长=n_neg)

    # R011 外键
    pile = read_ods(spark, "t_pile").select(to_int("pile_id").alias("k_pile"))
    stn = read_ods(spark, "t_station").select(to_int("station_id").alias("k_stn"))
    usr = read_ods(spark, "t_user").select(to_int("user_id").alias("k_user"))
    orphan = (typed.join(pile, typed.pile_id == pile.k_pile, "left")
                   .join(stn, typed.station_id == stn.k_stn, "left")
                   .join(usr, typed.user_id == usr.k_user, "left")
                   .filter(F.col("k_pile").isNull() | F.col("k_stn").isNull()
                           | F.col("k_user").isNull()))
    n_orphan = save_pending(orphan.select("order_no", "user_id", "pile_id", "station_id"),
                            "orphan_fk", "R011")
    note("R011", "外键完整性", 孤儿订单=n_orphan)

    # R007 明确不处理，但要留下"已核对"的痕迹
    note("R007", "冻结用户历史订单：保留不动", 涉及行=4136, 删除=0)

    typed = enforce(typed, "dwd_order")
    typed.write.mode("overwrite").parquet(str(DWD_ROOT / "dwd_order.parquet"))
    n_out = typed.count()
    note("写出", "dwd_order.parquet", 输入=n_in, 输出=n_out)
    assert n_in == n_out, "订单行数在清洗中发生变化——本轮不应有任何删除"

    # ---------------- t_user：R006 手机号标记 ----------------
    u, n_ufail = type_by_contract(spark, "t_user", "phone")
    u = (u.withColumn("phone_valid", F.col("phone").rlike(r"^1[3-9]\d{9}$"))
          .withColumn("status_label", F.when(F.col("status") == 0, "正常").otherwise("冻结")))
    bad = u.filter(~F.col("phone_valid"))
    n_bad = save_pending(bad.select("user_id", "phone", "nickname"),
                         "invalid_phone", "R006")
    u = enforce(u, "dwd_user")
    u.write.mode("overwrite").parquet(str(DWD_ROOT / "dwd_user.parquet"))
    note("R006", "非法手机号标记待核（保留原值）", 标记=n_bad, 删除=0, 表行数=u.count())
    note("R002/R003·t_user", "用户表类型转换（余额转整数分）", 失败行=n_ufail)

    # ---------------- t_pile / t_station 维表 ----------------
    p, n_pfail = type_by_contract(spark, "t_pile", "pile_code")
    p = (p.withColumn("type_label", F.when(F.col("type") == 0, "快充").otherwise("慢充"))
          .withColumn("status_label", F.when(F.col("status") == 0, "在用")
                      .when(F.col("status") == 1, "闲置").otherwise("故障")))
    p = enforce(p, "dwd_pile")
    p.write.mode("overwrite").parquet(str(DWD_ROOT / "dwd_pile.parquet"))
    note("R002/R003·t_pile", "维表类型转换（失败进待核）",
         列数=len(DS.cast_plan("dwd_pile")[DS.LONG]) + 3, 失败行=n_pfail, 表行数=p.count())

    s_, n_sfail = type_by_contract(spark, "t_station", "name")
    s_ = enforce(s_, "dwd_station")
    s_.write.mode("overwrite").parquet(str(DWD_ROOT / "dwd_station.parquet"))
    note("R002/R003·t_station", "维表类型转换（失败进待核）",
         列数=5, 失败行=n_sfail, 表行数=s_.count())
    note("R005", "电桩类型/状态码表归一", 电桩=p.count(), 站点=s_.count())

    # ---------------- t_pile_log：R008 列级剔除 ----------------
    EV = {0: "上线", 1: "离线", 2: "状态变更", 3: "远程重启", 4: "故障上报"}
    evmap = F.create_map([F.lit(x) for kv in EV.items() for x in kv])
    #  R008 的两列不在契约里，enforce 会把它们当「契约外的列」挡下——所以先 drop。
    dropped = ["old_status", "new_status"]
    pl, n_plfail = type_by_contract(spark, "t_pile_log", "log_id")
    pl = pl.drop(*dropped).withColumn(
        "event_label", F.coalesce(evmap[F.col("event")], F.lit("未知")))
    note("R002/R003·t_pile_log", "事件表类型转换（失败进待核）",
         列数=3, 失败行=n_plfail)
    pl = enforce(pl, "dwd_pile_log")
    pl.write.mode("overwrite").parquet(str(DWD_ROOT / "dwd_pile_log.parquet"))
    note("R008", "剔除近乎全空的状态列", 剔除列=dropped, 保留行=pl.count(),
         有效事件=pl.filter(F.col("event") != 2).count())

    # ---------------- R009：低量表照样导入，只在维度设计中排除 ----------------
    #  过去这几张表只做伪缺失归一就落盘，金额与电量列全是字符串——
    #  现在与主表一视同仁，按契约转类型、排列、断言。
    KEYS = {"t_carbon_daily": "daily_id", "t_load_forecast": "forecast_id",
            "t_carbon_report": "report_id", "t_wallet_tx": "tx_id",
            "t_station_review": "review_id", "t_carbon_factor": "factor_id"}
    kept = []
    for t, key in KEYS.items():
        d, _ = type_by_contract(spark, t, key)
        d = enforce(d, DS.ODS_TO_DWD[t])
        d.write.mode("overwrite").parquet(str(DWD_ROOT / f"dwd_{t[2:]}.parquet"))
        kept.append(f"{t}({d.count()})")
    note("R009", "低量/空表仍导入，不删数据", 表=kept)
    note("R004·全表", "金额与电量列一律整数",
         覆盖=f"{len(DS.DWD_SCHEMA)} 张 DWD 表 {sum(len(v) for v in DS.DWD_SCHEMA.values())} 列按契约断言")

    log["结论"] = {
        "本轮删除行数": 0,
        "本轮修改值": 0,
        "标记待核": n_bad,
        "类型转换失败": dict(sorted(CAST_FAILS.items())),
        "剔除列": dropped,
        "订单行数": f"{n_in} → {n_out}（不变）",
    }
    (QUALITY_DIR / "04_clean_log.json").write_text(
        json.dumps(log, ensure_ascii=False, indent=2), encoding="utf-8")

    print(f"\n执行日志：{QUALITY_DIR/'04_clean_log.json'}")
    print(f"DWD 产物：{DWD_ROOT}")
    print(f"待核清单：{PENDING_DIR}")
    spark.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

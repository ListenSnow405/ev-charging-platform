"""T3 阶段 5：清洗校验。对应 SOP 第 7 节。

「清洗完成不等于任务完成」——本脚本对 ODS 与 DWD 做三类核对：
  7.1 结构与数量　7.2 质量指标前后对比　7.3 抽样与业务断言

任何一条不过就以非零退出码结束，**不允许带着失败的校验进入下一阶段**。
产出 bigdata/quality/05_validation.json。
"""
from __future__ import annotations

import json
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spark_session import build_spark, REPO_ROOT, ods_file     # noqa: E402
import dwd_schema as DS                                       # noqa: E402

from pyspark.sql import functions as F                        # noqa: E402

DWD_ROOT = REPO_ROOT / "bigdata" / "dwd"
QUALITY_DIR = REPO_ROOT / "bigdata" / "quality"

results: list[dict] = []


def check(name: str, passed: bool, detail: str) -> None:
    results.append({"检查项": name, "结论": "PASS" if passed else "FAIL", "详情": detail})
    print(f"  [{'PASS' if passed else 'FAIL'}] {name}　{detail}")


def main() -> int:
    spark = build_spark("ecp-validation")
    ods = spark.read.csv(ods_file("t_order.csv"), header=True, inferSchema=False)
    dwd = spark.read.parquet(str(DWD_ROOT / "dwd_order.parquet"))

    print("== 7.1 结构与数量 ==\n")
    n_ods, n_dwd = ods.count(), dwd.count()
    check("订单行数对账", n_ods == n_dwd, f"ODS {n_ods} → DWD {n_dwd}（本轮无删除，应相等）")

    pk_null = dwd.filter(F.col("order_no").isNull()).count()
    pk_dup = dwd.groupBy("order_no").count().filter("count > 1").count()
    check("业务主键非空且唯一", pk_null == 0 and pk_dup == 0,
          f"order_no 空值 {pk_null}，重复 {pk_dup}")

    types = {f.name: f.dataType.simpleString() for f in dwd.schema.fields}
    ints_ok = all(types[c] == "bigint" for c in ("amount", "price", "kwh_x100"))
    check("金额/电量为整数型（R004）", ints_ok,
          f"amount={types['amount']} price={types['price']} kwh_x100={types['kwh_x100']}")
    ts_ok = all(types[c] == "timestamp" for c in
                ("reserve_time", "start_time", "end_time", "settle_time"))
    check("时间列为 timestamp（R003）", ts_ok, f"reserve_time={types['reserve_time']}")

    print("\n== 7.2 质量指标前后对比 ==\n")
    # 时间列非空数不得因类型转换而减少——转换失败静默变 null 是 SOP 明令禁止的
    drift = []
    for c in ("reserve_time", "start_time", "end_time", "settle_time"):
        a = ods.filter(F.col(c).isNotNull() & (F.trim(F.col(c)) != "")).count()
        b = dwd.filter(F.col(c).isNotNull()).count()
        if a != b:
            drift.append(f"{c}: {a}→{b}")
    check("时间列非空数无漂移", not drift, "、".join(drift) if drift else "四列逐列一致")

    # 全部 11 张 DWD 表逐表核对，**预期 Schema 来自 dwd_schema.py 契约**。
    # 每表三条：
    #   ① 列名、列序、类型与契约完全一致——列序也查，`cast_with_capture()`
    #      会把转换过的列挪到表尾，不显式断言就会随实现细节漂移；
    #   ② 契约声明非空的列确实没有 NULL；
    #   ③ 转换失败清单存在且为 0 行，且参与转换的列非空数在 ODS 与 DWD 间逐列一致。
    #      ③ 的后半句才是真正的静默置空探测器：清单为空但非空数掉了，说明捕获自己漏了。
    print()
    DWD = {t: spark.read.parquet(str(DWD_ROOT / f"{t}.parquet")).cache()
           for t in DS.ODS_TO_DWD.values()}
    for ods_t, dwd_t in DS.ODS_TO_DWD.items():
        out = DWD[dwd_t]
        want, got = DS.expected_types(dwd_t), {f.name: f.dataType.simpleString()
                                               for f in out.schema.fields}
        order_ok = list(got) == DS.columns(dwd_t)
        bad = {c: f"期望{want[c]}实得{got.get(c, '缺失')}" for c in want
               if got.get(c) != want[c]}
        check(f"{dwd_t} 符合 Schema 契约（列序+类型）", order_ok and not bad,
              f"{len(want)} 列全部一致" if order_ok and not bad
              else f"列序{'一致' if order_ok else '不一致'}；类型偏差 {bad}")

        nn = DS.non_null(dwd_t)
        viol = {c: out.filter(F.col(c).isNull()).count() for c in nn}
        viol = {c: v for c, v in viol.items() if v}
        check(f"{dwd_t} 非空约束", not viol,
              f"{len(nn)} 个非空列均无 NULL" if not viol else f"违规 {viol}")

        f = QUALITY_DIR / "pending" / f"type_cast_failed_{ods_t[2:]}.json"
        listed = json.loads(f.read_text(encoding="utf-8"))["行数"] if f.exists() else None
        src = spark.read.csv(ods_file(f"{ods_t}.csv"), header=True, inferSchema=False)
        cast_cols = [c for cs in DS.cast_plan(dwd_t).values() for c in cs]
        gap = []
        for c in cast_cols:
            if c not in out.columns:
                continue
            a = src.filter(F.col(c).isNotNull() & (F.trim(F.col(c)) != "")).count()
            b = out.filter(F.col(c).isNotNull()).count()
            if a != b:
                gap.append(f"{c}: {a}→{b}")
        check(f"{dwd_t} 转换无静默置空（R002/R003）", listed == 0 and not gap,
              f"待核清单 {listed} 行；{len(cast_cols)} 个转换列非空数逐列一致"
              if listed == 0 and not gap
              else f"清单={listed}　漂移={gap or '无'}")

    # ---- 主键与外键：**11 张表全查，不只查订单** ----
    #  SQLite 建库时 PRIMARY KEY / FOREIGN KEY 是生效的，但数据一旦离开业务库
    #  进了 ODS 就没人守了。DWD 必须自己查一遍，否则「外键完整」只是一句声称。
    print()
    for dwd_t, (pk, bk) in DS.KEYS.items():
        out = DWD[dwd_t]
        rows = out.count()
        prob = []
        for col, kind in ((pk, "代理主键"), (bk, "业务主键")):
            if not col:
                continue
            nulls = out.filter(F.col(col).isNull()).count()
            dups = out.groupBy(col).count().filter("count > 1").count()
            if nulls or dups:
                prob.append(f"{kind} {col} 空值{nulls} 重复{dups}")
        check(f"{dwd_t} 主键非空且唯一", not prob,
              f"{rows} 行；{pk}" + (f" + {bk}" if bk else "") + " 均无空值与重复"
              if not prob else "、".join(prob))

    for dwd_t, fks in DS.FOREIGN_KEYS.items():
        out, orphan = DWD[dwd_t], []
        for col, ref_t, ref_c in fks:
            ref = DWD[ref_t].select(F.col(ref_c).alias("_k")).distinct()
            n_orph = (out.filter(F.col(col).isNotNull())
                         .join(ref, F.col(col) == F.col("_k"), "left_anti").count())
            if n_orph:
                orphan.append(f"{col}→{ref_t}.{ref_c} 孤儿 {n_orph}")
        check(f"{dwd_t} 外键完整性（{len(fks)} 条）", not orphan,
              "全部命中维表" if not orphan else "、".join(orphan))

    # ---- 取值范围：直接核对源库 CHECK 约束 ----
    print()
    for dwd_t, rules in DS.RANGE_RULES.items():
        out = DWD[dwd_t]
        bad = {r: out.filter(f"NOT ({r})").count() for r in rules}
        bad = {r: v for r, v in bad.items() if v}
        check(f"{dwd_t} 取值范围（{len(rules)} 条规则）", not bad,
              f"{len(rules)} 条规则全部满足" if not bad
              else "；".join(f"{r} 违规 {v} 行" for r, v in bad.items()))

    # ---- 时间连续性：按业务频率建完整索引，找缺口（SOP 6.8）----
    print()
    ts_report = {}
    for dwd_t, (date_col, group_col, freq) in DS.TIME_SERIES.items():
        out = DWD[dwd_t]
        if out.count() == 0:
            continue
        span = out.agg(F.min(date_col).alias("lo"), F.max(date_col).alias("hi")).collect()[0]
        want_days = (span["hi"] - span["lo"]).days + 1
        if group_col:
            #  每组都应铺满整个区间：用组数 × 天数作为期望，缺口 = 期望 − 实际
            grp = out.select(group_col).distinct().count()
            got = out.select(group_col, date_col).distinct().count()
            want = want_days * grp
        else:
            grp, got, want = 1, out.select(date_col).distinct().count(), want_days
        ts_report[dwd_t] = {"区间": f"{span['lo']} ~ {span['hi']}", "天数": want_days,
                            "分组数": grp, "期望点数": want, "实际点数": got,
                            "缺口": want - got}
        check(f"{dwd_t} 时间连续性（{freq}）", got == want,
              f"{span['lo']} ~ {span['hi']} 共 {want_days} 天 × {grp} 组 = {want} 点，实际 {got} 点"
              + ("，无缺口" if got == want else f"，**缺 {want - got} 点**"))

    # ---- 关键分布前后对比：清洗不得改变任何统计量 ----
    #  SOP 7.2「关键统计量和分布没有出现无法解释的剧烈变化」。
    #  本轮是零删除零改值，所以标准可以定到最严：**逐项完全相等**，有一点差就是 FAIL。
    print()
    dist = {}
    for col in ("amount", "kwh_x100", "price"):
        a = (ods.select(F.col(col).cast("double").alias("v"))
                .agg(F.count("v"), F.sum("v"), F.min("v"), F.max("v")).collect()[0])
        b = (dwd.select(F.col(col).cast("double").alias("v"))
                .agg(F.count("v"), F.sum("v"), F.min("v"), F.max("v")).collect()[0])
        qa = ods.select(F.col(col).cast("double").alias("v")).approxQuantile("v", [0.25, 0.5, 0.75], 0.0)
        qb = dwd.select(F.col(col).cast("double").alias("v")).approxQuantile("v", [0.25, 0.5, 0.75], 0.0)
        same = tuple(a) == tuple(b) and qa == qb
        dist[col] = {"ODS": {"计数": a[0], "合计": a[1], "最小": a[2], "最大": a[3],
                             "Q1/中位/Q3": qa},
                     "DWD": {"计数": b[0], "合计": b[1], "最小": b[2], "最大": b[3],
                             "Q1/中位/Q3": qb}, "一致": same}
        check(f"{col} 分布前后一致", same,
              f"计数/合计/极值/四分位逐项相等（中位数 {qb[1]:.0f}）" if same
              else f"ODS {tuple(a)}{qa} vs DWD {tuple(b)}{qb}")

    #  状态分布：清洗不得让任何一类订单凭空增减
    sa = {r["status"]: r["n"] for r in
          ods.groupBy("status").agg(F.count("*").alias("n")).collect()}
    sb = {str(r["status"]): r["n"] for r in
          dwd.groupBy("status").agg(F.count("*").alias("n")).collect()}
    check("订单状态分布前后一致", sa == sb,
          f"{len(sb)} 类状态计数逐一相等：{ {k: sb[k] for k in sorted(sb)} }"
          f"（历史单只落终态，0/1/2 本就不该出现）" if sa == sb
          else f"ODS {sa} vs DWD {sb}")

    # 营收合计必须逐分相等：这是最硬的对账
    rev_ods = (ods.filter(F.col("status") == "3")
                  .agg(F.sum(F.col("amount").cast("long"))).collect()[0][0])
    rev_dwd = dwd.filter(F.col("status") == 3).agg(F.sum("amount")).collect()[0][0]
    check("已结算营收逐分对账", rev_ods == rev_dwd,
          f"ODS {rev_ods} 分 vs DWD {rev_dwd} 分")

    kwh_ods = (ods.filter(F.col("status") == "3")
                  .agg(F.sum(F.col("kwh_x100").cast("long"))).collect()[0][0])
    kwh_dwd = dwd.filter(F.col("status") == 3).agg(F.sum("kwh_x100")).collect()[0][0]
    check("已结算电量对账", kwh_ods == kwh_dwd, f"{kwh_ods} == {kwh_dwd}（×100 度）")

    lab_ok = dwd.filter(F.col("status_label") == "未知").count() == 0
    check("码表归一无未知值（R005）", lab_ok, "status_label 无「未知」")

    print("\n== 7.3 业务断言与抽样 ==\n")
    bad_seq = dwd.filter(F.col("end_time").isNotNull() &
                         (F.col("end_time") < F.col("start_time"))).count()
    check("时间顺序 end >= start", bad_seq == 0, f"倒置 {bad_seq} 行")

    neg = dwd.filter((F.col("charge_minutes") < 0) | (F.col("wait_minutes") < 0)).count()
    check("派生时长非负（R010）", neg == 0, f"负值 {neg} 行")

    # 单价派生值应与快照单价吻合；允许 1 分取整偏差
    dev = dwd.filter(F.col("status") == 3).filter(
        F.abs(F.col("unit_price_fen") - F.col("price")) > 1).count()
    check("派生单价与快照单价一致", dev == 0, f"偏差超 1 分的 {dev} 行")

    # 取消单的时长必须为空——这是「合理缺失」，不该被任何规则填上
    filled = dwd.filter((F.col("status") == 4) & F.col("charge_minutes").isNotNull()).count()
    check("取消单时长保持为空（合理缺失未被填充）", filled == 0, f"被误填 {filled} 行")

    #  用户表未因非法手机号而丢行（R006 只标记不删）。
    #  **判据是「与 ODS 行数相等」而不是「等于 5」**——写死行数换个数据集就误报，
    #  和报告里那些硬编码是同一类毛病（2026-09-15 评审第 5 条）。
    u = DWD["dwd_user"]
    u_ods = spark.read.csv(ods_file("t_user.csv"), header=True, inferSchema=False).count()
    n_bad_phone = u.filter(~F.col("phone_valid")).count()
    check("非法手机号只标记不删除（R006）", u.count() == u_ods,
          f"t_user 行数 ODS {u_ods} → DWD {u.count()}，其中 phone_valid=false "
          f"{n_bad_phone} 行（标记保留，未删）")

    # R008 剔除的列确实不在 DWD 里
    pl = DWD["dwd_pile_log"]
    gone = not ({"old_status", "new_status"} & set(pl.columns))
    check("近乎全空列已剔除（R008）", gone,
          f"dwd_pile_log 列：{len(pl.columns)} 个，有效事件 "
          f"{pl.filter(F.col('event') != 2).count()} 条")

    # ---- 抽样复核：**每类状态、每条规则都要有样本** ----
    #  原先只抽 3 条已结算订单——那恰好是最规整的一类，抽它等于没抽。
    #  SOP 7.3 要求「对每类规则抽样查看原值、处理后值和处理原因」，
    #  所以按订单状态分层各抽 2 条，再对每条有产出的规则各取一个样本。
    ORDER_COLS = ["order_no", "status", "status_label", "amount", "kwh_x100",
                  "unit_price_fen", "charge_minutes", "wait_minutes",
                  "order_date", "order_hour", "is_weekend"]
    sample = []
    print("\n  分层抽样（每种订单状态各 2 条）：")
    for st in sorted(r["status"] for r in dwd.select("status").distinct().collect()):
        for r in dwd.filter(F.col("status") == st).select(*ORDER_COLS).limit(2).collect():
            sample.append(r.asDict())
            print(f"    [{r['status_label']}] {r['order_no']}　{r['amount']}分　"
                  f"{r['kwh_x100'] / 100:.2f}度　时长={r['charge_minutes']}　"
                  f"等待={r['wait_minutes']}　周末={r['is_weekend']}")

    rule_samples = {}
    u_bad = DWD["dwd_user"].filter(~F.col("phone_valid")).select(
        "user_id", "phone", "nickname", "phone_valid", "status_label").collect()
    rule_samples["R006 非法手机号（标记不删）"] = [r.asDict() for r in u_bad]
    rule_samples["R008 剔除列后的设备日志"] = [
        r.asDict() for r in DWD["dwd_pile_log"].filter(F.col("event") != 2)
        .select("log_id", "pile_id", "event", "event_label", "operator", "create_time")
        .limit(3).collect()]
    rule_samples["R005 码表归一（原码值保留）"] = [
        r.asDict() for r in DWD["dwd_pile"].select(
            "pile_code", "type", "type_label", "status", "status_label").limit(3).collect()]
    rule_samples["R009 低量表（导入但不做维度）"] = [
        r.asDict() for r in DWD["dwd_wallet_tx"].limit(2).collect()]
    rule_samples["R010 取消单的合理缺失"] = [
        r.asDict() for r in dwd.filter(F.col("status") == 4)
        .select("order_no", "status_label", "start_time", "charge_minutes",
                "unit_price_fen").limit(2).collect()]
    print("\n  规则抽样：" + "、".join(f"{k}({len(v)})" for k, v in rule_samples.items()))

    # ---- DWD 清洗后画像：与 ODS 画像对照，回答「清洗后长什么样」----
    print("\n  生成 DWD 画像 …")
    dwd_profile = {}
    for ods_t, dwd_t in DS.ODS_TO_DWD.items():
        out = DWD[dwd_t]
        rows, cols = out.count(), len(out.columns)
        src_rows = spark.read.csv(ods_file(f"{ods_t}.csv"), header=True,
                                  inferSchema=False).count()
        fields = {}
        if rows:
            aggs = [F.sum(F.when(F.col(f"`{c}`").isNull(), 1).otherwise(0)).alias(c)
                    for c in out.columns]
            miss = out.agg(*aggs).collect()[0]
            fields = {c: {"缺失数": int(miss[c]), "缺失率": round(miss[c] / rows * 100, 2)}
                      for c in out.columns}
        dwd_profile[dwd_t] = {
            "ODS行数": src_rows, "DWD行数": rows, "行数变化": rows - src_rows,
            "列数": cols, "完全重复行数": rows - out.dropDuplicates().count(),
            "字段": fields,
        }

    failed = [r for r in results if r["结论"] == "FAIL"]
    out = {"生成时间": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
           "检查数": len(results), "失败数": len(failed), "明细": results,
           #  关键指标结构化输出。报告过去把电量 36,638,035 写死在文案里，
           #  换数据集就会说谎——现在从这里取。
           "关键指标": {"已结算营收_分": rev_dwd, "已结算电量_kwh_x100": kwh_dwd,
                    "订单行数_ODS": n_ods, "订单行数_DWD": n_dwd},
           "时间连续性": ts_report,
           "关键分布前后对比": dist,
           "抽样": sample,
           "规则抽样": rule_samples}
    (QUALITY_DIR / "05_validation.json").write_text(
        json.dumps(out, ensure_ascii=False, indent=2, default=str), encoding="utf-8")
    (QUALITY_DIR / "10_dwd_profile.json").write_text(
        json.dumps({"生成时间": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                    "说明": "DWD 清洗后画像，与 01_profile.json 的 ODS 画像对照阅读。",
                    "表": dwd_profile}, ensure_ascii=False, indent=2, default=str),
        encoding="utf-8")

    print(f"\n{'='*46}")
    print(f"校验结果：{len(results) - len(failed)}/{len(results)} 通过")
    print("RESULT:", "PASS" if not failed else "FAIL")
    spark.stop()
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())

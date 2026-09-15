"""T3 阶段 1+2：数据探查与六维度质量评估。

对应《数据清洗基本流程-操作SOP》第 3、4 节。产出两份：
  bigdata/quality/01_profile.json —— 数据概况与初始统计（阶段 1 必须产出）
  bigdata/quality/02_issues.csv   —— 数据质量问题清单（阶段 2 必须产出）

**本脚本只看不改**：不写 ODS，不写业务库，不产出清洗结果。
清洗规则要依据这里的实测数字来定（SOP「先评估、后清洗」），所以它必须先跑。
"""
from __future__ import annotations

import csv
import json
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from spark_session import (build_spark, REPO_ROOT,             # noqa: E402
                           ods_file, read_ods_text)

from pyspark.sql import functions as F                        # noqa: E402

QUALITY_DIR = REPO_ROOT / "bigdata" / "quality"
#  伪缺失：SOP 6.1 点名要归一的几种。空串与纯空白也算。
PSEUDO_NULL = ["", "NULL", "null", "N/A", "n/a", "-", "None", "nan"]

issues: list[dict] = []
_seq = 0


def add_issue(dim: str, table: str, field: str, itype: str, cond: str,
              n: int, total: int, sample: str, sev: str, action: str) -> None:
    """登记一条问题。字段对齐 SOP 第 4 节「问题清单至少包含」的九项。"""
    global _seq
    _seq += 1
    issues.append({
        "问题编号": f"Q{_seq:03d}", "质量维度": dim, "表": table, "字段": field,
        "问题类型": itype, "检测条件": cond, "影响数量": n,
        "影响比例": f"{(n / total * 100):.2f}%" if total else "—",
        "典型样例": sample, "严重程度": sev, "建议处理": action,
    })


def read(spark, table: str):
    """ODS 一律按字符串读入。

    让 Spark 推断类型会把「类型转换失败」这件事悄悄变成 null，
    而 SOP 6.2 明确要求「类型转换失败的值不要静默变为空值，应输出失败记录」。
    先原样读进来，转换是 DWD 层的事，失败的要能抓出来。
    """
    return spark.read.csv(ods_file(f"{table}.csv"), header=True,
                          inferSchema=False, encoding="utf-8")


def profile_table(df, table: str) -> dict:
    """阶段 1：通用探查——规模、字段、缺失、唯一值、完全重复。"""
    n = df.count()
    cols = df.columns
    info = {"rows": n, "columns": len(cols), "fields": {}}
    if n == 0:
        info["note"] = "空表"
        return info

    # 缺失与伪缺失一次算完，避免对每列反复扫表
    aggs = []
    for c in cols:
        col = F.col(f"`{c}`")
        miss = F.sum(F.when(col.isNull() | F.trim(col).isin(PSEUDO_NULL), 1).otherwise(0))
        aggs += [miss.alias(f"{c}__miss"), F.countDistinct(col).alias(f"{c}__uniq")]
    row = df.agg(*aggs).collect()[0]

    for c in cols:
        miss = int(row[f"{c}__miss"] or 0)
        info["fields"][c] = {
            "缺失数": miss,
            "缺失率": round(miss / n * 100, 2),
            "唯一值数": int(row[f"{c}__uniq"] or 0),
        }
    info["完全重复行数"] = n - df.dropDuplicates().count()
    return info


def numeric_stats(df, table: str, col: str) -> dict:
    """数值列的分布统计。ODS 是字符串，这里临时转 double 只为出统计量，不落地。"""
    d = df.select(F.col(f"`{col}`").cast("double").alias("v")).filter(F.col("v").isNotNull())
    if d.count() == 0:
        return {}
    s = d.agg(F.min("v"), F.max("v"), F.mean("v"), F.stddev("v")).collect()[0]
    q = d.approxQuantile("v", [0.25, 0.5, 0.75], 0.001)
    return {"min": s[0], "max": s[1], "mean": round(s[2], 2),
            "std": round(s[3], 2) if s[3] else 0.0,
            "Q1": q[0], "中位数": q[1], "Q3": q[2]}


def main() -> int:
    spark = build_spark("ecp-profiling")
    QUALITY_DIR.mkdir(parents=True, exist_ok=True)

    manifest = json.loads(read_ods_text(spark, "_manifest.json"))
    tables = list(manifest["tables"].keys())

    profile = {
        "生成时间": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "数据源": manifest["source"],
        "源库指纹": manifest["source_sha256_16"],
        "ODS导出时间": manifest["exported_at"],
        "表": {},
    }

    print("== 阶段 1：数据探查 ==\n")
    dfs = {}
    for t in tables:
        df = read(spark, t).cache()
        dfs[t] = df
        profile["表"][t] = profile_table(df, t)
        r = profile["表"][t]
        print(f"  {t:<20} {r['rows']:>6} 行 × {r['columns']:>2} 列"
              f"　完全重复 {r.get('完全重复行数', 0)}")

    # ---- t_order 是主表，单独做数值分布与时间范围 --------------------------
    o = dfs["t_order"]
    n_order = profile["表"]["t_order"]["rows"]
    for c in ["amount", "kwh_x100", "price"]:
        profile["表"]["t_order"]["fields"][c]["分布"] = numeric_stats(o, "t_order", c)
    tr = o.agg(F.min("reserve_time"), F.max("reserve_time")).collect()[0]
    profile["表"]["t_order"]["时间范围"] = {"最早预约": tr[0], "最晚预约": tr[1]}
    print(f"\n  t_order 时间范围：{tr[0]} ~ {tr[1]}")

    print("\n== 阶段 2：六维度质量评估 ==\n")

    # ---------- 唯一性 ----------
    dup_no = (o.groupBy("order_no").count().filter("count > 1"))
    k = dup_no.count()
    if k:
        add_issue("唯一性", "t_order", "order_no", "业务主键重复",
                  "GROUP BY order_no HAVING COUNT(*)>1", k, n_order,
                  str(dup_no.first()["order_no"]), "高", "按业务规则保留一条")

    # ---------- 有效性：状态枚举 ----------
    bad_st = o.filter(~F.col("status").isin(["0", "1", "2", "3", "4"]))
    k = bad_st.count()
    if k:
        add_issue("有效性", "t_order", "status", "状态枚举越界",
                  "status NOT IN (0,1,2,3,4)", k, n_order, "", "高", "拒绝入库并人工复核")

    # ---------- 有效性：手机号格式（[说明书] 11 位、1[3-9] 开头）----------
    u = dfs["t_user"]
    n_user = profile["表"]["t_user"]["rows"]
    bad_phone = u.filter(~F.col("phone").rlike(r"^1[3-9]\d{9}$"))
    k = bad_phone.count()
    if k:
        add_issue("有效性", "t_user", "phone", "手机号不符合规则",
                  r"NOT RLIKE '^1[3-9]\d{9}$'", k, n_user,
                  str(bad_phone.first()["phone"]), "中",
                  "标记待核；[说明书] 要求 11 位手机号，疑为测试残留")

    # ---------- 有效性：金额非负 ----------
    neg = o.filter(F.col("amount").cast("double") < 0)
    k = neg.count()
    if k:
        add_issue("有效性", "t_order", "amount", "金额为负",
                  "amount < 0", k, n_order, "", "高", "拒绝入库")

    # ---------- 一致性：冗余字段 station_id 与 pile 所属站是否一致 ----------
    #  t_order.station_id 是 schema 里写明的冗余字段（便于按站统计营收）。
    #  冗余字段天然有不同步风险，这是一致性维度最该查的地方。
    p = dfs["t_pile"].select(F.col("pile_id").alias("p_id"),
                             F.col("station_id").alias("p_station"))
    mism = (o.join(p, o.pile_id == p.p_id, "left")
             .filter(F.col("p_station").isNotNull() & (F.col("station_id") != F.col("p_station"))))
    k = mism.count()
    profile.setdefault("维度事实", {})["冗余station_id不一致行数"] = k
    if k:
        add_issue("一致性", "t_order", "station_id", "冗余字段与电桩所属站不一致",
                  "t_order.station_id <> t_pile.station_id", k, n_order,
                  "", "高", "以 t_pile 为权威来源改写")
    else:
        print("  [一致性] t_order.station_id 与 t_pile 逐行一致，冗余字段无漂移 ✓")

    # ---------- 一致性：冻结用户仍有订单 ----------
    fz = dfs["t_user"].filter(F.col("status") == "1").select(F.col("user_id").alias("fz_id"))
    ord_fz = o.join(fz, o.user_id == fz.fz_id, "inner")
    k = ord_fz.count()
    if k:
        add_issue("一致性", "t_order / t_user", "user_id", "冻结用户存在订单",
                  "t_user.status=1 且该用户有订单", k, n_order,
                  f"user_id={ord_fz.first()['user_id']}", "低",
                  "保留。冻结是后置管理动作，历史订单本就该在；仅作口径说明")

    # ---------- 完整性：关键字段按状态应有/应无 ----------
    settled_no_time = o.filter((F.col("status") == "3") & F.col("settle_time").isNull())
    k = settled_no_time.count()
    if k:
        add_issue("完整性", "t_order", "settle_time", "已结算订单缺结算时间",
                  "status=3 AND settle_time IS NULL", k, n_order, "", "高", "标记待核")
    cancel_has_time = o.filter((F.col("status") == "4") & F.col("settle_time").isNotNull())
    k = cancel_has_time.count()
    if k:
        add_issue("一致性", "t_order", "settle_time", "已取消订单却有结算时间",
                  "status=4 AND settle_time IS NOT NULL", k, n_order, "", "高", "标记待核")

    # 已取消订单缺时间是**合理缺失**，必须与异常缺失分开——SOP 6.3 的要求
    cancel_null = o.filter((F.col("status") == "4") & F.col("start_time").isNull()).count()
    print(f"  [完整性] 已取消订单 {cancel_null} 笔无 start_time —— 属**合理缺失**，不计为问题")

    # ---------- 有效性：时间顺序 ----------
    seq_bad = o.filter(F.col("start_time").isNotNull() & F.col("end_time").isNotNull()
                       & (F.col("end_time") < F.col("start_time")))
    k = seq_bad.count()
    if k:
        add_issue("有效性", "t_order", "start_time/end_time", "时间顺序倒置",
                  "end_time < start_time", k, n_order, "", "高", "拒绝入库并人工复核")

    res_bad = o.filter(F.col("start_time").isNotNull()
                       & (F.col("start_time") < F.col("reserve_time")))
    k = res_bad.count()
    if k:
        add_issue("有效性", "t_order", "reserve_time/start_time", "开始早于预约",
                  "start_time < reserve_time", k, n_order, "", "高", "人工复核")

    # ---------- 准确性：金额与 price×电量 是否对得上 ----------
    #  口径：amount(分) = price(分/度) × kwh_x100/100，四舍五入到分。
    #  容差 1 分，吸收结算侧的取整差异。
    calc = (o.filter(F.col("status") == "3")
             .withColumn("expect",
                         F.round(F.col("price").cast("double") * F.col("kwh_x100").cast("double") / 100.0))
             .withColumn("diff", F.abs(F.col("amount").cast("double") - F.col("expect"))))
    k = calc.filter(F.col("diff") > 1).count()
    n_settled = o.filter(F.col("status") == "3").count()
    profile["维度事实"].update({"已结算订单数": n_settled, "金额与单价电量不符行数": k})
    if k:
        s = calc.filter(F.col("diff") > 1).first()
        add_issue("准确性", "t_order", "amount", "金额与单价×电量不符",
                  "|amount - round(price*kwh_x100/100)| > 1", k, n_settled,
                  f"order_no={s['order_no']} amount={s['amount']} 期望={int(s['expect'])}",
                  "高", "以 price×电量 重算，记录旧值新值")
    else:
        print(f"  [准确性] {n_settled} 笔已结算订单金额与单价×电量逐笔吻合（容差 1 分）✓")

    #  下单用户数——遗留问题里「N 个用户、M 个有订单」过去是写死的
    profile["维度事实"]["下单用户数"] = o.select("user_id").distinct().count()

    # ---------- 准确性：电量 IQR 异常 ----------
    q = (o.filter(F.col("status") == "3")
          .select(F.col("kwh_x100").cast("double").alias("v"))
          .approxQuantile("v", [0.25, 0.75], 0.001))
    iqr = q[1] - q[0]
    lo, hi = q[0] - 1.5 * iqr, q[1] + 1.5 * iqr
    k = o.filter((F.col("status") == "3")
                 & ((F.col("kwh_x100").cast("double") < lo)
                    | (F.col("kwh_x100").cast("double") > hi))).count()
    if k:
        add_issue("准确性", "t_order", "kwh_x100", "电量 IQR 离群",
                  f"kwh_x100 < {lo:.0f} 或 > {hi:.0f}（Q1={q[0]:.0f} Q3={q[1]:.0f}）",
                  k, n_settled, "", "低",
                  "**保留**。快慢充混在一起本就双峰，离群多为真实大电量，非错误")

    # ---------- 唯一性：完全重复行 ----------
    for t in tables:
        d = profile["表"][t].get("完全重复行数", 0)
        if d:
            add_issue("唯一性", t, "(整行)", "完全重复记录",
                      "全字段相同", d, profile["表"][t]["rows"], "", "中", "保留一条")

    # ---------- 时效性 ----------
    latest = o.agg(F.max("settle_time")).collect()[0][0]
    lag = (datetime.now() - datetime.strptime(latest, "%Y-%m-%d %H:%M:%S")).days
    profile["时效性"] = {"最新结算时间": latest, "滞后天数": lag}
    if lag > 7:
        add_issue("时效性", "t_order", "settle_time", "数据滞后",
                  f"最新结算 {latest}，距今 {lag} 天", n_order, n_order, latest, "中",
                  "离线合成数据集，滞后属预期；答辩时说明口径")

    # ---------- 完整性：高缺失率字段（SOP 6.3 分档）----------
    #  上面那些是「我知道要查什么」的业务规则检查，容易漏掉没想到的字段。
    #  这里做通用扫描：任何字段缺失率 > 50%，按 SOP 6.3 都该评估其信息量。
    #
    #  EXPECTED_MISSING 登记**已判定为合理缺失**的字段——不登记就会淹没在噪声里，
    #  登记了就必须写明理由，这本身是质量报告的一部分。
    EXPECTED_MISSING = {
        ("t_order", "start_time"):   "已取消订单从未开始充电，1815 笔缺失 = 取消单数，恒等",
        ("t_order", "end_time"):     "同上",
        ("t_order", "settle_time"):  "同上",
        ("t_user", "avatar"):        "[说明书] 规定默认灰色头像，空值即默认，非异常",
        ("t_pile", "last_heartbeat"): "离线电桩本就没有心跳时间",
        ("t_carbon_factor", "effect_to"): "当前生效的因子没有截止时刻，开口区间是设计如此",
        ("t_wallet_tx", "order_id"): "充值类流水不关联订单",
        ("t_carbon_report", "output_path"): "尚未导出的报告没有文件路径",
        ("t_admin_oplog", "detail"): "detail 是可选备注字段",
    }
    #  合理缺失也要留痕。理由只 print 到终端等于没写——评审拿到归档包
    #  看不到「为什么 avatar 缺失 60% 却不算问题」，无从判断是想清楚了还是漏了。
    #  所以逐条落进 01_profile.json，质量报告再渲染成表。
    print()
    for t in tables:
        rows = profile["表"][t]["rows"]
        if rows == 0:
            continue
        for c, v in profile["表"][t]["fields"].items():
            rate = v["缺失率"]
            if rate <= 50:
                continue
            why = EXPECTED_MISSING.get((t, c))
            if why:
                print(f"  [完整性] {t}.{c} 缺失 {rate}% —— 合理缺失：{why}")
                continue
            add_issue("完整性", t, c, f"字段缺失率 {rate}%（>50%）",
                      "缺失率 > 50%，SOP 6.3 要求评估字段信息量",
                      v["缺失数"], rows, "", "高",
                      "该字段近乎全空，承载不了分析；剔除或改用其他字段")

    #  ---- 合理缺失登记：**全量落盘，不只登记越过 50% 阈值的那几个** ----
    #  上面的循环只在缺失率 > 50% 时才查这张字典，像 t_order.start_time（22%，
    #  等于取消单数）就永远进不了登记表——而它恰恰是最该讲清楚的一条。
    #  这里按字典逐条登记实测缺失率，顺带把「字段已不存在」「早已不缺失」的
    #  过期条目暴露出来：字典与数据脱节比没有字典更危险。
    reg = []
    for (t, c), why in EXPECTED_MISSING.items():
        info = profile["表"].get(t, {})
        field = info.get("fields", {}).get(c)
        if field is None:
            reg.append({"表": t, "字段": c, "缺失数": None, "缺失率": "—",
                        "判定": "⚠ 登记已过期：字段不存在", "理由": why})
            continue
        rate = field["缺失率"]
        reg.append({
            "表": t, "字段": c, "缺失数": field["缺失数"], "缺失率": f"{rate}%",
            "判定": ("合理缺失，不计为问题" if rate > 0 else "⚠ 登记已过期：该字段当前无缺失"),
            "超过SOP阈值": rate > 50, "理由": why,
        })
    profile["合理缺失登记"] = sorted(reg, key=lambda r: (r["表"], r["字段"]))
    print(f"  合理缺失登记 {len(reg)} 条已写入 01_profile.json")

    # ---------- 完整性：枚举事件类型有无实际数据 ----------
    #  1800 行 pile_log 看着不少，但要看每种 event 下面是不是真有内容。
    pl = dfs["t_pile_log"]
    n_pl = profile["表"]["t_pile_log"]["rows"]
    ev = {r["event"]: r["n"] for r in
          pl.groupBy("event").agg(F.count("*").alias("n")).collect()}
    EV_NAME = {"0": "上线", "1": "离线", "2": "状态变更", "3": "远程重启", "4": "故障上报"}
    profile["表"]["t_pile_log"]["event分布"] = {EV_NAME.get(k, k): v for k, v in ev.items()}
    missing_ev = [name for code, name in EV_NAME.items() if code not in ev]
    if missing_ev:
        add_issue("完整性", "t_pile_log", "event", "事件类型无任何记录",
                  f"event 取值中缺少：{'、'.join(missing_ev)}", 0, n_pl,
                  f"已有 {profile['表']['t_pile_log']['event分布']}", "高",
                  "依赖这些事件的分析维度不成立，须在报告与维度设计中删除")

    # ---------- 完整性：事实表的数据量 ----------
    #  **只对事实表判数据量。** 维度表/配置表天生就该行数少——
    #  t_admin 只有 1 个管理员、t_carbon_factor 只有 2 个因子版本，都是设计如此，
    #  把它们报成「数据量不足」是误报，会污染问题清单的可信度。
    FACT_TABLES = {"t_order", "t_pile_log", "t_carbon_daily", "t_wallet_tx",
                   "t_station_review", "t_admin_oplog", "t_load_forecast",
                   "t_carbon_report"}
    for t in tables:
        if t not in FACT_TABLES:
            continue
        r = profile["表"][t]["rows"]
        if r == 0:
            add_issue("完整性", t, "(整表)", "空表", "COUNT(*)=0", 0, 0, "", "高",
                      "不纳入分析维度，在质量报告中说明")
        elif r < 5:
            add_issue("完整性", t, "(整表)", "事实表数据量严重不足",
                      f"COUNT(*)={r} < 5", r, r, "", "中", "不纳入分析维度，在报告中说明")
        elif r < 100:
            add_issue("完整性", t, "(整表)", "事实表数据量偏少，限制可做的分析",
                      f"COUNT(*)={r} < 100", r, r, "", "低",
                      "可做快照类展示，不足以支撑趋势/分布类维度")

    # ---- 落地 ---------------------------------------------------------------
    (QUALITY_DIR / "01_profile.json").write_text(
        json.dumps(profile, ensure_ascii=False, indent=2), encoding="utf-8")

    with open(QUALITY_DIR / "02_issues.csv", "w", newline="", encoding="utf-8-sig") as f:
        w = csv.DictWriter(f, fieldnames=list(issues[0].keys()))
        w.writeheader()
        w.writerows(issues)

    print(f"\n== 问题清单：{len(issues)} 条 ==\n")
    for i in issues:
        print(f"  {i['问题编号']} [{i['质量维度']}] {i['表']}.{i['字段']}"
              f"　{i['问题类型']}　{i['影响数量']} 行 ({i['影响比例']})　{i['严重程度']}")

    print(f"\n产出：\n  {QUALITY_DIR/'01_profile.json'}\n  {QUALITY_DIR/'02_issues.csv'}")
    spark.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

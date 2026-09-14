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
from spark_session import build_spark, ODS_ROOT, REPO_ROOT   # noqa: E402

from pyspark.sql import functions as F                        # noqa: E402

DWD_ROOT = REPO_ROOT / "bigdata" / "dwd"
QUALITY_DIR = REPO_ROOT / "bigdata" / "quality"

results: list[dict] = []


def check(name: str, passed: bool, detail: str) -> None:
    results.append({"检查项": name, "结论": "PASS" if passed else "FAIL", "详情": detail})
    print(f"  [{'PASS' if passed else 'FAIL'}] {name}　{detail}")


def main() -> int:
    spark = build_spark("ecp-validation")
    ods = spark.read.csv(str(ODS_ROOT / "t_order.csv"), header=True, inferSchema=False)
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

    # 用户表未因非法手机号而丢行（R006 只标记不删）
    u = spark.read.parquet(str(DWD_ROOT / "dwd_user.parquet"))
    check("非法手机号只标记不删除（R006）", u.count() == 5,
          f"t_user 行数 {u.count()}，其中 phone_valid=false 共 "
          f"{u.filter(~F.col('phone_valid')).count()} 行")

    # R008 剔除的列确实不在 DWD 里
    pl = spark.read.parquet(str(DWD_ROOT / "dwd_pile_log.parquet"))
    gone = not ({"old_status", "new_status"} & set(pl.columns))
    check("近乎全空列已剔除（R008）", gone,
          f"dwd_pile_log 列：{len(pl.columns)} 个，有效事件 "
          f"{pl.filter(F.col('event') != 2).count()} 条")

    # 抽样复核：每类规则取一条，人工能看懂
    sample = (dwd.filter(F.col("status") == 3)
                 .select("order_no", "status", "status_label", "amount",
                         "kwh_x100", "unit_price_fen", "charge_minutes", "is_weekend")
                 .limit(3).collect())
    print("\n  抽样（已结算单 3 条）：")
    for r in sample:
        print(f"    {r['order_no']}　{r['status_label']}　{r['amount']}分　"
              f"{r['kwh_x100']/100:.2f}度　{r['unit_price_fen']}分/度　"
              f"{r['charge_minutes']}分钟　周末={r['is_weekend']}")

    failed = [r for r in results if r["结论"] == "FAIL"]
    out = {"生成时间": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
           "检查数": len(results), "失败数": len(failed), "明细": results,
           "抽样": [r.asDict() for r in sample]}
    (QUALITY_DIR / "05_validation.json").write_text(
        json.dumps(out, ensure_ascii=False, indent=2, default=str), encoding="utf-8")

    print(f"\n{'='*46}")
    print(f"校验结果：{len(results) - len(failed)}/{len(results)} 通过")
    print("RESULT:", "PASS" if not failed else "FAIL")
    spark.stop()
    return 0 if not failed else 1


if __name__ == "__main__":
    raise SystemExit(main())

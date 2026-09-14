"""最小 SparkSession 冒烟测试。

CLAUDE.md 第 2.2 节要求：装完 PySpark 先跑通这个再写业务代码。
它只验证三件事——JVM 起得来、SparkSession 建得起、DataFrame 算得出正确结果。
不连数据库、不读 HDFS、不依赖任何项目数据。
"""
import sys

from spark_session import build_spark


def main() -> int:
    from pyspark.sql import functions as F

    # 统一入口会自动钉住 worker/driver 解释器并补 JAVA_HOME
    spark = build_spark("ecp-phase2-smoke")

    try:
        print(f"Spark 版本 : {spark.version}")
        print(f"Python     : {sys.version.split()[0]}")
        print(f"master     : {spark.sparkContext.master}")

        # 金额一律整数分（CLAUDE.md 5.2 第 7 条），这里顺带验证整数聚合不丢精度
        rows = [("SZ001", 9758), ("SZ002", 4134), ("SZ001", 10000)]
        df = spark.createDataFrame(rows, ["station", "amount_fen"])
        agg = (df.groupBy("station")
                 .agg(F.sum("amount_fen").alias("total_fen"))
                 .orderBy("station"))
        got = {r["station"]: r["total_fen"] for r in agg.collect()}

        expected = {"SZ001": 19758, "SZ002": 4134}
        assert got == expected, f"聚合结果不符：期望 {expected}，实得 {got}"
        assert all(isinstance(v, int) for v in got.values()), "金额必须保持整数"

        print(f"整数分聚合  : {got}  ✓")
        print("\nSMOKE: PASS —— SparkSession 可用，可以开始写业务代码")
        return 0
    finally:
        spark.stop()


if __name__ == "__main__":
    raise SystemExit(main())

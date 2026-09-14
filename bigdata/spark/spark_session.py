"""全项目统一的 SparkSession 入口。**所有 Spark job 一律从这里取 session。**

存在的理由是一个真实踩到的坑：
PySpark 的 driver 用当前解释器，而 worker 默认拉 PATH 里的 `python3`。
本机 PATH 上是系统 Python 3.10，venv 里是 3.11，于是每个 job 都报

    [PYTHON_VERSION_MISMATCH] Python in worker has different version (3, 10)
    than that in driver 3.11, PySpark cannot run with different minor versions.

靠 shell 里 export PYSPARK_PYTHON 也能修，但那是「每个人每次都要记得」的方案，
迟早有人忘。这里在建 session 之前用 sys.executable 把两端钉死，
无论从哪个 shell、IDE 还是 cron 拉起都一致。
"""
from __future__ import annotations

import os
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
#  ODS 原始层：本地开发是目录，答辩换成 hdfs:// 前缀即可。
#  CLAUDE.md 5.2 第 10 条——代码只认路径不认介质。
ODS_ROOT = os.environ.get("ECP_ODS_ROOT", str(REPO_ROOT / "bigdata" / "ods"))


def _pin_worker_python() -> None:
    """把 worker 与 driver 的解释器钉成同一个，必须在建 session 前调用。"""
    os.environ.setdefault("PYSPARK_PYTHON", sys.executable)
    os.environ.setdefault("PYSPARK_DRIVER_PYTHON", sys.executable)


def _ensure_java_home() -> None:
    """JAVA_HOME 缺失时自动推导，推不出来就明说，而不是让 py4j 报天书。"""
    if os.environ.get("JAVA_HOME"):
        return
    javac = os.popen("readlink -f \"$(command -v javac)\" 2>/dev/null").read().strip()
    if javac:
        os.environ["JAVA_HOME"] = str(Path(javac).parent.parent)
    else:
        print("[警告] 未找到 JDK，Spark 起不来。先跑 bash scripts/install-phase2-env.sh",
              file=sys.stderr)


def build_spark(app_name: str, *, shuffle_partitions: int = 4):
    """返回配置好的 SparkSession。

    shuffle_partitions 默认 4 而不是 Spark 默认的 200：本机 2 核、数据仅八千余行，
    200 个分区只会产生大量空任务和调度开销。真上集群时按数据量调大。
    """
    _pin_worker_python()
    _ensure_java_home()

    from pyspark.sql import SparkSession

    spark = (SparkSession.builder
             .appName(app_name)
             .master(os.environ.get("ECP_SPARK_MASTER", "local[*]"))
             .config("spark.sql.shuffle.partitions", str(shuffle_partitions))
             .config("spark.sql.session.timeZone", "Asia/Shanghai")   # 库里存的是本地时间
             .config("spark.ui.enabled", os.environ.get("ECP_SPARK_UI", "false"))
             .getOrCreate())
    spark.sparkContext.setLogLevel("ERROR")
    return spark

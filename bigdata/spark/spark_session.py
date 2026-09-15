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

#  ODS 原始层根路径：本地开发是目录，答辩是 hdfs:// URI。
#  CLAUDE.md 5.2 第 10 条——代码只认路径不认介质。
#
#  **必须是 str，不能包成 Path。** 2026-09-15 接 HDFS 时实测：
#      Path("hdfs://localhost:9000/ecp/ods") -> "hdfs:/localhost:9000/ecp/ods"
#  pathlib 会把连续斜杠折叠掉，Spark 拿到这个串当本地相对路径找，
#  报的是「Path does not exist」，完全看不出根因是被规范化了。别改回 Path。
ODS_ROOT = os.environ.get("ECP_ODS_ROOT", str(REPO_ROOT / "bigdata" / "ods")).rstrip("/")


def ods_is_remote() -> bool:
    """ODS 是否在远端文件系统（hdfs:// 等）上。file:// 与裸路径都算本地。"""
    return "://" in ODS_ROOT and not ODS_ROOT.startswith("file://")


def ods_file(name: str) -> str:
    """拼 ODS 下的文件路径。本地与 HDFS 共用一条代码路径。"""
    return f"{ODS_ROOT}/{name}"


def read_ods_text(spark, name: str) -> str:
    """读 ODS 上的小文本文件（_manifest.json），本地与 HDFS 通吃。

    HDFS 侧直接用 Hadoop 的 FileSystem API，**不能走 wholeTextFiles / spark.read.text**：
    那两个都过 FileInputFormat，而它默认把 `_` 与 `.` 开头的文件当元数据过滤掉
    （`_SUCCESS` 就是这么被忽略的）。我们的清单恰好叫 `_manifest.json`，
    于是 Spark 报「Input path does not exist」——文件明明在，hdfs dfs -cat 也读得到。
    2026-09-15 接 HDFS 时实测踩到；本地路径走 Python 原生读取，这个坑一直没露头。

    FileSystem.open 不经过 InputFormat，没有这层过滤，也顺带保住了整文件的原样字节
    （pretty-print 的 JSON 按行切了再拼回来不保证顺序）。
    """
    if not ods_is_remote():
        return Path(ods_file(name)).read_text(encoding="utf-8")

    jvm = spark.sparkContext._jvm
    uri = jvm.java.net.URI(ODS_ROOT)
    conf = spark.sparkContext._jsc.hadoopConfiguration()
    fs = jvm.org.apache.hadoop.fs.FileSystem.get(uri, conf)
    stream = fs.open(jvm.org.apache.hadoop.fs.Path(ods_file(name)))
    try:
        # commons-io 是 Spark 自带的 jar，不是本项目新引的依赖
        return jvm.org.apache.commons.io.IOUtils.toString(stream, "UTF-8")
    finally:
        stream.close()


def _pin_worker_python() -> None:
    """把 worker 与 driver 的解释器钉成同一个，必须在建 session 前调用。"""
    os.environ.setdefault("PYSPARK_PYTHON", sys.executable)
    os.environ.setdefault("PYSPARK_DRIVER_PYTHON", sys.executable)


def _pin_hdfs_identity() -> None:
    """ODS 落 HDFS 后，用非超级用户身份访问，否则 444 形同虚设。

    HDFS 的超级用户是**启动 NameNode 的那个系统账号**（本机是 bit），
    它绕过一切权限检查——本地 444 能挡住属主，HDFS 444 挡不住超级用户。
    所以 ODS 目录属主设为 ecp_ods、权限 444，分析侧固定以 ecp_analyst 身份连，
    「ODS 只读」才是 NameNode 强制的，而不是靠自觉。CLAUDE.md 5.2 第 6 条。

    与 _pin_worker_python 同理：shell 里 export 也行，但那是「每次都要记得」的方案。
    只影响 HDFS 访问身份，不影响本地 DWD 产物的写入。
    """
    if ods_is_remote():
        os.environ.setdefault("HADOOP_USER_NAME",
                              os.environ.get("ECP_HDFS_USER", "ecp_analyst"))


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
    _pin_hdfs_identity()
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

    #  把「本次读的是哪个 ODS」打进日志。不打的话，事后看输出根本分不清
    #  这一跑用的是本地目录还是 HDFS，而两者结果理应一致、更难分辨。
    where = "HDFS" if ods_is_remote() else "本地"
    who = f"　身份 {os.environ['HADOOP_USER_NAME']}" if ods_is_remote() else ""
    print(f"[ODS] {where}　{ODS_ROOT}{who}")
    return spark

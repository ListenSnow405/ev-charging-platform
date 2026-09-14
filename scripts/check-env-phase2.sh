#!/usr/bin/env bash
# =============================================================================
#  scripts/check-env-phase2.sh  —  第二阶段环境自检　归属 L5
#
#  仿第一阶段 check-env.sh 的思路：**能实际跑一下的就不只看命令在不在**。
#  基线见 CLAUDE.md 第 2.2 节。退出码 = 未通过项数。
# =============================================================================
cd "$(dirname "$0")/.."
ROOT=$(pwd)
FAIL=0
VENV="$ROOT/.venv-phase2"

ok()   { printf "  \033[32m[ok]\033[0m   %-22s %s\n" "$1" "$2"; }
bad()  { printf "  \033[31m[缺]\033[0m   %-22s %s\n" "$1" "$2"; FAIL=$((FAIL+1)); }
warn() { printf "  \033[33m[注]\033[0m   %-22s %s\n" "$1" "$2"; }

echo "== 第二阶段环境自检 =="
echo

# ---- 1. JDK：Hadoop 与 Spark 的共同前置 -------------------------------------
if command -v java >/dev/null; then
    JV=$(java -version 2>&1 | head -1)
    ok "JDK" "$JV"
    [ -z "${JAVA_HOME:-}" ] && warn "JAVA_HOME" "未设置，Spark 多数情况仍能自找；建议写进 ~/.bashrc"
else
    bad "JDK" "未安装，Hadoop 与 Spark 都起不来 → bash scripts/install-phase2-env.sh"
fi

# ---- 2. Python 3.11 ---------------------------------------------------------
if [ -x "$VENV/bin/python" ]; then
    PV=$($VENV/bin/python -V 2>&1)
    case "$PV" in
        *3.11*|*3.12*) ok "Python(二阶段)" "$PV  @ .venv-phase2" ;;
        *) bad "Python(二阶段)" "$PV —— 要求 3.11/3.12，见 CLAUDE.md 2.2" ;;
    esac
else
    bad "Python(二阶段)" "缺 .venv-phase2 → python3.11 -m venv .venv-phase2"
fi

# ---- 3. PySpark：不只看版本号，实际起一个 SparkSession ----------------------
#  这是本脚本存在的主要理由。pip list 显示装好了，但 JVM 起不来是最常见的坑，
#  只有真的建一次 session 才查得出来。
if [ -x "$VENV/bin/python" ]; then
    SPARK_V=$($VENV/bin/python -c "import pyspark;print(pyspark.__version__)" 2>/dev/null)
    if [ -n "$SPARK_V" ]; then
        # 走 bigdata/spark/spark_session.py 的统一入口：它会把 worker 与 driver 的
        # 解释器钉成同一个。不这么做必然踩 PYTHON_VERSION_MISMATCH（PATH 上是 3.10）。
        if PYTHONPATH="$ROOT/bigdata/spark" $VENV/bin/python - >/dev/null 2>&1 <<'PY'
from spark_session import build_spark
from pyspark.sql import functions as F
s = build_spark("envcheck", shuffle_partitions=2)
df = s.createDataFrame([("a", 1), ("a", 2)], ["k", "v"])
assert df.groupBy("k").agg(F.sum("v").alias("s")).collect()[0]["s"] == 3
s.stop()
PY
        then ok "PySpark" "$SPARK_V　SparkSession 实测可建"
        else bad "PySpark" "$SPARK_V 已装，但 SparkSession 起不来（多半是 JDK/JAVA_HOME）"
        fi
    else
        bad "PySpark" "未安装 → .venv-phase2/bin/pip install 'pyspark==3.5.*'"
    fi
fi

# ---- 4. Flask / MySQL 驱动 --------------------------------------------------
if [ -x "$VENV/bin/python" ]; then
    # 用 importlib.metadata 读**发行包**版本，不读模块的 __version__——
    # PyMySQL 的 __version__ 是它模拟的 MySQL 协议版本(2.2.8)，不是包版本(1.2.0)，照打会误导。
    for pair in "flask:Flask" "flask_cors:Flask-Cors" "pymysql:PyMySQL" "pandas:pandas" "pyarrow:pyarrow"; do
        mod=${pair%%:*}; dist=${pair##*:}
        V=$($VENV/bin/python -c "import importlib.metadata as m, importlib; importlib.import_module('$mod'); print(m.version('$dist'))" 2>/dev/null)
        [ -n "$V" ] && ok "py:$mod" "$V" || bad "py:$mod" "未安装"
    done
fi

# ---- 5. MySQL 服务 ----------------------------------------------------------
if command -v mysql >/dev/null; then
    ok "MySQL" "$(mysql --version | sed 's/^mysql *//')"
    systemctl is-active --quiet mysql 2>/dev/null \
        && ok "mysqld" "running" \
        || warn "mysqld" "未运行 → sudo systemctl start mysql"
else
    bad "MySQL" "未安装 → bash scripts/install-phase2-env.sh"
fi

# ---- 6. Node 23+ ------------------------------------------------------------
#  nvm 装的 node 不在非交互 shell 的 PATH 里，先把 nvm 拉起来再查。
[ -s "$HOME/.nvm/nvm.sh" ] && . "$HOME/.nvm/nvm.sh" >/dev/null 2>&1
if command -v node >/dev/null; then
    NV=$(node -v); MAJOR=${NV#v}; MAJOR=${MAJOR%%.*}
    [ "$MAJOR" -ge 23 ] 2>/dev/null \
        && ok "Node" "$NV（npm $(npm -v)）" \
        || bad "Node" "$NV —— 要求 23 及以上"
else
    bad "Node" "未安装或未加载 → . ~/.nvm/nvm.sh && nvm use default"
fi

# ---- 7. Hadoop：按老师原话，本地开发期可缺 ----------------------------------
if command -v hdfs >/dev/null; then
    ok "Hadoop" "$(hadoop version 2>/dev/null | head -1)"
else
    warn "Hadoop" "未安装。本地开发用 bigdata/ods 目录即可，**答辩前须落 HDFS**"
fi

echo
if [ "$FAIL" -eq 0 ]; then
    echo "结论：第二阶段环境齐备（Hadoop 若为 [注] 属预期，见 CLAUDE.md 2.2）"
else
    echo "结论：$FAIL 项未通过，按上面每行末尾的提示处理"
fi
exit "$FAIL"

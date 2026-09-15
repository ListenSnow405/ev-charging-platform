#!/usr/bin/env bash
# =============================================================================
#  scripts/hdfs-ctl.sh  —  单节点 HDFS 起停与状态　归属 L5
#
#  用法：bash scripts/hdfs-ctl.sh {start|stop|status|report}
#
#  **不走 start-dfs.sh，直接 hdfs --daemon start。** start-dfs.sh 是给多节点用的，
#  它要 ssh 到每台机器上拉进程，单节点也不例外——于是本机就得配 ssh 免密。
#  单节点下两者完全等价，直启不用碰 ~/.ssh，少一个与本项目无关的系统改动。
#  答辩若要求演示官方流程，配好 ssh 免密后 start-dfs.sh 一样可用，本脚本不冲突。
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."

HADOOP_HOME="${HADOOP_HOME:-$HOME/opt/hadoop}"
export HADOOP_HOME
HDFS="$HADOOP_HOME/bin/hdfs"

ok()  { printf "  \033[32m[ok]\033[0m   %-14s %s\n" "$1" "$2"; }
bad() { printf "  \033[31m[×]\033[0m    %-14s %s\n" "$1" "$2"; }

if [ ! -x "$HDFS" ]; then
    bad "Hadoop" "未找到 $HDFS → 先跑 bash scripts/install-hadoop-phase2.sh"
    exit 1
fi

#  判断守护进程是否在跑：用 jps 按类名找，不用 pgrep 按命令行找。
#  ——T7 的教训：pgrep -f 会匹配到发起检查的进程自己，恒为假阳性。
#  jps 列的是 JVM 主类，本脚本是 bash，不可能混进去。
JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"
JPS="$(command -v jps || echo "$JAVA_HOME/bin/jps")"
running() { "$JPS" 2>/dev/null | grep -qw "$1"; }

case "${1:-status}" in
start)
    echo "== 启动 HDFS =="
    for d in namenode datanode; do
        cls=$([ "$d" = namenode ] && echo NameNode || echo DataNode)
        if running "$cls"; then
            ok "$d" "已在运行"
        else
            "$HDFS" --daemon start "$d"
            sleep 3
            running "$cls" && ok "$d" "已启动" \
                || { bad "$d" "启动失败 → 看 $HADOOP_HOME/logs/hadoop-$USER-$d-*.log"; exit 1; }
        fi
    done
    echo "-- 等待退出安全模式"
    "$HDFS" dfsadmin -safemode wait >/dev/null && ok "safemode" "已退出，可读写"
    echo
    echo "  NameNode Web UI  http://localhost:9870"
    ;;
stop)
    echo "== 停止 HDFS =="
    for d in datanode namenode; do
        "$HDFS" --daemon stop "$d" 2>/dev/null && ok "$d" "已停止" || bad "$d" "未在运行"
    done
    ;;
status)
    echo "== HDFS 状态 =="
    running NameNode && ok "NameNode" "运行中" || bad "NameNode" "未运行"
    running DataNode && ok "DataNode" "运行中" || bad "DataNode" "未运行"
    ;;
report)
    "$HDFS" dfsadmin -report
    ;;
*)
    echo "用法：bash scripts/hdfs-ctl.sh {start|stop|status|report}"; exit 2 ;;
esac

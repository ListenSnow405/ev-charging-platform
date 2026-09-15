#!/usr/bin/env bash
# =============================================================================
#  scripts/install-hadoop-phase2.sh  —  Hadoop 3.x 伪分布式安装与配置　归属 L5
#
#  目标：把 ODS 原始层从本地目录搬到 HDFS（CLAUDE.md 5.2 第 6/10 条、
#  PHASE2-PLAN 第 13 节唯一未达标项）。**只装 HDFS，不启 YARN**——
#  Spark 跑 local[*]，资源调度用不上 YARN，多起两个守护进程只是多两处会崩的地方。
#
#  三个刻意的选择：
#   · 装到 $HOME/opt，**不需要 root**。本机 sudo 需要密码，且单节点 HDFS
#     本就没有跨用户共享的需求，装进系统目录只会让清理变麻烦。
#   · 版本 3.3.6：PySpark 3.5.9 自带的是 hadoop-client-api **3.3.4**，
#     服务端取同一条 3.3 线最稳。HDFS RPC 在 3.x 内兼容，但没必要去赌。
#   · JDK 17 需要 --add-opens：Hadoop 3.3.x 官方支持到 JDK 11，
#     在 17 上跑 NameNode 会撞 InaccessibleObjectException。下面显式放开。
#
#  幂等：重复跑只覆盖配置，不会重新格式化已有的 NameNode。
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

VER="${ECP_HADOOP_VERSION:-3.3.6}"
PREFIX="${ECP_HADOOP_PREFIX:-$HOME/opt}"
HADOOP_HOME="$PREFIX/hadoop-$VER"
DATA_ROOT="${ECP_HADOOP_DATA:-$PREFIX/hadoop-data}"
#  镜像实测（2026-09-15）：阿里云 13MB/s、BFSU 15MB/s、清华仅 0.3MB/s。
#  清华的 pypi 源很快但 apache 这条线慢，别照搬 bigdata/requirements 的经验。
MIRROR="${ECP_HADOOP_MIRROR:-https://mirrors.aliyun.com/apache/hadoop/common}"
JAVA_HOME_DETECTED="${JAVA_HOME:-$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")}"

echo "== Hadoop $VER 伪分布式安装 =="
echo "   HADOOP_HOME = $HADOOP_HOME"
echo "   数据目录     = $DATA_ROOT"
echo "   JAVA_HOME   = $JAVA_HOME_DETECTED"
echo

# ---- 1. 取包 ----------------------------------------------------------------
TGZ="${ECP_HADOOP_TGZ:-$PREFIX/dl/hadoop-$VER.tar.gz}"
if [ ! -f "$TGZ" ]; then
    mkdir -p "$(dirname "$TGZ")"
    echo "-- 下载 hadoop-$VER.tar.gz（约 700MB，境内走清华镜像）"
    curl -# -o "$TGZ" "$MIRROR/hadoop-$VER/hadoop-$VER.tar.gz"
fi
echo "-- 安装包 $(du -h "$TGZ" | cut -f1)  $TGZ"

# ---- 2. 解包 ----------------------------------------------------------------
if [ ! -d "$HADOOP_HOME" ]; then
    mkdir -p "$PREFIX"
    echo "-- 解包到 $PREFIX"
    tar -xzf "$TGZ" -C "$PREFIX"
else
    echo "-- $HADOOP_HOME 已存在，跳过解包"
fi
ln -sfn "$HADOOP_HOME" "$PREFIX/hadoop"

# ---- 3. 配置 ----------------------------------------------------------------
ETC="$HADOOP_HOME/etc/hadoop"
mkdir -p "$DATA_ROOT/name" "$DATA_ROOT/data" "$DATA_ROOT/tmp"

#  hadoop-env.sh：JAVA_HOME + JDK17 的 --add-opens。
#  用标记块包起来，重复跑先删旧块再追加，避免越堆越长。
sed -i '/# >>> ecp-phase2 >>>/,/# <<< ecp-phase2 <<</d' "$ETC/hadoop-env.sh"
cat >> "$ETC/hadoop-env.sh" <<EOF
# >>> ecp-phase2 >>>
export JAVA_HOME=$JAVA_HOME_DETECTED
#  Hadoop 3.3.x 官方支持 JDK 8/11；本机是 JDK 17，强封装会挡住它的反射调用，
#  NameNode 启动即抛 InaccessibleObjectException。放开这几个包即可，
#  换 JDK 11 也能解决，但那要 root 装包。
export HADOOP_OPTS="\${HADOOP_OPTS:-} \\
  --add-opens java.base/java.lang=ALL-UNNAMED \\
  --add-opens java.base/java.lang.reflect=ALL-UNNAMED \\
  --add-opens java.base/java.io=ALL-UNNAMED \\
  --add-opens java.base/java.net=ALL-UNNAMED \\
  --add-opens java.base/java.nio=ALL-UNNAMED \\
  --add-opens java.base/java.util=ALL-UNNAMED \\
  --add-opens java.base/java.util.concurrent=ALL-UNNAMED \\
  --add-opens java.base/sun.nio.ch=ALL-UNNAMED"
# <<< ecp-phase2 <<<
EOF

cat > "$ETC/core-site.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<?xml-stylesheet type="text/xsl" href="configuration.xsl"?>
<!-- 第二阶段 ODS 原始层 · 单节点伪分布式。由 scripts/install-hadoop-phase2.sh 生成 -->
<configuration>
  <property>
    <name>fs.defaultFS</name>
    <value>hdfs://localhost:9000</value>
  </property>
  <property>
    <name>hadoop.tmp.dir</name>
    <value>$DATA_ROOT/tmp</value>
    <!-- 不用 /tmp：重启会被清，NameNode 元数据跟着没 -->
  </property>
  <property>
    <name>hadoop.http.staticuser.user</name>
    <value>$USER</value>
  </property>
</configuration>
EOF

cat > "$ETC/hdfs-site.xml" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<?xml-stylesheet type="text/xsl" href="configuration.xsl"?>
<configuration>
  <property>
    <name>dfs.replication</name>
    <value>1</value>
    <!-- 单节点，副本只能是 1；默认 3 会让每个块永远处于 under-replicated -->
  </property>
  <property>
    <name>dfs.namenode.name.dir</name>
    <value>file://$DATA_ROOT/name</value>
  </property>
  <property>
    <name>dfs.datanode.data.dir</name>
    <value>file://$DATA_ROOT/data</value>
  </property>
  <property>
    <name>dfs.permissions.enabled</name>
    <value>true</value>
    <!-- 必须开：ODS 的 444 只读靠它强制。关掉则 CLAUDE.md 5.2 第 6 条形同虚设 -->
  </property>
</configuration>
EOF
echo "-- 配置已写入 $ETC/{hadoop-env.sh,core-site.xml,hdfs-site.xml}"

# ---- 4. 格式化 NameNode（仅首次）--------------------------------------------
if [ -f "$DATA_ROOT/name/current/VERSION" ]; then
    echo "-- NameNode 已格式化过，跳过（重复格式化会让 DataNode 的 clusterID 对不上）"
else
    echo "-- 首次格式化 NameNode"
    "$HADOOP_HOME/bin/hdfs" namenode -format -force -nonInteractive >/dev/null 2>&1 \
        && echo "   格式化完成" || { echo "   [错误] 格式化失败，用下面命令看详情："; \
           echo "   $HADOOP_HOME/bin/hdfs namenode -format -force"; exit 1; }
fi

echo
echo "安装完成。下一步："
echo "  bash scripts/hdfs-ctl.sh start     # 起 NameNode + DataNode"
echo "  bash scripts/ods-to-hdfs.sh        # 推 ODS 上去并校验"

#!/usr/bin/env bash
# =============================================================================
#  scripts/install-phase2-env.sh  —  第二阶段需要 root 权限的那部分环境
#  归属 L5。只装必须 sudo 的三样：JDK 17 / Python 3.11 / MySQL。
#  PySpark、Node、Hadoop 不在此脚本内——它们能在用户空间装，不需要 root。
#
#  用法：bash scripts/install-phase2-env.sh
#  版本依据：CLAUDE.md 第 2.2 节。Python 3.11 而非 3.12，因为 PySpark 3.5.x
#  的 PyPI classifiers 只声明支持到 3.11（实测，非记忆）。
# =============================================================================
set -euo pipefail

echo "==> 本脚本需要 sudo，会提示输入密码"
sudo -v

# ---- 1. JDK 17：Hadoop 与 Spark 的共同前置 ----------------------------------
echo "==> [1/3] 安装 OpenJDK 17"
sudo apt-get update -qq
sudo apt-get install -y openjdk-17-jdk-headless

# ---- 2. Python 3.11：系统自带 3.10 不达标；apt 源里的 python3.11 是 RC 版 ----
echo "==> [2/3] 通过 deadsnakes PPA 安装 Python 3.11 稳定版"
sudo apt-get install -y software-properties-common
sudo add-apt-repository -y ppa:deadsnakes/ppa
sudo apt-get update -qq
sudo apt-get install -y python3.11 python3.11-venv python3.11-dev

# ---- 3. MySQL：第二阶段的分析结果层 -----------------------------------------
echo "==> [3/3] 安装 MySQL Server"
sudo apt-get install -y mysql-server
sudo systemctl enable --now mysql || true

# ---- 验证 -------------------------------------------------------------------
echo
echo "================ 安装结果 ================"
printf "%-14s %s\n" "java"      "$(java -version 2>&1 | head -1)"
printf "%-14s %s\n" "javac"     "$(javac -version 2>&1)"
printf "%-14s %s\n" "python3.11" "$(python3.11 -V 2>&1)"
printf "%-14s %s\n" "mysql"     "$(mysql --version 2>&1)"
printf "%-14s %s\n" "mysqld"    "$(systemctl is-active mysql 2>/dev/null || echo '未运行')"
echo "=========================================="
echo
echo "JAVA_HOME 建议值（写进 ~/.bashrc）："
echo "  export JAVA_HOME=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
echo
echo "完成。回到 agent 会话继续下一步（PySpark / Node / Hadoop，均无需 root）。"

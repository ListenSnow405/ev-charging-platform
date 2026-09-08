#!/usr/bin/env bash
# =============================================================================
#  scripts/test-carbon-calc.sh  —  扩展模块 08 计算核心手算夹具
#  归属 L5（模块 08 认领人）
#
#  单独编译 server/biz/ext_08_carbon_calc.cpp 与它的测试，不依赖服务端产物、
#  不连数据库、不起服务、不碰 charging.db —— 改完算法先跑它，秒级出结果。
#
#  用法：bash scripts/test-carbon-calc.sh
#  退出码：0 = 全过，1 = 有断言失败
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

command -v qmake6 >/dev/null || { echo "缺少 qmake6，先跑 bash scripts/check-env.sh"; exit 1; }
QT_INC=$(qmake6 -query QT_INSTALL_HEADERS)
QT_LIB=$(qmake6 -query QT_INSTALL_LIBS)

OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT

g++ -std=c++17 -fPIC -O1 -Wall -Wextra \
    -I"$QT_INC" -I"$QT_INC/QtCore" -I"$ROOT/server/biz" \
    "$ROOT/server/biz/ext_08_carbon_calc.cpp" \
    "$ROOT/server/biz/ext_08_carbon_calc_test.cpp" \
    -o "$OUT/carbon-calc-test" \
    -L"$QT_LIB" -lQt6Core

"$OUT/carbon-calc-test"

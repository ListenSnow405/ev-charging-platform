#!/usr/bin/env bash
# =============================================================================
#  scripts/check-env.sh  —  开发环境自检
#
#  属主 SCML（L5）。用法：
#      bash scripts/check-env.sh          # 检查全部
#      bash scripts/check-env.sh L3       # 只检查 L3 这条线需要的
#
#  在群里回报时，把本脚本最后的「结论」一行贴出来即可。
#  依据：docs/conventions.md 第 5 节 环境基线
# =============================================================================
set -uo pipefail

LINE="${1:-ALL}"
EXPECT_QT="6.2.4"
FAIL=0
NEED=()

c_ok()   { printf "  \033[32m✓\033[0m %s\n" "$1"; }
c_bad()  { printf "  \033[31m✗\033[0m %s\n" "$1"; FAIL=$((FAIL+1)); }
c_warn() { printf "  \033[33m!\033[0m %s\n" "$1"; }
head_()  { printf "\n\033[1m%s\033[0m\n" "$1"; }

head_ "1. 基础工具"
for t in qmake6 g++ make python3; do
  if command -v "$t" >/dev/null 2>&1; then c_ok "$t  ($(command -v $t))"
  else c_bad "$t 未安装"; NEED+=("qt6-base-dev-tools g++ make python3"); fi
done
if command -v sqlite3 >/dev/null 2>&1; then c_ok "sqlite3  ($(sqlite3 --version | cut -d' ' -f1))"
else c_warn "sqlite3 命令行未装（不影响编译，但建库调试方便，建议装：sudo apt install sqlite3）"; fi

head_ "2. Qt 版本"
if command -v qmake6 >/dev/null 2>&1; then
  QV=$(qmake6 -query QT_VERSION)
  if [ "$QV" = "$EXPECT_QT" ]; then c_ok "Qt $QV  （与全组基线一致）"
  else c_bad "Qt $QV  —— 全组基线为 $EXPECT_QT，版本不一致会导致编译差异，请在群里说明"; fi
  QH=$(qmake6 -query QT_INSTALL_HEADERS); QL=$(qmake6 -query QT_INSTALL_LIBS)
else
  c_bad "qmake6 不可用，后续检查跳过"; echo; echo "结论：环境不完整，请先装 qt6-base-dev"; exit 1
fi

# 模块名 | 需要它的线 | 用途 | 安装包
MODS=(
  "QtCore|ALL|基础|qt6-base-dev"
  "QtNetwork|L1 L3 L4|Socket 通信|qt6-base-dev"
  "QtSql|L2|QSQLite 访问|qt6-base-dev"
  "QtWidgets|L3 L4|界面控件|qt6-base-dev"
  "QtCharts|L3|[说明书] 营收趋势折线图 QChart|libqt6charts6-dev"
  "QtWebEngineWidgets|L4|[说明书] 一键导航 QWebEngineView|qt6-webengine-dev qt6-webengine-dev-tools"
)

head_ "3. Qt 模块（头文件 = 能否编译）"
for row in "${MODS[@]}"; do
  IFS='|' read -r mod lines usage pkg <<< "$row"
  if [ "$LINE" != "ALL" ] && [ "$lines" != "ALL" ] && [[ " $lines " != *" $LINE "* ]]; then continue; fi
  if [ -d "$QH/$mod" ]; then c_ok "$mod  —— $usage"
  else c_bad "$mod 缺失  —— $usage  （需要它的线：$lines）"; NEED+=("$pkg"); fi
done

head_ "4. QSQLite 驱动"
DRV="$(qmake6 -query QT_INSTALL_PLUGINS)/sqldrivers"
if [ -f "$DRV/libqsqlite.so" ]; then c_ok "libqsqlite.so 存在  —— [说明书] 1.6 要求的 QSQLite"
else c_bad "libqsqlite.so 缺失，QSQLite 无法使用"; NEED+=("qt6-base-dev"); fi

head_ "5. 试编译（最终判定：头文件在不代表链接得上）"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
try_build() {                     # $1=模块 $2=QT模块名 $3=测试代码
  local mod="$1" qtmod="$2" code="$3"
  [ -d "$QH/$mod" ] || { c_warn "$mod 未安装，跳过试编译"; return; }
  mkdir -p "$TMP/$mod" && cd "$TMP/$mod"
  printf '%s\n' "$code" > main.cpp
  printf 'TEMPLATE=app\nTARGET=t\nCONFIG+=c++17\nQT+=%s\nSOURCES+=main.cpp\n' "$qtmod" > t.pro
  if qmake6 t.pro >/dev/null 2>&1 && make >/dev/null 2>&1; then c_ok "$mod 试编译通过"
  else c_bad "$mod 试编译失败 —— 头文件在但链接不上，多半只装了运行库没装 -dev 包"; fi
  cd - >/dev/null
}
if [ "$LINE" = "ALL" ] || [ "$LINE" = "L3" ]; then
  try_build QtCharts "core gui widgets charts" \
'#include <QtCharts/QChart>
#include <QtCharts/QLineSeries>
int main(){ QChart c; QLineSeries s; s.append(0,1); c.addSeries(&s); return 0; }'
fi
if [ "$LINE" = "ALL" ] || [ "$LINE" = "L4" ]; then
  try_build QtWebEngineWidgets "core gui widgets webenginewidgets" \
'#include <QtWebEngineWidgets/QWebEngineView>
int main(){ return 0; }'
fi

head_ "结论"
if [ "$FAIL" -eq 0 ]; then
  printf "  \033[32m环境检查全部通过\033[0m（检查范围：%s）\n" "$LINE"
else
  printf "  \033[31m有 %d 项未通过\033[0m（检查范围：%s）\n" "$FAIL" "$LINE"
  UNIQ=$(printf '%s\n' "${NEED[@]}" | tr ' ' '\n' | sort -u | tr '\n' ' ')
  printf "\n  安装命令：\n    sudo apt update && sudo apt install %s\n" "$UNIQ"
  printf "\n  装完重新运行本脚本确认。\n"
fi
echo
exit "$FAIL"

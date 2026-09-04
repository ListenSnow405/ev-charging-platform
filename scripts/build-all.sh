#!/usr/bin/env bash
# =============================================================================
#  scripts/build-all.sh  —  一键构建全部 C++ 子工程
#  归属 L3（集成与构建，第二顶帽子）
#
#  用法：bash scripts/build-all.sh [clean]
#  产物：build/bin/{ecp-server, ecp-admin, ecp-user, ecp-pile-sim}
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

if [ "${1:-}" = "clean" ]; then
    rm -rf build; echo "已清理 build/"; exit 0
fi

command -v qmake6 >/dev/null || { echo "缺少 qmake6，先跑 bash scripts/check-env.sh"; exit 1; }

mkdir -p build/all && cd build/all
qmake6 "$ROOT/ev-charging-platform.pro" > /dev/null
make -j"$(nproc)" -s

echo
echo "构建完成，产物在 build/bin/："
ls -1 "$ROOT/build/bin/" 2>/dev/null | sed 's/^/  /'

# 数据库
cd "$ROOT"
if [ ! -f charging.db ]; then
    echo
    echo "数据库尚未生成，正在建库…"
    if command -v sqlite3 >/dev/null; then
        sqlite3 charging.db < docs/db-schema.sql
    else
        python3 -c "import sqlite3,pathlib;c=sqlite3.connect('charging.db');c.executescript(pathlib.Path('docs/db-schema.sql').read_text(encoding='utf-8'));c.commit()"
    fi
    echo "  已生成 charging.db"
fi
[ -f config/app.ini ] || { cp config/app.ini.example config/app.ini; echo "  已生成 config/app.ini"; }

cat <<'TIP'

启动方式：
  ./build/bin/ecp-server                 # 服务端（先启动）
  ./build/bin/ecp-admin                  # PC 管理端
  ./build/bin/ecp-user                   # 充电用户端
  ./build/bin/ecp-pile-sim SZ001-01      # 电桩模拟器
  python3 ml/export_snapshot.py && python3 -m http.server 8080 -d dataviz   # 大屏

管理端协议 smoke（先启动服务端）：
  python3 scripts/smoke-admin.py
TIP

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

# 本地运行数据。隔离式集成测试只构建产物，不接触真实 DB/config。
cd "$ROOT"
if [ "${ECP_BUILD_ONLY:-0}" != "1" ]; then
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

    # ---- B3 · 扩展模块建表脚本（docs/expand/00 第 4.4 节）-----------------------
    # 每次构建都执行，不只在首次建库时执行：
    # 脚本本身要求幂等（只允许 CREATE TABLE IF NOT EXISTS / CREATE INDEX IF NOT EXISTS
    # / INSERT OR IGNORE，禁止 DROP），所以重复跑是安全的；而只在建库时跑一次的话，
    # 别人拉到新的 ext 脚本后不会自动建表，会以「表不存在」的形式在运行期才暴露。
    # 按文件名排序执行，保证模块编号顺序稳定。
    shopt -s nullglob
    EXT_SQL=(docs/db-schema-ext-*.sql)
    shopt -u nullglob
    if [ ${#EXT_SQL[@]} -gt 0 ]; then
        echo
        echo "执行扩展模块建表脚本（${#EXT_SQL[@]} 个）…"
        for f in "${EXT_SQL[@]}"; do
            if command -v sqlite3 >/dev/null; then
                sqlite3 charging.db < "$f"
            else
                python3 -c "import sqlite3,pathlib,sys;c=sqlite3.connect('charging.db');c.executescript(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8'));c.commit()" "$f"
            fi
            echo "  ok  $f"
        done
    fi
fi

cat <<'TIP'

启动方式：
  ./build/bin/ecp-server                 # 服务端（先启动）
  ./build/bin/ecp-admin                  # PC 管理端
  ./build/bin/ecp-user                   # 充电用户端
  ./build/bin/ecp-pile-sim SZ001-01      # 电桩模拟器
  python3 ml/export_snapshot.py && python3 -m http.server 8080 -d dataviz   # 大屏

管理端协议 smoke（先启动服务端）：
  python3 scripts/smoke-admin.py

一键管理端集成测试：
  bash scripts/test-admin-integration.sh
TIP

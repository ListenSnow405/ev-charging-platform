#!/usr/bin/env bash
# =============================================================================
#  scripts/test-carbon-integration.sh  —  扩展模块 08 隔离式协议集成测试
#  归属 L3（集成与构建，第二顶帽子）
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

TMP_DIR=""
SERVER_PID=""
SERVER_LOG=""

apply_sql_file()
{
    local database="$1"
    local sql_file="$2"

    if command -v sqlite3 >/dev/null; then
        sqlite3 "$database" < "$sql_file"
    else
        python3 - "$sql_file" "$database" <<'PY'
import pathlib
import sqlite3
import sys

schema_path = pathlib.Path(sys.argv[1])
database_path = pathlib.Path(sys.argv[2])
connection = sqlite3.connect(database_path)
try:
    connection.executescript(schema_path.read_text(encoding="utf-8"))
    connection.commit()
finally:
    connection.close()
PY
    fi
}

core_order_snapshot()
{
    python3 - "$1" <<'PY'
import sqlite3
import sys

with sqlite3.connect(sys.argv[1]) as connection:
    row = connection.execute(
        "SELECT COUNT(*),"
        " SUM(CASE WHEN status = 3 THEN 1 ELSE 0 END),"
        " COALESCE(SUM(CASE WHEN status = 3 THEN kwh_x100 ELSE 0 END), 0)"
        " FROM t_order"
    ).fetchone()
print("|".join(str(value) for value in row))
PY
}

cleanup()
{
    local status="${1:-$?}"
    trap - EXIT INT TERM

    if [ -n "$SERVER_PID" ]; then
        if kill -0 "$SERVER_PID" 2>/dev/null; then
            kill "$SERVER_PID" 2>/dev/null || true
        fi
        wait "$SERVER_PID" 2>/dev/null || true
    fi

    if [ "$status" -ne 0 ]; then
        if [ -n "$SERVER_LOG" ] && [ -s "$SERVER_LOG" ]; then
            echo "----- server.log -----"
            cat "$SERVER_LOG"
        fi
        echo "CARBON INTEGRATION: FAIL"
    fi

    if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then
        rm -rf -- "$TMP_DIR"
    fi
    exit "$status"
}

trap 'cleanup $?' EXIT
trap 'cleanup 130' INT
trap 'cleanup 143' TERM

echo "ECP carbon integration test"
echo "building project without touching repository DB/config..."
ECP_BUILD_ONLY=1 bash scripts/build-all.sh

TMP_DIR=$(mktemp -d)
TEST_DB="$TMP_DIR/charging-carbon-test.db"
TEST_CONFIG="$TMP_DIR/app-carbon-test.ini"
SERVER_LOG="$TMP_DIR/server.log"

echo "creating isolated database..."
apply_sql_file "$TEST_DB" "$ROOT/docs/db-schema.sql"

shopt -s nullglob
EXT_SQL=("$ROOT"/docs/db-schema-ext-*.sql)
shopt -u nullglob
if [ ${#EXT_SQL[@]} -eq 0 ]; then
    echo "[FAIL] no extension schema found"
    exit 1
fi

echo "applying extension schemas (${#EXT_SQL[@]} files)..."
for file in "${EXT_SQL[@]}"; do
    apply_sql_file "$TEST_DB" "$file"
    echo "  ok ${file#"$ROOT"/}"
done

echo "inserting deterministic order fixtures..."
python3 - "$TEST_DB" <<'PY'
import sqlite3
import sys

database = sys.argv[1]
with sqlite3.connect(database) as connection:
    connection.execute("PRAGMA foreign_keys = ON")
    user = connection.execute(
        "SELECT user_id FROM t_user WHERE status = 0 ORDER BY user_id LIMIT 1"
    ).fetchone()
    piles = connection.execute(
        "SELECT p.pile_id, p.station_id, s.price"
        " FROM t_pile AS p JOIN t_station AS s ON s.station_id = p.station_id"
        " ORDER BY p.station_id, p.pile_id"
    ).fetchall()

    if user is None:
        raise SystemExit("fixture setup failed: no normal seed user")
    if not piles:
        raise SystemExit("fixture setup failed: no seed pile")

    first_pile = piles[0]
    second_pile = next(
        (pile for pile in piles if pile[1] != first_pile[1]),
        None,
    )
    if second_pile is None:
        raise SystemExit("fixture setup failed: fewer than two seed stations")

    user_id = user[0]

    def amount_fen(kwh_x100, price_fen):
        return (kwh_x100 * price_fen + 50) // 100

    fixtures = [
        (
            "IT-CARBON-SETTLED-A",
            user_id,
            first_pile[0],
            first_pile[1],
            3,
            first_pile[2],
            1234,
            amount_fen(1234, first_pile[2]),
            "2026-09-01 08:30:00",
            "2026-09-01 09:00:00",
            "2026-09-01 11:00:00",
            "2026-09-01 11:05:00",
        ),
        (
            "IT-CARBON-SETTLED-B",
            user_id,
            first_pile[0],
            first_pile[1],
            3,
            first_pile[2],
            2345,
            amount_fen(2345, first_pile[2]),
            "2026-09-02 17:30:00",
            "2026-09-02 18:30:00",
            "2026-09-02 20:00:00",
            "2026-09-02 20:05:00",
        ),
        (
            "IT-CARBON-SETTLED-C",
            user_id,
            second_pile[0],
            second_pile[1],
            3,
            second_pile[2],
            3456,
            amount_fen(3456, second_pile[2]),
            "2026-09-02 05:30:00",
            "2026-09-02 06:00:00",
            "2026-09-02 08:00:00",
            "2026-09-02 08:05:00",
        ),
        (
            "IT-CARBON-CANCELLED-D",
            user_id,
            second_pile[0],
            second_pile[1],
            4,
            second_pile[2],
            9876,
            amount_fen(9876, second_pile[2]),
            "2026-09-02 12:00:00",
            None,
            None,
            None,
        ),
    ]

    connection.executemany(
        "INSERT INTO t_order"
        " (order_no, user_id, pile_id, station_id, status, price, kwh_x100,"
        " amount, reserve_time, start_time, end_time, settle_time)"
        " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        fixtures,
    )
    connection.commit()

print(
    "fixture ready: 4 orders (3 settled, 1 cancelled), "
    "2 dates, 2 stations"
)
PY

CORE_BEFORE=$(core_order_snapshot "$TEST_DB")
echo "core order snapshot before smoke: $CORE_BEFORE"

TEST_PORT=$(python3 - <<'PY'
import socket

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)

cat > "$TEST_CONFIG" <<EOF
[server]
host = 127.0.0.1
port = $TEST_PORT
pool_size = 4
db = $TEST_DB
EOF

echo "starting isolated server on 127.0.0.1:$TEST_PORT..."
./build/bin/ecp-server "$TEST_CONFIG" >"$SERVER_LOG" 2>&1 &
SERVER_PID=$!

SERVER_READY=0
for ((attempt = 1; attempt <= 50; ++attempt)); do
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "[FAIL] isolated server exited before becoming ready"
        exit 1
    fi

    if python3 - "$TEST_PORT" <<'PY'
import socket
import sys

try:
    with socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=0.1):
        pass
except OSError:
    sys.exit(1)
PY
    then
        SERVER_READY=1
        break
    fi
    sleep 0.2
done

if [ "$SERVER_READY" -ne 1 ]; then
    echo "[FAIL] isolated server did not become ready"
    exit 1
fi

python3 scripts/smoke-carbon.py \
    --host 127.0.0.1 \
    --port "$TEST_PORT" \
    --db "$TEST_DB"

CORE_AFTER=$(core_order_snapshot "$TEST_DB")
echo "core order snapshot after smoke:  $CORE_AFTER"
if [ "$CORE_BEFORE" != "$CORE_AFTER" ]; then
    echo "[FAIL] core order facts changed: $CORE_BEFORE -> $CORE_AFTER"
    exit 1
fi

echo "[PASS] core order facts unchanged"
echo "CARBON INTEGRATION: PASS"

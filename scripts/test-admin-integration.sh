#!/usr/bin/env bash
# =============================================================================
#  scripts/test-admin-integration.sh  —  隔离式管理端协议集成测试
#  归属 L3（集成与构建，第二顶帽子）
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

TMP_DIR=""
SERVER_PID=""

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

    if [ -n "$TMP_DIR" ] && [ -d "$TMP_DIR" ]; then
        rm -rf -- "$TMP_DIR"
    fi
    exit "$status"
}

trap 'cleanup $?' EXIT
trap 'cleanup 130' INT
trap 'cleanup 143' TERM

echo "ECP admin integration test"
echo "building project..."
ECP_BUILD_ONLY=1 bash scripts/build-all.sh

TMP_DIR=$(mktemp -d)
TEST_DB="$TMP_DIR/charging-test.db"
TEST_CONFIG="$TMP_DIR/app-test.ini"
SERVER_LOG="$TMP_DIR/server.log"

if command -v sqlite3 >/dev/null; then
    sqlite3 "$TEST_DB" < "$ROOT/docs/db-schema.sql"
else
    python3 - "$ROOT/docs/db-schema.sql" "$TEST_DB" <<'PY'
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
        echo "----- server.log -----"
        cat "$SERVER_LOG"
        echo "ADMIN INTEGRATION: FAIL"
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
    echo "[FAIL] server did not become ready"
    echo "----- server.log -----"
    cat "$SERVER_LOG"
    echo "ADMIN INTEGRATION: FAIL"
    exit 1
fi

if python3 scripts/smoke-admin.py \
    --host 127.0.0.1 \
    --port "$TEST_PORT"
then
    echo "ADMIN INTEGRATION: PASS"
else
    smoke_status=$?
    echo "----- server.log -----"
    cat "$SERVER_LOG"
    echo "ADMIN INTEGRATION: FAIL"
    exit "$smoke_status"
fi

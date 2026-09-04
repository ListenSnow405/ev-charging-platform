#!/usr/bin/env python3
"""Read-only protocol smoke test for the ECP admin server APIs.

No business mutation command is sent. The required 2001 login may update the
server-managed t_admin.last_login audit timestamp.
"""

import configparser
import json
import os
from pathlib import Path
import socket
import struct
import sys


DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9527
SOCKET_TIMEOUT_SECONDS = 5
FRAME_MAX_PAYLOAD = 1024 * 1024

ERR_OK = 0
ERR_NOT_LOGIN = 1002

CMD_ADMIN_LOGIN = 2001
CMD_STATION_LIST = 2101
CMD_STATION_DETAIL = 2103
CMD_PILE_LIST = 2111
CMD_ADMIN_USER_LIST = 2201
CMD_STAT_REVENUE = 2301
CMD_STAT_REVENUE_TREND = 2302
CMD_STAT_PILE_STATUS = 2303
CMD_ADMIN_ORDER_LIST = 2304


class SmokeFailure(Exception):
    """A failed protocol or response contract assertion."""


def recv_exact(sock, size):
    chunks = []
    remaining = size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise SmokeFailure(
                f"connection closed while receiving frame: expected {size} bytes"
            )
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_request(sock, cmd, seq, token, data):
    envelope = {
        "cmd": cmd,
        "seq": seq,
        "token": token,
        "data": data,
    }
    payload = json.dumps(
        envelope, ensure_ascii=False, separators=(",", ":")
    ).encode("utf-8")
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise SmokeFailure(
            f"cmd {cmd}: request payload exceeds {FRAME_MAX_PAYLOAD} bytes"
        )
    sock.sendall(struct.pack("!I", len(payload)) + payload)


def recv_response(sock):
    header = recv_exact(sock, 4)
    (length,) = struct.unpack("!I", header)
    if length > FRAME_MAX_PAYLOAD:
        raise SmokeFailure(
            f"response frame length {length} exceeds {FRAME_MAX_PAYLOAD} bytes"
        )
    body = recv_exact(sock, length)
    try:
        response = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SmokeFailure(f"invalid UTF-8 JSON response: {exc}") from exc
    if not isinstance(response, dict):
        raise SmokeFailure(
            f"response envelope: expected object, got {type_name(response)}"
        )
    return response


def is_integer(value):
    return isinstance(value, int) and not isinstance(value, bool)


def is_number(value):
    return (isinstance(value, (int, float))
            and not isinstance(value, bool))


def type_name(value):
    if value is None:
        return "null"
    return type(value).__name__


def fail(cmd, expected, actual):
    raise SmokeFailure(f"cmd {cmd}: expected {expected}; actual {actual}")


def require(condition, cmd, expected, actual):
    if not condition:
        fail(cmd, expected, actual)


def require_field(data, key, predicate, expected_type, cmd):
    require(key in data, cmd, f"data.{key} to exist", "field is missing")
    value = data[key]
    require(
        predicate(value),
        cmd,
        f"data.{key} to be {expected_type}",
        type_name(value),
    )
    return value


def require_nonnegative_integer(data, key, cmd):
    value = require_field(data, key, is_integer, "an integer", cmd)
    require(value >= 0, cmd, f"data.{key} >= 0", value)
    return value


def require_string(data, key, cmd, nonempty=False):
    value = require_field(data, key, lambda item: isinstance(item, str), "a string", cmd)
    if nonempty:
        require(bool(value), cmd, f"data.{key} to be non-empty", "empty string")
    return value


def require_list_result(response, cmd):
    data = response["data"]
    total = require_nonnegative_integer(data, "total", cmd)
    items = require_field(data, "list", lambda item: isinstance(item, list), "an array", cmd)
    require(
        total >= len(items),
        cmd,
        "data.total >= returned list length",
        f"total={total}, list length={len(items)}",
    )
    return total, items


class SmokeClient:
    def __init__(self, sock):
        self.sock = sock
        self.seq = 0

    def request(self, cmd, token, data):
        self.seq += 1
        request_seq = self.seq
        try:
            send_request(self.sock, cmd, request_seq, token, data)
            response = recv_response(self.sock)
        except (SmokeFailure, OSError) as exc:
            raise SmokeFailure(f"cmd {cmd}: transport/frame failure: {exc}") from exc

        response_cmd = response.get("cmd")
        response_seq = response.get("seq")
        require(
            is_integer(response_cmd) and response_cmd == cmd,
            cmd,
            f"response cmd == {cmd}",
            response_cmd,
        )
        require(
            is_integer(response_seq) and response_seq == request_seq,
            cmd,
            f"response seq == {request_seq}",
            response_seq,
        )
        require_field(response, "code", is_integer, "an integer", cmd)
        require_field(response, "msg", lambda item: isinstance(item, str), "a string", cmd)
        require_field(response, "data", lambda item: isinstance(item, dict), "an object", cmd)
        return response


def expect_ok(response, cmd):
    code = response["code"]
    require(
        code == ERR_OK,
        cmd,
        f"code == {ERR_OK}",
        f"code={code}, msg={response['msg']!r}",
    )
    return response["data"]


def load_target():
    root = Path(__file__).resolve().parents[1]
    config_path = root / "config" / "app.ini"
    parser = configparser.ConfigParser()
    if config_path.exists():
        try:
            with config_path.open("r", encoding="utf-8") as config_file:
                parser.read_file(config_file)
        except (OSError, configparser.Error) as exc:
            raise SmokeFailure(f"cannot read {config_path}: {exc}") from exc

    host = parser.get("server", "host", fallback=DEFAULT_HOST).strip() or DEFAULT_HOST
    try:
        port = parser.getint("server", "port", fallback=DEFAULT_PORT)
    except ValueError as exc:
        raise SmokeFailure(f"invalid server port in {config_path}: {exc}") from exc
    if not 1 <= port <= 65535:
        raise SmokeFailure(f"invalid server port in {config_path}: {port}")
    return host, port


def validate_station_list(client, token):
    response = client.request(
        CMD_STATION_LIST, token, {"page": 1, "size": 100}
    )
    expect_ok(response, CMD_STATION_LIST)
    total, items = require_list_result(response, CMD_STATION_LIST)
    for item in items:
        require(isinstance(item, dict), CMD_STATION_LIST, "each list item to be an object", type_name(item))
        station_id = require_nonnegative_integer(item, "stationId", CMD_STATION_LIST)
        require(station_id > 0, CMD_STATION_LIST, "stationId > 0", station_id)
        require_string(item, "name", CMD_STATION_LIST)
        require_string(item, "address", CMD_STATION_LIST)
        require_field(item, "lng", is_number, "a number", CMD_STATION_LIST)
        require_field(item, "lat", is_number, "a number", CMD_STATION_LIST)
        require_nonnegative_integer(item, "pileTotal", CMD_STATION_LIST)
        online_rate = require_field(item, "onlineRate", is_number, "a number", CMD_STATION_LIST)
        require(0 <= online_rate <= 100, CMD_STATION_LIST, "onlineRate in [0, 100]", online_rate)
    print(f"[PASS] 2101 station list: total={total}")
    return items[0]["stationId"] if items else None


def validate_station_detail(client, token, station_id):
    if station_id is None:
        print("[SKIP] 2103 station detail: station list is empty")
        return
    response = client.request(
        CMD_STATION_DETAIL, token, {"stationId": station_id}
    )
    data = expect_ok(response, CMD_STATION_DETAIL)
    items = require_field(data, "list", lambda item: isinstance(item, list), "an array", CMD_STATION_DETAIL)
    print(f"[PASS] 2103 station detail: stationId={station_id}, piles={len(items)}")


def validate_pile_list(client, token):
    response = client.request(
        CMD_PILE_LIST,
        token,
        {"page": 1, "size": 100, "stationId": 0, "status": -1},
    )
    expect_ok(response, CMD_PILE_LIST)
    total, items = require_list_result(response, CMD_PILE_LIST)
    for item in items:
        require(isinstance(item, dict), CMD_PILE_LIST, "each list item to be an object", type_name(item))
        require_nonnegative_integer(item, "pileId", CMD_PILE_LIST)
        require_string(item, "code", CMD_PILE_LIST)
        require_string(item, "stationName", CMD_PILE_LIST)
        require_nonnegative_integer(item, "type", CMD_PILE_LIST)
        require_field(item, "power", is_number, "a number", CMD_PILE_LIST)
        require_nonnegative_integer(item, "status", CMD_PILE_LIST)
        require_nonnegative_integer(item, "chargeCount", CMD_PILE_LIST)
        require_nonnegative_integer(item, "chargeDuration", CMD_PILE_LIST)
    print(f"[PASS] 2111 pile list: total={total}")


def validate_user_list(client, token):
    response = client.request(
        CMD_ADMIN_USER_LIST,
        token,
        {"page": 1, "size": 100, "phoneLike": ""},
    )
    expect_ok(response, CMD_ADMIN_USER_LIST)
    total, items = require_list_result(response, CMD_ADMIN_USER_LIST)
    for item in items:
        require(isinstance(item, dict), CMD_ADMIN_USER_LIST, "each list item to be an object", type_name(item))
        require_nonnegative_integer(item, "userId", CMD_ADMIN_USER_LIST)
        require_string(item, "phone", CMD_ADMIN_USER_LIST)
        require_string(item, "nickname", CMD_ADMIN_USER_LIST)
        require_nonnegative_integer(item, "balance", CMD_ADMIN_USER_LIST)
        require_string(item, "createTime", CMD_ADMIN_USER_LIST)
        require_nonnegative_integer(item, "status", CMD_ADMIN_USER_LIST)
    print(f"[PASS] 2201 user list: total={total}")


def validate_revenue(client, token):
    response = client.request(CMD_STAT_REVENUE, token, {})
    data = expect_ok(response, CMD_STAT_REVENUE)
    today = require_nonnegative_integer(data, "today", CMD_STAT_REVENUE)
    month = require_nonnegative_integer(data, "month", CMD_STAT_REVENUE)
    total = require_nonnegative_integer(data, "total", CMD_STAT_REVENUE)
    print(f"[PASS] 2301 revenue: today={today}, month={month}, total={total}")


def validate_revenue_trend(client, token, days):
    response = client.request(CMD_STAT_REVENUE_TREND, token, {"days": days})
    data = expect_ok(response, CMD_STAT_REVENUE_TREND)
    items = require_field(data, "list", lambda item: isinstance(item, list), "an array", CMD_STAT_REVENUE_TREND)
    require(len(items) == days, CMD_STAT_REVENUE_TREND, f"list length == {days}", len(items))
    for item in items:
        require(isinstance(item, dict), CMD_STAT_REVENUE_TREND, "each list item to be an object", type_name(item))
        require_string(item, "date", CMD_STAT_REVENUE_TREND, nonempty=True)
        require_nonnegative_integer(item, "amount", CMD_STAT_REVENUE_TREND)
    print(f"[PASS] 2302 revenue trend: {days} days")


def validate_pile_status(client, token):
    response = client.request(CMD_STAT_PILE_STATUS, token, {})
    data = expect_ok(response, CMD_STAT_PILE_STATUS)
    in_use = require_nonnegative_integer(data, "inUse", CMD_STAT_PILE_STATUS)
    idle = require_nonnegative_integer(data, "idle", CMD_STAT_PILE_STATUS)
    fault = require_nonnegative_integer(data, "fault", CMD_STAT_PILE_STATUS)
    total = require_nonnegative_integer(data, "total", CMD_STAT_PILE_STATUS)
    require(
        in_use + idle + fault == total,
        CMD_STAT_PILE_STATUS,
        "inUse + idle + fault == total",
        f"{in_use} + {idle} + {fault} != {total}",
    )
    print(f"[PASS] 2303 pile status: total={total}")


def validate_order_list(client, token):
    response = client.request(
        CMD_ADMIN_ORDER_LIST,
        token,
        {
            "page": 1,
            "size": 100,
            "status": -1,
            "dateFrom": "",
            "dateTo": "",
        },
    )
    expect_ok(response, CMD_ADMIN_ORDER_LIST)
    total, items = require_list_result(response, CMD_ADMIN_ORDER_LIST)
    for item in items:
        require(isinstance(item, dict), CMD_ADMIN_ORDER_LIST, "each list item to be an object", type_name(item))
        require_nonnegative_integer(item, "orderId", CMD_ADMIN_ORDER_LIST)
        require_string(item, "orderNo", CMD_ADMIN_ORDER_LIST)
        require_nonnegative_integer(item, "status", CMD_ADMIN_ORDER_LIST)
        require_nonnegative_integer(item, "price", CMD_ADMIN_ORDER_LIST)
        require_field(item, "kwh", is_number, "a number", CMD_ADMIN_ORDER_LIST)
        require_nonnegative_integer(item, "amount", CMD_ADMIN_ORDER_LIST)
        require_string(item, "reserveTime", CMD_ADMIN_ORDER_LIST)
    print(f"[PASS] 2304 admin order list: total={total}")


def run_smoke(host, port, account, password):
    with socket.create_connection(
        (host, port), timeout=SOCKET_TIMEOUT_SECONDS
    ) as sock:
        sock.settimeout(SOCKET_TIMEOUT_SECONDS)
        client = SmokeClient(sock)

        response = client.request(CMD_STAT_REVENUE, "", {})
        require(
            response["code"] == ERR_NOT_LOGIN,
            CMD_STAT_REVENUE,
            f"code == ERR_NOT_LOGIN ({ERR_NOT_LOGIN}) without token",
            f"code={response['code']}, msg={response['msg']!r}",
        )
        print("[PASS] auth guard: 2301 without token -> ERR_NOT_LOGIN")

        response = client.request(
            CMD_ADMIN_LOGIN,
            "",
            {"account": account, "password": password},
        )
        data = expect_ok(response, CMD_ADMIN_LOGIN)
        token = require_string(data, "token", CMD_ADMIN_LOGIN, nonempty=True)
        admin_id = require_field(data, "adminId", is_integer, "an integer", CMD_ADMIN_LOGIN)
        require(admin_id > 0, CMD_ADMIN_LOGIN, "data.adminId > 0", admin_id)
        require_string(data, "account", CMD_ADMIN_LOGIN, nonempty=True)
        print("[PASS] 2001 admin login: token received")

        station_id = validate_station_list(client, token)
        validate_station_detail(client, token, station_id)
        validate_pile_list(client, token)
        validate_user_list(client, token)
        validate_revenue(client, token)
        validate_revenue_trend(client, token, 7)
        validate_revenue_trend(client, token, 30)
        validate_pile_status(client, token)
        validate_order_list(client, token)


def main():
    print("ECP admin smoke test")
    try:
        host, port = load_target()
        print(f"target: {host}:{port}\n")
        account = os.environ.get("ECP_ADMIN_ACCOUNT", "admin")
        password = os.environ.get("ECP_ADMIN_PASSWORD", "123456")
        run_smoke(host, port, account, password)
    except (SmokeFailure, OSError, socket.timeout) as exc:
        print(f"[FAIL] {exc}")
        print("[SKIP] 2112 server handler not implemented")
        print("[SKIP] 2305 server handler not implemented")
        print("\nRESULT: FAIL")
        return 1

    print("[SKIP] 2112 server handler not implemented")
    print("[SKIP] 2305 server handler not implemented")
    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

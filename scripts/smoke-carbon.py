#!/usr/bin/env python3
"""扩展模块 08「碳减排与能源报告」协议 smoke —— 归属 L5。

照 scripts/smoke-admin.py 的范式：4 字节大端长度头 + UTF-8 JSON 体，
逐条断言响应契约。**全程只读**，只发查询与重算命令，不改订单、钱包、电桩。

3745 会重写 t_carbon_daily（那正是它的职责），除此之外不写任何表。

用法：
    ./build/bin/ecp-server &            # 先起服务端
    python3 scripts/smoke-carbon.py

    --db PATH   额外做一次「服务端数字 vs SQLite 直算」对拍（只读打开）

退出码：0 = 全过，1 = 有断言失败
"""

import argparse
import configparser
import json
import os
from pathlib import Path
import socket
import sqlite3
import struct
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 9527
SOCKET_TIMEOUT_SECONDS = 30          # 首次懒聚合要跑全量，给足时间
FRAME_MAX_PAYLOAD = 1024 * 1024

ERR_OK = 0
ERR_PARAM = 1001
ERR_NOT_LOGIN = 1002

CMD_ADMIN_LOGIN = 2001
CMD_EXT_CARBON_METRIC = 3740
CMD_EXT_FACTOR_LIST = 3741
CMD_EXT_CARBON_AGGREGATE = 3745

DISCLAIMER = "课程项目估算，非认证碳数据，不可用于碳交易或监管申报"
ALGO_VERSION = "carbon-v1"


class SmokeFailure(Exception):
    """协议或响应契约断言失败。"""


# ---- 帧收发（与 smoke-admin.py 一致） ---------------------------------------

def recv_exact(sock, size):
    chunks, remaining = [], size
    while remaining:
        chunk = sock.recv(remaining)
        if not chunk:
            raise SmokeFailure(f"接收 {size} 字节时连接被关闭")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_request(sock, cmd, seq, token, data):
    envelope = {"cmd": cmd, "seq": seq, "token": token, "data": data}
    payload = json.dumps(envelope, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise SmokeFailure(f"cmd {cmd}: 请求体超过 {FRAME_MAX_PAYLOAD} 字节")
    sock.sendall(struct.pack("!I", len(payload)) + payload)


def recv_response(sock):
    (length,) = struct.unpack("!I", recv_exact(sock, 4))
    if length > FRAME_MAX_PAYLOAD:
        raise SmokeFailure(f"响应帧长度 {length} 超过 {FRAME_MAX_PAYLOAD}")
    body = recv_exact(sock, length)
    try:
        response = json.loads(body.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SmokeFailure(f"响应不是合法的 UTF-8 JSON: {exc}") from exc
    if not isinstance(response, dict):
        raise SmokeFailure("响应信封应为对象")
    return response


def is_integer(value):
    return isinstance(value, int) and not isinstance(value, bool)


def require(condition, cmd, expected, actual):
    if not condition:
        raise SmokeFailure(f"cmd {cmd}: 期望 {expected}；实得 {actual}")


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
            raise SmokeFailure(f"cmd {cmd}: 传输/帧失败: {exc}") from exc
        require(response.get("cmd") == cmd, cmd, f"响应 cmd == {cmd}", response.get("cmd"))
        require(response.get("seq") == request_seq, cmd, f"响应 seq == {request_seq}", response.get("seq"))
        require(is_integer(response.get("code")), cmd, "code 为整数", response.get("code"))
        require(isinstance(response.get("data"), dict), cmd, "data 为对象", type(response.get("data")).__name__)
        return response


def expect_ok(response, cmd):
    require(response["code"] == ERR_OK, cmd, f"code == {ERR_OK}",
            f"code={response['code']}, msg={response['msg']!r}")
    return response["data"]


# ---- 断言 --------------------------------------------------------------------

def check_metric_shape(data, cmd):
    """逐格校验 3740 的响应契约与守恒等式。"""
    for key in ("list", "totals", "factorVersion", "algoVersion", "cutoffTime",
                "completeness", "tariffMode", "disclaimer"):
        require(key in data, cmd, f"data.{key} 存在", "字段缺失")

    require(data["algoVersion"] == ALGO_VERSION, cmd,
            f"algoVersion == {ALGO_VERSION}", data["algoVersion"])
    # 模块 05 未落地，必须如实标注固定时段口径，页面据此显示说明
    require(data["tariffMode"] == "FIXED_RANGE", cmd, "tariffMode == FIXED_RANGE", data["tariffMode"])
    require(data["disclaimer"] == DISCLAIMER, cmd, "免责声明逐字一致", data["disclaimer"])

    rows = data["list"]
    require(isinstance(rows, list), cmd, "list 为数组", type(rows).__name__)

    acc = {k: 0 for k in ("totalKwhX100", "peakKwhX100", "flatKwhX100",
                          "valleyKwhX100", "unallocKwhX100", "orderCnt", "emissionG")}
    prev_date = ""
    for row in rows:
        for key in ("date", "totalKwhX100", "peakKwhX100", "flatKwhX100", "valleyKwhX100",
                    "unallocKwhX100", "orderCnt", "emissionG", "intensityGPerKwh", "completeness"):
            require(key in row, cmd, f"list 项含 {key}", f"缺 {key}：{row}")
        # 分时守恒 —— 最大余数法的核心保证，逐日成立
        total = row["totalKwhX100"]
        parts = row["peakKwhX100"] + row["flatKwhX100"] + row["valleyKwhX100"] + row["unallocKwhX100"]
        require(parts == total, cmd,
                f"{row['date']} 峰+平+谷+未分配 == 总量 {total}", parts)
        require(row["date"] > prev_date, cmd, "list 按日期严格升序", f"{prev_date} → {row['date']}")
        prev_date = row["date"]
        require(-1 <= row["completeness"] <= 100, cmd, "completeness 落在 -1..100", row["completeness"])
        for key in acc:
            acc[key] += row[key]

    totals = data["totals"]
    for key, want in acc.items():
        require(totals[key] == want, cmd, f"totals.{key} == 各行之和 {want}", totals[key])
    parts = totals["peakKwhX100"] + totals["flatKwhX100"] + totals["valleyKwhX100"] + totals["unallocKwhX100"]
    require(parts == totals["totalKwhX100"], cmd, "totals 分时守恒", parts)
    return totals


def validate_auth_guard(client):
    response = client.request(CMD_EXT_CARBON_METRIC, "", {"stationId": 0,
                                                          "dateFrom": "2026-07-01", "dateTo": "2026-07-02"})
    require(response["code"] == ERR_NOT_LOGIN, CMD_EXT_CARBON_METRIC,
            f"无 token 时 code == ERR_NOT_LOGIN({ERR_NOT_LOGIN})", response["code"])
    print("[PASS] 鉴权门：3740 无 token → ERR_NOT_LOGIN")


def validate_param_guard(client, token):
    cases = [
        ({"stationId": 0, "dateFrom": "2026-07-10", "dateTo": "2026-07-01"}, "dateTo < dateFrom"),
        ({"stationId": 0, "dateFrom": "not-a-date", "dateTo": "2026-07-01"}, "日期格式非法"),
        ({"stationId": -1, "dateFrom": "2026-07-01", "dateTo": "2026-07-02"}, "stationId 为负"),
        ({"stationId": 0, "dateFrom": "2020-01-01", "dateTo": "2026-07-01"}, "范围超 366 天"),
        ({"dateFrom": "2026-07-01", "dateTo": "2026-07-02"}, "缺 stationId"),
    ]
    for data, name in cases:
        response = client.request(CMD_EXT_CARBON_METRIC, token, data)
        require(response["code"] == ERR_PARAM, CMD_EXT_CARBON_METRIC,
                f"{name} → ERR_PARAM({ERR_PARAM})", response["code"])
    print(f"[PASS] 入参校验：{len(cases)} 种非法入参全部被拒为 ERR_PARAM")


def validate_factor_list(client, token):
    data = expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}), CMD_EXT_FACTOR_LIST)
    rows = data["list"]
    require(len(rows) >= 1, CMD_EXT_FACTOR_LIST, "至少一个演示因子", len(rows))
    for row in rows:
        require(row["factorGPerKwh"] > 0, CMD_EXT_FACTOR_LIST, "因子为正", row["factorGPerKwh"])
        # 08 文档第 9 节：来源不可靠是已登记风险，因此来源说明不得为空
        require(bool(row["source"]), CMD_EXT_FACTOR_LIST, "来源说明非空", row["source"])
    print(f"[PASS] 3741 排放因子列表：{len(rows)} 条，来源说明齐备")
    return rows


def validate_empty_range(client, token):
    """空日期范围必须返回空结果而不是崩溃或伪造 100% 完整度。"""
    data = expect_ok(client.request(CMD_EXT_CARBON_METRIC, token,
                                    {"stationId": 0, "dateFrom": "2030-01-01", "dateTo": "2030-01-03"}),
                     CMD_EXT_CARBON_METRIC)
    totals = check_metric_shape(data, CMD_EXT_CARBON_METRIC)
    require(data["list"] == [], CMD_EXT_CARBON_METRIC, "无数据日期范围 list 为空", data["list"])
    require(totals["totalKwhX100"] == 0, CMD_EXT_CARBON_METRIC, "总电量为 0", totals["totalKwhX100"])
    # 总量为 0 时完整度必须是 -1「不适用」，绝不能伪装成 100（实现规划第 2 节）
    require(data["completeness"] == -1, CMD_EXT_CARBON_METRIC,
            "总量为 0 时 completeness == -1（不适用），不得伪装成 100", data["completeness"])
    require(totals["intensityGPerKwh"] == -1, CMD_EXT_CARBON_METRIC,
            "总量为 0 时强度 == -1", totals["intensityGPerKwh"])
    print("[PASS] 空日期范围：返回空列表、完整度与强度均为 -1 不适用")


def validate_idempotent(client, token, date_from, date_to):
    """同范围重跑两次结果必须逐格相同 —— 幂等是懒聚合能成立的前提。"""
    args = {"stationId": 0, "dateFrom": date_from, "dateTo": date_to}
    first = expect_ok(client.request(CMD_EXT_CARBON_METRIC, token, args), CMD_EXT_CARBON_METRIC)
    check_metric_shape(first, CMD_EXT_CARBON_METRIC)

    second = expect_ok(client.request(CMD_EXT_CARBON_METRIC, token, args), CMD_EXT_CARBON_METRIC)
    for key in ("list", "totals"):
        require(first[key] == second[key], CMD_EXT_CARBON_METRIC,
                f"两次查询 {key} 逐格一致", "两次结果不同")
    print(f"[PASS] 幂等：{date_from} ~ {date_to} 连查两次，list 与 totals 逐格一致")

    # 显式重算后数字仍须不变 —— 重算改的是行，不是结论
    agg = expect_ok(client.request(CMD_EXT_CARBON_AGGREGATE, token, args), CMD_EXT_CARBON_AGGREGATE)
    require(agg["days"] > 0, CMD_EXT_CARBON_AGGREGATE, "重算天数 > 0", agg["days"])
    third = expect_ok(client.request(CMD_EXT_CARBON_METRIC, token, args), CMD_EXT_CARBON_METRIC)
    require(first["totals"] == third["totals"], CMD_EXT_CARBON_METRIC,
            "3745 重算后 totals 不变", f"{first['totals']} → {third['totals']}")
    print(f"[PASS] 3745 显式重算 {agg['days']} 天 / {agg['rewritten']} 行后，totals 逐格不变")
    return first


def validate_station_split(client, token, date_from, date_to, station_ids):
    """各站之和必须等于全站数字 —— station_id=0 不入库，全站数字只能由分站行加出来。"""
    all_data = expect_ok(client.request(CMD_EXT_CARBON_METRIC, token,
                                        {"stationId": 0, "dateFrom": date_from, "dateTo": date_to}),
                         CMD_EXT_CARBON_METRIC)
    summed = 0
    for station_id in station_ids:
        data = expect_ok(client.request(CMD_EXT_CARBON_METRIC, token,
                                        {"stationId": station_id, "dateFrom": date_from, "dateTo": date_to}),
                         CMD_EXT_CARBON_METRIC)
        check_metric_shape(data, CMD_EXT_CARBON_METRIC)
        summed += data["totals"]["totalKwhX100"]
    require(summed == all_data["totals"]["totalKwhX100"], CMD_EXT_CARBON_METRIC,
            f"各站电量之和 == 全站电量 {all_data['totals']['totalKwhX100']}", summed)
    print(f"[PASS] 分站求和：{len(station_ids)} 个站的电量之和 == 全站总量 {summed}")


def validate_against_db(db_path, token_data, date_from, date_to):
    """与 SQLite 直算对拍：服务端的总电量必须等于 SUM(kwh_x100) 手算值。"""
    uri = f"file:{Path(db_path).resolve()}?mode=ro"
    with sqlite3.connect(uri, uri=True) as conn:
        row = conn.execute(
            "SELECT COALESCE(SUM(kwh_x100), 0), COUNT(*) FROM t_order"
            " WHERE status = 3 AND date(settle_time) >= ? AND date(settle_time) <= ?",
            (date_from, date_to)).fetchone()
    db_kwh, db_cnt = row
    totals = token_data["totals"]
    require(totals["totalKwhX100"] == db_kwh, CMD_EXT_CARBON_METRIC,
            f"总电量 == SQLite SUM(kwh_x100) = {db_kwh}", totals["totalKwhX100"])
    require(totals["orderCnt"] == db_cnt, CMD_EXT_CARBON_METRIC,
            f"订单数 == SQLite COUNT(*) = {db_cnt}（只计 status=3）", totals["orderCnt"])
    print(f"[PASS] 对拍 SQLite：总电量 {db_kwh}（{db_cnt} 单，只计已结算）逐格一致")


# ---- 主流程 ------------------------------------------------------------------

def load_target(host_override, port_override):
    host, port = DEFAULT_HOST, DEFAULT_PORT
    ini = Path(__file__).resolve().parent.parent / "config" / "app.ini"
    if ini.is_file():
        parser = configparser.ConfigParser()
        try:
            parser.read(ini, encoding="utf-8")
            if parser.has_option("server", "host"):
                host = parser.get("server", "host")
            if parser.has_option("server", "port"):
                port = parser.getint("server", "port")
        except (configparser.Error, ValueError):
            pass
    if host_override:
        host = host_override
    if port_override:
        port = int(port_override)
    return host, port


def date_range_from_db(db_path):
    """用真实数据的日期范围跑，而不是硬编码一段可能没有订单的日期。"""
    uri = f"file:{Path(db_path).resolve()}?mode=ro"
    with sqlite3.connect(uri, uri=True) as conn:
        row = conn.execute(
            "SELECT MIN(date(settle_time)), MAX(date(settle_time))"
            " FROM t_order WHERE status = 3").fetchone()
        stations = [r[0] for r in conn.execute(
            "SELECT DISTINCT station_id FROM t_order WHERE status = 3 ORDER BY station_id")]
    return row[0], row[1], stations


def parse_args():
    parser = argparse.ArgumentParser(description="扩展模块 08 碳减排协议 smoke")
    parser.add_argument("--host", help="服务端地址，覆盖 config/app.ini")
    parser.add_argument("--port", help="服务端端口，覆盖 config/app.ini")
    parser.add_argument("--db", default="charging.db",
                        help="SQLite 路径，用于对拍与推断日期范围（只读打开）")
    parser.add_argument("--date-from", help="查询起始日，默认取库中最早结算日")
    parser.add_argument("--date-to", help="查询结束日，默认取库中最晚结算日")
    return parser.parse_args()


def run_smoke(host, port, account, password, args):
    date_from, date_to, stations = args.date_from, args.date_to, []
    db_ok = Path(args.db).is_file()
    if db_ok:
        db_from, db_to, stations = date_range_from_db(args.db)
        date_from = date_from or db_from
        date_to = date_to or db_to
    if not date_from or not date_to:
        raise SmokeFailure("无法确定查询日期范围：请给 --date-from/--date-to 或可读的 --db")

    with socket.create_connection((host, port), timeout=SOCKET_TIMEOUT_SECONDS) as sock:
        sock.settimeout(SOCKET_TIMEOUT_SECONDS)
        client = SmokeClient(sock)

        validate_auth_guard(client)

        data = expect_ok(client.request(CMD_ADMIN_LOGIN, "",
                                        {"account": account, "password": password}), CMD_ADMIN_LOGIN)
        token = data["token"]
        require(bool(token), CMD_ADMIN_LOGIN, "token 非空", token)
        print("[PASS] 2001 管理员登录：已获取 token")

        validate_param_guard(client, token)
        validate_factor_list(client, token)
        validate_empty_range(client, token)

        print(f"\n查询范围：{date_from} ~ {date_to}")
        metric = validate_idempotent(client, token, date_from, date_to)
        if stations:
            validate_station_split(client, token, date_from, date_to, stations)
        if db_ok:
            validate_against_db(args.db, metric, date_from, date_to)
        else:
            print(f"[SKIP] 未找到 {args.db}，跳过 SQLite 对拍")

        totals = metric["totals"]
        print(f"\n汇总：电量 {totals['totalKwhX100'] / 100:.2f} 度　"
              f"排放 {totals['emissionG'] / 1000:.2f} kg　"
              f"强度 {totals['intensityGPerKwh']} g/度　"
              f"完整度 {metric['completeness']}%　"
              f"因子 {metric['factorVersion']}　算法 {metric['algoVersion']}")
        print(f"峰/平/谷/未分配：{totals['peakKwhX100']} / {totals['flatKwhX100']}"
              f" / {totals['valleyKwhX100']} / {totals['unallocKwhX100']}")


def main():
    print("扩展模块 08 碳减排与能源报告 · 协议 smoke\n")
    try:
        args = parse_args()
        host, port = load_target(args.host, args.port)
        print(f"目标：{host}:{port}")
        run_smoke(host, port,
                  os.environ.get("ECP_ADMIN_ACCOUNT", "admin"),
                  os.environ.get("ECP_ADMIN_PASSWORD", "123456"),
                  args)
    except (SmokeFailure, OSError, socket.timeout) as exc:
        print(f"\n[FAIL] {exc}")
        print("\nRESULT: FAIL")
        return 1
    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

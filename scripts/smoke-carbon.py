#!/usr/bin/env python3
"""扩展模块 08「碳减排与能源报告」协议 smoke —— 归属 L5。

照 scripts/smoke-admin.py 的范式：4 字节大端长度头 + UTF-8 JSON 体，
逐条断言响应契约。**全程只读**，只发查询与重算命令，不改订单、钱包、电桩。

3745 会重写 t_carbon_daily（那正是它的职责），除此之外不写任何表。

用法：
    ./build/bin/ecp-server &            # 先起服务端
    python3 scripts/smoke-carbon.py

    --db PATH   额外做一次「服务端数字 vs SQLite 直算」对拍（只读打开）

⚠ --mutating 会写 t_carbon_factor / t_carbon_report 并落导出文件；新增因子时
  演示因子的开区间会被**接续闭合**（裁决 D9）。请在库副本上跑，或接受这些改动。

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
CMD_EXT_FACTOR_SET = 3742
CMD_EXT_REPORT_GEN = 3743
CMD_EXT_REPORT_EXPORT = 3744
CMD_EXT_CARBON_AGGREGATE = 3745
CMD_EXT_REPORT_LIST = 3746
CMD_EXT_FACTOR_DELETE = 3747
CMD_EXT_FACTOR_PURGE = 3748

ERR_CARBON_NO_FACTOR = 6701
ERR_CARBON_FACTOR_OVERLAP = 6702
ERR_CARBON_REPORT_STALE = 6703
ERR_CARBON_REPORT_NOT_FOUND = 6704
ERR_CARBON_FACTOR_NOT_FOUND = 6705
ERR_CARBON_LAST_FACTOR = 6706
ERR_CARBON_VERSION_SHARED = 6707

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


def validate_no_factor(client, token):
    """2000-01-01 之前没有任何因子生效 → 必须报 6701，而不是编一个排放量出来。"""
    response = client.request(CMD_EXT_CARBON_METRIC, token,
                              {"stationId": 0, "dateFrom": "1999-12-01", "dateTo": "1999-12-05"})
    require(response["code"] == ERR_CARBON_NO_FACTOR, CMD_EXT_CARBON_METRIC,
            f"演示因子生效期之前 → ERR_CARBON_NO_FACTOR({ERR_CARBON_NO_FACTOR})", response["code"])
    print(f"[PASS] 无因子时段：1999-12 查询 → {ERR_CARBON_NO_FACTOR}（不编造排放量）")

    # 服务端对扩展段错误码也应返回中文文案，而不是「未知错误(6701)」。
    # 这条验的是 common/error_code.h 的 ExtMsgProvider 挂钩确实被注册上了 ——
    # 非 Qt 客户端（脚本、调试工具）只看 msg，拿不到本地映射表。
    msg = response["msg"]
    require("未知错误" not in msg and bool(msg.strip()), CMD_EXT_CARBON_METRIC,
            "msg 为扩展码的中文文案，而非「未知错误(NNNN)」", repr(msg))
    print(f"[PASS] 扩展错误码文案：服务端 msg = {msg!r}")


def validate_factor_set_guards(client, token, existing):
    """3742 的拒绝路径 —— 全部在写入前返回，不会改动 t_carbon_factor。"""
    demo = existing[0]
    bad = [
        ({}, "空入参"),
        ({"region": "", "version": "v", "source": "s", "factorGPerKwh": 500,
          "effectFrom": "2027-01-01 00:00:00"}, "region 为空"),
        ({"region": "测试区", "version": "v", "source": "", "factorGPerKwh": 500,
          "effectFrom": "2027-01-01 00:00:00"}, "来源说明为空（08 第 9 节风险项）"),
        ({"region": "测试区", "version": "v", "source": "s", "factorGPerKwh": 0,
          "effectFrom": "2027-01-01 00:00:00"}, "因子非正"),
        ({"region": "测试区", "version": "v", "source": "s", "factorGPerKwh": 500,
          "effectFrom": "2027-01-01 12:00:00"}, "生效边界未对齐自然日（裁决 D6）"),
        ({"region": "测试区", "version": "v", "source": "s", "factorGPerKwh": 500,
          "effectFrom": "2027-01-01 00:00:00", "effectTo": "2026-01-01 00:00:00"}, "生效止早于生效起"),
        ({"region": demo["region"], "version": demo["version"], "source": "改过的来源",
          "factorGPerKwh": demo["factorGPerKwh"], "effectFrom": demo["effectFrom"]},
         "同版本改内容（因子版本不可变）"),
    ]
    for data, name in bad:
        response = client.request(CMD_EXT_FACTOR_SET, token, data)
        require(response["code"] == ERR_PARAM, CMD_EXT_FACTOR_SET,
                f"{name} → ERR_PARAM({ERR_PARAM})", response["code"])
    print(f"[PASS] 3742 入参校验：{len(bad)} 种非法入参全部被拒")

    # 与演示因子 [2000-01-01, ∞) 相交 → 6702
    # 起点早于演示因子起点且区间与之相交 → 真重叠（起点更晚会被当成合法接续）
    response = client.request(CMD_EXT_FACTOR_SET, token, {
        "region": demo["region"], "version": "overlap-probe", "source": "重叠探针",
        "factorGPerKwh": 500, "effectFrom": "1990-01-01 00:00:00",
        "effectTo": "2010-01-01 00:00:00"})
    require(response["code"] == ERR_CARBON_FACTOR_OVERLAP, CMD_EXT_FACTOR_SET,
            f"生效区间重叠 → ERR_CARBON_FACTOR_OVERLAP({ERR_CARBON_FACTOR_OVERLAP})", response["code"])
    print(f"[PASS] 3742 重叠拒绝：与演示因子相交 → {ERR_CARBON_FACTOR_OVERLAP}")

    # 换个区域名不能成为绕过重叠校验的后门 —— 本系统只有一条全局因子时间线，
    # pickFactor 不看 region。放行的话历史数字会被悄悄改掉（实测 581 → 999）。
    # 起点必须早于演示因子起点，否则会被当成合法的「接续发布」而不是重叠。
    # 这里要验的是：换个区域名不能绕过重叠校验。
    response = client.request(CMD_EXT_FACTOR_SET, token, {
        "region": "另一个电网", "version": "cross-region-probe", "source": "跨区域重叠探针",
        "factorGPerKwh": 999, "effectFrom": "1990-01-01 00:00:00",
        "effectTo": "2010-01-01 00:00:00"})
    require(response["code"] == ERR_CARBON_FACTOR_OVERLAP, CMD_EXT_FACTOR_SET,
            f"跨区域时间重叠 → ERR_CARBON_FACTOR_OVERLAP({ERR_CARBON_FACTOR_OVERLAP})",
            response["code"])
    print(f"[PASS] 3742 跨区域重叠：换区域名也被拒 → {ERR_CARBON_FACTOR_OVERLAP}")

    # 原样重提演示因子 → 幂等命中，不新建
    data = expect_ok(client.request(CMD_EXT_FACTOR_SET, token, {
        "region": demo["region"], "version": demo["version"], "source": demo["source"],
        "factorGPerKwh": demo["factorGPerKwh"], "effectFrom": demo["effectFrom"],
        "effectTo": demo["effectTo"]}), CMD_EXT_FACTOR_SET)
    require(data["created"] is False, CMD_EXT_FACTOR_SET, "重复提交 created == False", data["created"])
    require(data["factorId"] == demo["factorId"], CMD_EXT_FACTOR_SET,
            f"幂等返回原 factorId {demo['factorId']}", data["factorId"])
    after = expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}), CMD_EXT_FACTOR_LIST)["list"]
    require(len(after) == len(existing), CMD_EXT_FACTOR_LIST,
            f"因子条数不变 {len(existing)}", len(after))
    print("[PASS] 3742 幂等：原样重提演示因子 → created=False，未新增行")


def validate_factor_set_write(client, token, existing):
    """真正写入一个新因子版本（仅 --mutating）。生效期紧邻演示因子右端，不重叠。"""
    before = len(existing)
    payload = {"region": "smoke-测试电网", "version": "smoke-v1",
               "source": "smoke 测试写入的因子，非真实数据", "factorGPerKwh": 500,
               "effectFrom": "2027-01-01 00:00:00", "effectTo": "2028-01-01 00:00:00"}
    data = expect_ok(client.request(CMD_EXT_FACTOR_SET, token, payload), CMD_EXT_FACTOR_SET)
    require(data["created"] is True, CMD_EXT_FACTOR_SET, "首次写入 created == True", data["created"])
    print(f"[PASS] 3742 新增因子：factorId={data['factorId']}")

    # 演示因子是 [2000-01-01, 无穷)，新因子起点更晚 → 自动接续，把它闭合到新起点。
    # 不这样处理的话，出厂状态下第二个因子永远发布不进去（裁决 D9）。
    require(data.get("supersededFactorId", 0) > 0, CMD_EXT_FACTOR_SET,
            "新因子接续了既有开区间因子", data)
    print(f"[PASS] 3742 接续发布：旧开区间因子 {data['supersededFactorId']} 已闭合到 "
          f"{payload['effectFrom'][:10]}")

    again = expect_ok(client.request(CMD_EXT_FACTOR_SET, token, payload), CMD_EXT_FACTOR_SET)
    require(again["created"] is False and again["factorId"] == data["factorId"],
            CMD_EXT_FACTOR_SET, "再发一次 → 幂等命中同一 factorId", again)
    rows = expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}), CMD_EXT_FACTOR_LIST)["list"]
    require(len(rows) == before + 1, CMD_EXT_FACTOR_LIST, f"因子条数 {before} → {before + 1}", len(rows))
    print("[PASS] 3742 写幂等：重发同一因子未产生第二行")

    # 相邻区间 [2028-01-01, ∞) 不算重叠，必须放行
    ok = expect_ok(client.request(CMD_EXT_FACTOR_SET, token, {
        "region": "smoke-测试电网", "version": "smoke-v2", "source": "smoke 相邻区间",
        "factorGPerKwh": 480, "effectFrom": "2028-01-01 00:00:00"}), CMD_EXT_FACTOR_SET)
    require(ok["created"] is True, CMD_EXT_FACTOR_SET, "[a,b) 紧邻 [b,∞) 放行", ok)
    print("[PASS] 3742 相邻区间：[2027,2028) 与 [2028,∞) 不判重叠，正常写入")

    # 用 3747 把本次写入的因子撤干净，让 --mutating 可以重复跑。
    # 只跑得了一次的测试是脆的：第二次必然红，久了就没人敢跑它。
    # 顺序必须从晚到早 —— 每撤一个，它的前驱就把生效止收回来，逐层退回出厂状态。
    for factor_id in (ok["factorId"], data["factorId"]):
        undo = expect_ok(client.request(CMD_EXT_FACTOR_DELETE, token, {"factorId": factor_id}),
                         CMD_EXT_FACTOR_DELETE)
        require(undo["removed"] is True, CMD_EXT_FACTOR_DELETE,
                f"撤销 smoke 写入的因子 {factor_id}（无历史引用，应物理删除）", undo)
    final = expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}), CMD_EXT_FACTOR_LIST)["list"]
    require(len(final) == before, CMD_EXT_FACTOR_LIST,
            f"因子条数回到 {before}", len(final))
    demo_now = next(f for f in final if f["version"] == existing[0]["version"])
    require(demo_now["effectTo"] == existing[0]["effectTo"], CMD_EXT_FACTOR_LIST,
            f"演示因子生效止还原为 {existing[0]['effectTo']!r}", demo_now["effectTo"])
    print(f"[PASS] 3747 清理：撤销 2 个 smoke 因子，时间线还原（--mutating 可重复运行）")


def validate_report_guards(client, token):
    """3744/3746 的只读检查 —— 不生成任何报告，不写库。"""
    data = expect_ok(client.request(CMD_EXT_REPORT_LIST, token, {}), CMD_EXT_REPORT_LIST)
    rows = data["list"]
    require(isinstance(rows, list), CMD_EXT_REPORT_LIST, "list 为数组", type(rows).__name__)
    for row in rows:
        require(row["status"] in ("GENERATING", "READY", "FAILED", "STALE"),
                CMD_EXT_REPORT_LIST, "status 落在四种取值内", row["status"])
        # 报告是快照，四项分时之和必须仍然守恒
        parts = (row["peakKwhX100"] + row["flatKwhX100"]
                 + row["valleyKwhX100"] + row["unallocKwhX100"])
        require(parts == row["totalKwhX100"], CMD_EXT_REPORT_LIST,
                f"报告 {row['reportId']} 分时守恒", f"{parts} != {row['totalKwhX100']}")
    stale = [r for r in rows if r["status"] == "STALE"]
    print(f"[PASS] 3746 报告列表：{len(rows)} 份（其中 {len(stale)} 份已过期），分时守恒")

    bad = [
        ({}, "空入参"),
        ({"reportId": 1}, "缺 format"),
        ({"reportId": 1, "format": "pdf"}, "不支持的格式"),
        ({"reportId": 0, "format": "csv"}, "reportId 非正"),
    ]
    for payload, name in bad:
        response = client.request(CMD_EXT_REPORT_EXPORT, token, payload)
        require(response["code"] == ERR_PARAM, CMD_EXT_REPORT_EXPORT,
                f"{name} → ERR_PARAM({ERR_PARAM})", response["code"])
    response = client.request(CMD_EXT_REPORT_EXPORT, token, {"reportId": 999999, "format": "csv"})
    require(response["code"] == ERR_CARBON_REPORT_NOT_FOUND, CMD_EXT_REPORT_EXPORT,
            f"不存在的报告 → {ERR_CARBON_REPORT_NOT_FOUND}", response["code"])
    print(f"[PASS] 3744 入参校验：{len(bad)} 种非法入参被拒，不存在的报告 → "
          f"{ERR_CARBON_REPORT_NOT_FOUND}")

    # scope 与 stationId 必须自洽，否则表里会出现解释不清的行
    for payload, name in [
        ({"scope": "ALL", "stationId": 1, "dateFrom": "2026-08-01", "dateTo": "2026-08-02"},
         "scope=ALL 却带站号"),
        ({"scope": "STATION", "stationId": 0, "dateFrom": "2026-08-01", "dateTo": "2026-08-02"},
         "scope=STATION 却没站号"),
        ({"scope": "EVERYTHING", "stationId": 0, "dateFrom": "2026-08-01", "dateTo": "2026-08-02"},
         "未知 scope"),
    ]:
        response = client.request(CMD_EXT_REPORT_GEN, token, payload)
        require(response["code"] == ERR_PARAM, CMD_EXT_REPORT_GEN,
                f"{name} → ERR_PARAM({ERR_PARAM})", response["code"])
    print("[PASS] 3743 入参校验：scope 与 stationId 必须自洽")


def validate_report_write(client, token, date_from, date_to):
    """真正生成并导出一份报告（仅 --mutating）。"""
    req_id = f"smoke-{date_from}-{date_to}"
    args = {"scope": "ALL", "stationId": 0, "dateFrom": date_from, "dateTo": date_to,
            "reqId": req_id}
    gen = expect_ok(client.request(CMD_EXT_REPORT_GEN, token, args), CMD_EXT_REPORT_GEN)
    # 重复运行时同一 reqId 会幂等命中首次结果，此时状态可能已是 STALE，不必强求 READY
    require(gen["status"] in ("READY", "STALE"), CMD_EXT_REPORT_GEN,
            "报告状态为 READY 或 STALE", gen["status"])
    report_id = gen["reportId"]
    print(f"[PASS] 3743 生成报告：id={report_id} v{gen['version']} {gen['status']}")

    again = expect_ok(client.request(CMD_EXT_REPORT_GEN, token, args), CMD_EXT_REPORT_GEN)
    require(again["created"] is False and again["reportId"] == report_id,
            CMD_EXT_REPORT_GEN, "同 reqId 幂等返回原报告", again)
    print("[PASS] 3743 写幂等：同 reqId 重发未新建版本")

    # 报告快照必须与 3740 的当前汇总一致（刚生成，两者同源）
    metric = expect_ok(client.request(CMD_EXT_CARBON_METRIC, token,
                                      {"stationId": 0, "dateFrom": date_from, "dateTo": date_to}),
                       CMD_EXT_CARBON_METRIC)
    rows = expect_ok(client.request(CMD_EXT_REPORT_LIST, token, {}), CMD_EXT_REPORT_LIST)["list"]
    row = next(r for r in rows if r["reportId"] == report_id)
    for key in ("totalKwhX100", "peakKwhX100", "flatKwhX100", "valleyKwhX100", "emissionG"):
        require(row[key] == metric["totals"][key], CMD_EXT_REPORT_GEN,
                f"报告快照 {key} == 3740 当前汇总 {metric['totals'][key]}", row[key])
    print("[PASS] 报告快照与 3740 当前汇总逐格一致")

    # 空范围也要能生成并导出报告：这条曾经是 1099（factor_version 绑成了 NULL）
    empty = expect_ok(client.request(CMD_EXT_REPORT_GEN, token,
                                     {"scope": "ALL", "stationId": 0,
                                      "dateFrom": "2030-01-01", "dateTo": "2030-01-03",
                                      "reqId": "smoke-empty-range"}), CMD_EXT_REPORT_GEN)
    require(empty["status"] == "READY", CMD_EXT_REPORT_GEN, "空范围报告生成成功", empty)
    rows = expect_ok(client.request(CMD_EXT_REPORT_LIST, token, {}), CMD_EXT_REPORT_LIST)["list"]
    erow = next(r for r in rows if r["reportId"] == empty["reportId"])
    require(bool(erow["factorVersion"]) and bool(erow["cutoffTime"]), CMD_EXT_REPORT_LIST,
            "空范围报告的因子版本与截止时刻非空", erow)
    require(erow["totalKwhX100"] == 0, CMD_EXT_REPORT_LIST, "空范围报告总量为 0", erow)
    print("[PASS] 3743 空范围：报告可生成，因子版本与截止时刻均已落库")

    for fmt, marker in (("csv", "指标,数值,单位"), ("html", "<!doctype html>")):
        exp = expect_ok(client.request(CMD_EXT_REPORT_EXPORT, token,
                                       {"reportId": report_id, "format": fmt,
                                        "allowStale": True}),
                        CMD_EXT_REPORT_EXPORT)
        path = Path(exp["absPath"])
        require(path.is_file(), CMD_EXT_REPORT_EXPORT, f"导出文件已落盘 {path}", "文件不存在")
        text = path.read_text(encoding="utf-8-sig")
        require(marker in text, CMD_EXT_REPORT_EXPORT, f"{fmt} 内容含 {marker!r}", "未找到")
        # 免责声明与口径标注三处一致：页眉、导出文件、快照 JSON
        require(DISCLAIMER in text, CMD_EXT_REPORT_EXPORT, "导出含免责声明", "未找到")
        require("固定时段" in text, CMD_EXT_REPORT_EXPORT, "导出标注固定时段口径", "未找到")
        print(f"[PASS] 3744 导出 {fmt}：{exp['path']}（含免责声明与口径标注）")


def validate_factor_delete_guards(client, token, existing):
    """3747 的只读拒绝路径 —— 不改动任何因子。"""
    for payload, name in [({}, "空入参"), ({"factorId": 0}, "factorId 非正"),
                          ({"factorId": "x"}, "类型错")]:
        response = client.request(CMD_EXT_FACTOR_DELETE, token, payload)
        require(response["code"] == ERR_PARAM, CMD_EXT_FACTOR_DELETE,
                f"{name} → ERR_PARAM({ERR_PARAM})", response["code"])
    response = client.request(CMD_EXT_FACTOR_DELETE, token, {"factorId": 999999})
    require(response["code"] == ERR_CARBON_FACTOR_NOT_FOUND, CMD_EXT_FACTOR_DELETE,
            f"不存在的因子 → {ERR_CARBON_FACTOR_NOT_FOUND}", response["code"])

    # 只剩一个启用因子时不许撤销：撤了之后什么都算不出来
    enabled = [f for f in existing if f["enabled"]]
    if len(enabled) == 1:
        response = client.request(CMD_EXT_FACTOR_DELETE, token,
                                  {"factorId": enabled[0]["factorId"]})
        require(response["code"] == ERR_CARBON_LAST_FACTOR, CMD_EXT_FACTOR_DELETE,
                f"撤销最后一个启用因子 → {ERR_CARBON_LAST_FACTOR}", response["code"])
        print(f"[PASS] 3747 拒绝路径：非法入参、不存在(6705)、最后一个启用因子(6706)")
    else:
        print(f"[PASS] 3747 拒绝路径：非法入参、不存在(6705)　[SKIP] 6706（当前有多个启用因子）")


def validate_factor_delete_write(client, token):
    """加一个因子再撤销，验证时间线完整回复（仅 --mutating）。"""
    before = expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}), CMD_EXT_FACTOR_LIST)["list"]
    added = expect_ok(client.request(CMD_EXT_FACTOR_SET, token, {
        "region": "smoke-撤销测试", "version": "smoke-undo", "source": "加了就撤，用于验证时间线恢复",
        "factorGPerKwh": 400, "effectFrom": "2029-01-01 00:00:00"}), CMD_EXT_FACTOR_SET)
    superseded = added.get("supersededFactorId", 0)

    removed = expect_ok(client.request(CMD_EXT_FACTOR_DELETE, token,
                                       {"factorId": added["factorId"]}), CMD_EXT_FACTOR_DELETE)
    require(removed["removed"] is True, CMD_EXT_FACTOR_DELETE,
            "刚加的因子没有任何历史引用 → 物理删除", removed)
    if superseded:
        require(removed["restoredFactorId"] == superseded, CMD_EXT_FACTOR_DELETE,
                f"前驱 {superseded} 的生效止已恢复", removed)

    after = expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}), CMD_EXT_FACTOR_LIST)["list"]
    require(len(after) == len(before), CMD_EXT_FACTOR_LIST,
            f"因子条数回到 {len(before)}", len(after))
    # 时间线必须逐格还原，否则撤销就等于换了一种方式破坏数据
    key = lambda rows: sorted((r["factorId"], r["effectFrom"], r["effectTo"], r["enabled"])
                              for r in rows)
    require(key(after) == key(before), CMD_EXT_FACTOR_DELETE,
            "撤销后因子时间线逐格还原", f"{key(before)} → {key(after)}")
    print(f"[PASS] 3747 撤销：物理删除并还原时间线（前驱 {removed['restoredFactorId']}）")


def validate_factor_disable_twice(client, token, date_from, date_to):
    """回归 2026-09-10 的 bug：对已停用的因子再点一次撤销（仅 --mutating）。

    原来会走两条错路：一是重跑时间线恢复，把前驱的生效止改第二遍；二是停用行
    留着旧的 effect_to，与被恢复到同一时刻的前驱一起被判成「多个因子闭合在同一
    时刻 = 破损」，撤下一个因子时直接 5001。这里把整条路铺完再逐项断言。
    """
    # 造一个「已被引用」的因子：查一次 3740 就会按它写下日聚合行，撤销时只能停用
    added = expect_ok(client.request(CMD_EXT_FACTOR_SET, token, {
        "region": "smoke-停用回归", "version": "smoke-disable-twice",
        "source": "回归用：被引用后只能停用的因子", "factorGPerKwh": 350,
        "effectFrom": f"{date_from} 00:00:00"}), CMD_EXT_FACTOR_SET)
    factor_id = added["factorId"]
    superseded = added.get("supersededFactorId", 0)
    expect_ok(client.request(CMD_EXT_CARBON_METRIC, token,
                             {"stationId": 0, "dateFrom": date_from, "dateTo": date_to}),
              CMD_EXT_CARBON_METRIC)

    first = expect_ok(client.request(CMD_EXT_FACTOR_DELETE, token, {"factorId": factor_id}),
                      CMD_EXT_FACTOR_DELETE)
    require(first["removed"] is False and first["disabled"] is True, CMD_EXT_FACTOR_DELETE,
            "已被日聚合引用 → 停用而非删除", first)
    require(first.get("alreadyDisabled") is False, CMD_EXT_FACTOR_DELETE,
            "首次撤销 alreadyDisabled == False", first)
    timeline = factor_timeline(client, token)

    second = expect_ok(client.request(CMD_EXT_FACTOR_DELETE, token, {"factorId": factor_id}),
                       CMD_EXT_FACTOR_DELETE)
    require(second.get("alreadyDisabled") is True, CMD_EXT_FACTOR_DELETE,
            "对已停用因子再撤一次 → 幂等空操作而非报错", second)
    require(second["restoredFactorId"] == 0, CMD_EXT_FACTOR_DELETE,
            "空操作不得再次恢复前驱", second)
    require(factor_timeline(client, token) == timeline, CMD_EXT_FACTOR_DELETE,
            "第二次撤销后时间线逐格不变", factor_timeline(client, token))
    print("[PASS] 3747 幂等：已停用因子再撤一次不报错、不改时间线")

    # 停用行还留在表里。此时撤销它的后继（前驱刚被恢复到同一时刻）曾误判为破损 → 5001
    tail = expect_ok(client.request(CMD_EXT_FACTOR_SET, token, {
        "region": "smoke-停用回归", "version": "smoke-disable-tail",
        "source": "回归用：停用行的后继", "factorGPerKwh": 360,
        "effectFrom": f"{date_to} 00:00:00"}), CMD_EXT_FACTOR_SET)
    undo_tail = expect_ok(client.request(CMD_EXT_FACTOR_DELETE, token,
                                         {"factorId": tail["factorId"]}), CMD_EXT_FACTOR_DELETE)
    require(undo_tail["removed"] is True, CMD_EXT_FACTOR_DELETE,
            "停用行的存在不应把后继的撤销打成 5001", undo_tail)
    print("[PASS] 3747 不再误判破损：停用行不参与时间线，后继照常撤销")

    # 清场：把这一轮造出来的东西连同日聚合一起删干净，--mutating 才能重复跑
    purged = expect_ok(client.request(CMD_EXT_FACTOR_PURGE, token, {"factorId": factor_id}),
                       CMD_EXT_FACTOR_PURGE)
    require(purged["dailyDeleted"] > 0, CMD_EXT_FACTOR_PURGE,
            "彻底删除应连同它算出的日聚合一起清掉", purged)
    if superseded:
        require(purged["restoredFactorId"] == 0, CMD_EXT_FACTOR_PURGE,
                "停用因子早已退出时间线，彻底删除不该再恢复一次前驱", purged)
    require(all(f["factorId"] != factor_id for f in
                expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}),
                          CMD_EXT_FACTOR_LIST)["list"]),
            CMD_EXT_FACTOR_PURGE, "彻底删除后因子行消失", purged)
    print(f"[PASS] 3748 彻底删除：因子行 + {purged['dailyDeleted']} 行日聚合"
          f" + {purged['reportsDeleted']} 份报告已永久清除")


def validate_factor_purge_guards(client, token, existing):
    """3748 的只读拒绝路径 —— 不改动任何因子。"""
    for payload in ({}, {"factorId": "1"}, {"factorId": 0}, {"factorId": -3}):
        response = client.request(CMD_EXT_FACTOR_PURGE, token, payload)
        require(response["code"] == ERR_PARAM, CMD_EXT_FACTOR_PURGE,
                f"入参 {payload} → ERR_PARAM({ERR_PARAM})", response["code"])
    response = client.request(CMD_EXT_FACTOR_PURGE, token, {"factorId": 999999})
    require(response["code"] == ERR_CARBON_FACTOR_NOT_FOUND, CMD_EXT_FACTOR_PURGE,
            f"不存在的 factorId → {ERR_CARBON_FACTOR_NOT_FOUND}", response["code"])
    enabled = [f for f in existing if f["enabled"]]
    if len(enabled) == 1:
        response = client.request(CMD_EXT_FACTOR_PURGE, token,
                                  {"factorId": enabled[0]["factorId"]})
        require(response["code"] == ERR_CARBON_LAST_FACTOR, CMD_EXT_FACTOR_PURGE,
                f"删最后一个启用因子 → {ERR_CARBON_LAST_FACTOR}", response["code"])
        print("[PASS] 3748 拒绝路径：非法入参、不存在(6705)、最后一个启用因子(6706)")
    else:
        print("[PASS] 3748 拒绝路径：非法入参、不存在(6705)　[SKIP] 6706（当前有多个启用因子）")


def factor_timeline(client, token):
    """因子表的可比较快照，用于断言「时间线逐格不变」。"""
    rows = expect_ok(client.request(CMD_EXT_FACTOR_LIST, token, {}), CMD_EXT_FACTOR_LIST)["list"]
    return sorted((r["factorId"], r["effectFrom"], r["effectTo"], r["enabled"]) for r in rows)


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
    # 没有数据 ≠ 没有因子：该范围照样归某个因子管辖，必须报得出来。
    # 这里为空说明因子版本是按聚合行取的，而不是按日取的 —— 那条路会让
    # 3743 往 NOT NULL 列里绑 null（空 QStringList 的 join 是 null 而非空串）。
    require(bool(data["factorVersion"]), CMD_EXT_CARBON_METRIC,
            "无数据时仍报出管辖因子版本", repr(data["factorVersion"]))
    print(f"[PASS] 空日期范围：空列表、完整度与强度均为 -1，仍报出因子 "
          f"{data['factorVersion']!r}")


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
    parser.add_argument("--mutating", action="store_true",
                        help="在只读检查全部通过后，额外跑 3742 真实写入测试（会新增因子行）")
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
        factors = validate_factor_list(client, token)
        validate_no_factor(client, token)
        validate_factor_set_guards(client, token, factors)
        validate_factor_delete_guards(client, token, factors)
        validate_factor_purge_guards(client, token, factors)
        if args.mutating:
            validate_factor_delete_write(client, token)
            validate_factor_set_write(client, token, factors)
        validate_empty_range(client, token)
        validate_report_guards(client, token)

        print(f"\n查询范围：{date_from} ~ {date_to}")
        metric = validate_idempotent(client, token, date_from, date_to)
        if stations:
            validate_station_split(client, token, date_from, date_to, stations)
        if db_ok:
            validate_against_db(args.db, metric, date_from, date_to)
        else:
            print(f"[SKIP] 未找到 {args.db}，跳过 SQLite 对拍")
        if args.mutating:
            validate_report_write(client, token, date_from, date_to)
            validate_factor_disable_twice(client, token, date_from, date_to)
        else:
            print("[SKIP] 3743/3744 写入测试需 --mutating（会生成报告行与导出文件）")
            print("[SKIP] 3747 幂等 / 3748 彻底删除写入测试需 --mutating")

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

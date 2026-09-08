#!/usr/bin/env python3
"""
ml/carbon_crosscheck.py  —  扩展模块 08 碳排放对拍　归属 L5

**从 t_order 独立重算一遍分时分摊与排放，与服务端写下的 t_carbon_daily 逐格比对。**

为什么值得单写一份：docs/expand/08-实现规划.md 裁决 D1 定了「聚合在 C++，Python 只做
图表与对拍」。既然两边都要读同一批订单，那就让 Python 完全不复用 C++ 的任何逻辑，
自己把算式重写一遍 —— 两份独立实现算出同一个数，才叫对上了；
共用一个函数再比一次，只能证明这个函数被调用了两次。

⚠ 全程只读（sqlite3 URI mode=ro），不写任何表，符合 ml/CLAUDE.md 的运行期脚本权限。
⚠ 口径必须与 server/biz/ext_08_carbon_calc.cpp 一致，改任何一边都要同步改另一边：
    · 峰谷时段读 t_sys_config，平段 = 24h 扣掉峰谷
    · 分摊按重叠秒数 + 最大余数法，并列时固定顺序 峰 > 平 > 谷
    · emission_g = (kwh_x100 * factor + 50) // 100
    · 日归属按 date(settle_time) 整单归一天；峰平谷按真实钟点切分，可跨两天（裁决 D3）

用法：python3 ml/carbon_crosscheck.py [charging.db]
退出码：0 = 逐格一致　2 = 有不一致　1 = 无法执行（缺表、缺因子等）
"""
import sqlite3
import sys
from datetime import datetime, timedelta
from pathlib import Path

DB = Path(sys.argv[1] if len(sys.argv) > 1 else "charging.db")
FMT = "%Y-%m-%d %H:%M:%S"
ALGO_VERSION = "carbon-v1"
KWH_X100_MAX = 100_000_000_000        # 与 C++ 侧 KWH_X100_MAX 一致
SESSION_SEC_MAX = 7 * 24 * 3600       # 与 C++ 侧 SESSION_SEC_MAX 一致
ORDER_SETTLED = 3

MAX_REPORTED = 10                     # 最多列出几行差异，避免刷屏


def die(msg, code=1):
    print(f"[FAIL] {msg}")
    print("\nRESULT: FAIL")
    sys.exit(code)


def parse_clock(text):
    """'HH:MM' → 当日 0 点起的秒数。'24:00' 合法，只能作区间右端。"""
    hh, _, mm = text.strip().partition(":")
    h, m = int(hh), int(mm)
    if not (0 <= h <= 24 and 0 <= m <= 59):
        raise ValueError(f"时刻非法: {text}")
    sec = h * 3600 + m * 60
    if sec > 86400:
        raise ValueError(f"时刻越界: {text}")
    return sec


def parse_ranges(text):
    """'HH:MM-HH:MM,...' → 非回绕的 [from, to) 片段列表。跨午夜就地拆两片。"""
    slices = []
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        a_text, _, b_text = item.partition("-")
        a, b = parse_clock(a_text), parse_clock(b_text)
        if a == b:
            raise ValueError(f"时段长度为零: {item}")
        if a < b:
            slices.append((a, b))
        else:                                   # 跨午夜，如 23:00-07:00
            if a < 86400:
                slices.append((a, 86400))
            if b > 0:
                slices.append((0, b))
    if not slices:
        raise ValueError("时段配置为空")
    return slices


def overlap_seconds(start, end, slices):
    """[start, end) 与各时段片的重叠秒数，按自然日推进逐日求交。"""
    total = 0
    day = start.date()
    while day <= end.date():
        day0 = datetime.combine(day, datetime.min.time())
        for a, b in slices:
            lo = max(start, day0 + timedelta(seconds=a))
            hi = min(end, day0 + timedelta(seconds=b))
            if hi > lo:
                total += int((hi - lo).total_seconds())
        day += timedelta(days=1)
    return total


def split_order(start, end, kwh, peak_slices, valley_slices):
    """→ (peak, flat, valley, unalloc, allocatable)；四项之和恒等于 kwh。"""
    if kwh < 0 or kwh > KWH_X100_MAX:
        return 0, 0, 0, 0, False              # 脏数据整单拒绝，不计入任何项
    if start is None or end is None:
        return 0, 0, 0, kwh, False
    dur = int((end - start).total_seconds())
    if dur <= 0 or dur > SESSION_SEC_MAX:
        return 0, 0, 0, kwh, False
    if kwh == 0:
        return 0, 0, 0, 0, True

    d_peak = overlap_seconds(start, end, peak_slices)
    d_valley = overlap_seconds(start, end, valley_slices)
    d_flat = dur - d_peak - d_valley
    if d_flat < 0:
        return 0, 0, 0, kwh, False

    # 最大余数法。并列时固定顺序 峰(0) > 平(1) > 谷(2)，结果才可复现
    raws, rems = [], []
    for d in (d_peak, d_flat, d_valley):
        n = kwh * d
        raws.append(n // dur)
        rems.append(n - (n // dur) * dur)
    leftover = kwh - sum(raws)
    for i in sorted(range(3), key=lambda i: (-rems[i], i))[:leftover]:
        raws[i] += 1
    return raws[0], raws[1], raws[2], 0, True


def pick_factor(factors, at_text):
    """左闭右开 [effect_from, effect_to)，effect_to 为 NULL 表示右开无穷。"""
    at = datetime.strptime(at_text, FMT)
    for version, eff_from, eff_to, g in factors:
        if at < datetime.strptime(eff_from, FMT):
            continue
        if eff_to and at >= datetime.strptime(eff_to, FMT):
            continue
        return version, g
    return None, None


def parse_time(text):
    if not text:
        return None
    try:
        return datetime.strptime(text, FMT)
    except ValueError:
        return None


def main():
    print("扩展模块 08 碳排放对拍 —— Python 独立重算 vs t_carbon_daily\n")
    if not DB.is_file():
        die(f"找不到数据库 {DB}")
    conn = sqlite3.connect(f"file:{DB.resolve()}?mode=ro", uri=True)

    try:
        cfg = dict(conn.execute(
            "SELECT cfg_key, cfg_value FROM t_sys_config"
            " WHERE cfg_key IN ('carbon_peak_ranges','carbon_valley_ranges')"))
        peak_slices = parse_ranges(cfg["carbon_peak_ranges"])
        valley_slices = parse_ranges(cfg["carbon_valley_ranges"])
    except (sqlite3.Error, KeyError, ValueError) as exc:
        die(f"读取峰谷时段配置失败（先执行 docs/db-schema-ext-08.sql）: {exc}")
    print(f"峰段 {cfg['carbon_peak_ranges']}　谷段 {cfg['carbon_valley_ranges']}"
          f"　平段 = 24h 扣除峰谷")

    factors = list(conn.execute(
        "SELECT version, effect_from, effect_to, factor_g_per_kwh"
        " FROM t_carbon_factor WHERE enabled = 1 ORDER BY effect_from DESC"))
    if not factors:
        die("没有启用的排放因子")

    rows = list(conn.execute(
        "SELECT station_id, stat_date, total_kwh_x100, peak_kwh_x100, flat_kwh_x100,"
        " valley_kwh_x100, unalloc_kwh_x100, order_cnt, emission_g, factor_version"
        " FROM t_carbon_daily WHERE algo_version = ?", (ALGO_VERSION,)))
    if not rows:
        print("[SKIP] t_carbon_daily 尚无本算法版本的行，先用管理端查一次或调 3745")
        print("\nRESULT: PASS")
        return 0

    # ---- 独立重算 ----
    acc = {}
    for sid, kwh, st, et, stat_date in conn.execute(
            "SELECT station_id, kwh_x100, start_time, end_time, date(settle_time)"
            " FROM t_order WHERE status = ?", (ORDER_SETTLED,)):
        p, f, v, u, _ = split_order(parse_time(st), parse_time(et), kwh,
                                    peak_slices, valley_slices)
        a = acc.setdefault((sid, stat_date), [0, 0, 0, 0, 0])
        a[0] += p + f + v + u                 # 总量取分摊结果之和，与服务端一致
        a[1] += p
        a[2] += f
        a[3] += v
        a[4] += 1
    print(f"独立重算 {len(acc)} 个（站, 日）组合；库中 {len(rows)} 行\n")

    # ---- 逐格比对 ----
    bad = 0
    for (sid, stat_date, tot, pk, fl, va, un, cnt, em, fver) in rows:
        want = acc.get((sid, stat_date))
        if want is None:
            bad += 1
            if bad <= MAX_REPORTED:
                print(f"  [差异] 站 {sid} {stat_date}：库中有行，订单里没有对应数据")
            continue
        _, g = pick_factor(factors, f"{stat_date} 00:00:00")
        if g is None:
            bad += 1
            if bad <= MAX_REPORTED:
                print(f"  [差异] 站 {sid} {stat_date}：该日无生效因子，库中却有行")
            continue
        want_em = (want[0] * g + 50) // 100
        got = [tot, pk, fl, va, un, cnt, em]
        exp = [want[0], want[1], want[2], want[3], 0, want[4], want_em]
        if got != exp:
            bad += 1
            if bad <= MAX_REPORTED:
                print(f"  [差异] 站 {sid} {stat_date} 因子 {fver}\n"
                      f"          库中 总{tot} 峰{pk} 平{fl} 谷{va} 未分摊{un} 单{cnt} 排放{em}\n"
                      f"          独算 总{exp[0]} 峰{exp[1]} 平{exp[2]} 谷{exp[3]}"
                      f" 未分摊{exp[4]} 单{exp[5]} 排放{exp[6]}")

    missing = len(acc) - (len(rows) - bad)
    if bad > MAX_REPORTED:
        print(f"  …另有 {bad - MAX_REPORTED} 行差异未列出")

    print(f"\n比对 {len(rows)} 行，不一致 {bad} 行")
    if bad == 0 and missing > 0:
        # 库里少了一些（站, 日）组合。可能是这些日期还没查过（懒聚合尚未触发），
        # 不算错，但要说出来，免得「零不一致」被当成「全都算过了」
        print(f"[注意] 有 {missing} 个（站, 日）组合尚未落库 —— 查询这些日期时会自动补算")
    if bad:
        print("\nRESULT: FAIL")
        return 2
    print("\nRESULT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())

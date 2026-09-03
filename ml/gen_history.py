#!/usr/bin/env python3
"""
ml/gen_history.py  —  历史数据生成器　归属 L5

大屏与时序模型共同的燃料：当前 charging.db 的 t_order / t_pile_log 为空，
大屏营收趋势与站点排行两张图全零，模型没有任何训练数据。

⚠ 权限边界（docs/conventions.md CR-002）：
    落库（--commit）需 L2（t_order/t_pile_log/t_pile 属主）批准。
    批准前只应使用默认 dry-run 预览，或对 charging.db 的副本验证 --commit 路径。
    只写：t_order（INSERT）/ t_pile_log（INSERT）/ t_pile.charge_count,charge_duration（UPDATE 聚合重算）
    不碰：t_user / t_station / t_admin / t_wallet_tx / t_sys_config；不改任何表结构

天气/节假日特征在 t_order 表中没有对应列（db-schema.sql 冻结，不能加列），
因此按天写入独立的 ml/data/day_features.csv，供后续特征工程按日期 join。

用法：
    python3 ml/gen_history.py                          # dry-run，只打印统计，不写 db
    python3 ml/gen_history.py --commit                  # 落库（追加）
    python3 ml/gen_history.py --commit --reset          # 先清空历史区间再落库（仅限开发库）
    python3 ml/gen_history.py charging.db --days 90 --commit
"""
import argparse
import csv
import math
import random
import sqlite3
import sys
from datetime import date, datetime, timedelta
from pathlib import Path

DB_DEFAULT = Path("charging.db")
DAY_FEATURES_OUT = Path("ml/data/day_features.csv")

# 简化节假日日历（演示用，非官方）：周末 + 几个示例法定节假日
SAMPLE_HOLIDAYS = {"01-01", "05-01", "10-01", "10-02", "10-03"}

# (名称, 概率, 需求折损系数)
WEATHERS = [
    ("晴", 0.45, 1.00),
    ("多云", 0.25, 0.97),
    ("阴", 0.15, 0.93),
    ("小雨", 0.10, 0.85),
    ("大雨", 0.05, 0.65),
]

# 24 小时负荷权重：早高峰 8-9，晚高峰 18-21，凌晨低谷
HOUR_WEIGHTS = [
    2, 1, 1, 1, 1, 2,
    4, 8, 12, 9, 6, 6,
    7, 6, 5, 5, 6, 8,
    12, 14, 11, 7, 5, 3,
]


def parse_args():
    ap = argparse.ArgumentParser(description="生成历史充电订单，供大屏与时序模型训练使用")
    ap.add_argument("db", nargs="?", default=str(DB_DEFAULT))
    ap.add_argument("--days", type=int, default=60, help="生成过去多少天历史，不含今天（默认 60）")
    ap.add_argument("--commit", action="store_true", help="实际写库；缺省仅 dry-run 预览")
    ap.add_argument("--reset", action="store_true", help="配合 --commit：先清空历史区间内的 t_order/t_pile_log")
    ap.add_argument("--seed", type=int, default=42, help="随机种子，便于复现")
    return ap.parse_args()


def is_holiday(d: date) -> bool:
    return d.weekday() >= 5 or d.strftime("%m-%d") in SAMPLE_HOLIDAYS


def pick_weather(rng: random.Random):
    r, acc = rng.random(), 0.0
    for name, p, factor in WEATHERS:
        acc += p
        if r <= acc:
            return name, factor
    return WEATHERS[-1][0], WEATHERS[-1][2]


def poisson(rng: random.Random, lam: float) -> int:
    # Knuth 算法，避免引入 numpy 依赖
    if lam <= 0:
        return 0
    l, k, p = math.exp(-lam), 0, 1.0
    while True:
        k += 1
        p *= rng.random()
        if p <= l:
            return k - 1


def fetch_context(con: sqlite3.Connection):
    stations = dict(con.execute("SELECT station_id, price FROM t_station").fetchall())
    piles = con.execute("SELECT pile_id, station_id, type, power FROM t_pile").fetchall()
    users = [r[0] for r in con.execute("SELECT user_id FROM t_user").fetchall()]
    if not stations or not piles or not users:
        print("t_station / t_pile / t_user 为空，请先执行 docs/db-schema.sql 建库种子数据", file=sys.stderr)
        sys.exit(1)
    piles_by_station = {}
    for pile_id, station_id, ptype, power in piles:
        piles_by_station.setdefault(station_id, []).append((pile_id, ptype, power))
    return stations, piles_by_station, users


def gen_session(rng: random.Random, ptype: int, power: float):
    minutes = rng.uniform(20, 60) if ptype == 0 else rng.uniform(120, 480)
    kwh = power * (minutes / 60.0) * rng.uniform(0.75, 0.95)  # 非满功率折损 + 充电效率
    return minutes, max(1, round(kwh * 100))  # 返回 (时长分钟, kwh_x100)


def make_order_no(dt: datetime, rng: random.Random, used: set) -> str:
    for _ in range(20):
        cand = f"ORD{dt.strftime('%Y%m%d%H%M%S')}{rng.randint(0, 9999):04d}"
        if cand not in used:
            used.add(cand)
            return cand
    raise RuntimeError("order_no 生成冲突过多，检查随机种子")


def build_dataset(days: int, seed: int, stations, piles_by_station, users):
    """纯内存生成，不接触数据库。返回 orders / pile_logs / day_features 三个列表。"""
    rng = random.Random(seed)
    today_mid = datetime.combine(datetime.now().date(), datetime.min.time())
    window_start = (today_mid - timedelta(days=days)).date()

    orders, pile_logs, day_features, used_no = [], [], [], set()

    d = window_start
    while d < today_mid.date():
        holiday = is_holiday(d)
        weather, wfactor = pick_weather(rng)
        weekday_factor = 0.75 if d.weekday() >= 5 else 1.0
        holiday_factor = 1.15 if holiday else 1.0
        day_features.append({
            "date": d.isoformat(), "weekday": d.weekday(), "is_holiday": int(holiday),
            "weather": weather, "weather_factor": round(wfactor, 2),
        })

        for station_id, piles in piles_by_station.items():
            price = stations.get(station_id)
            if price is None:
                continue
            expected = 1.6 * len(piles) * weekday_factor * holiday_factor * wfactor
            n = poisson(rng, expected)
            for _ in range(n):
                hour = rng.choices(range(24), weights=HOUR_WEIGHTS)[0]
                minute = rng.randint(0, 59)
                reserve_dt = datetime.combine(d, datetime.min.time()) + timedelta(hours=hour, minutes=minute)
                pile_id, ptype, power = rng.choice(piles)
                user_id = rng.choice(users)
                order_no = make_order_no(reserve_dt, rng, used_no)

                # 历史订单只落两种终态：已结算/已取消。
                # 绝不能残留 0/1/2（预约中/充电中/待结算）——1201 会把这些当成
                # “未完成订单”强制拦截，测试者一登录就被弹窗堵死，还以为链路坏了。
                # 预约时刻已贴着生成区间上界（今天零点），跳过——慢充最长 8 小时，
                # 硬凑一条会把 reserve_time 都推到今天，和红线自检的判定边界打架。
                if reserve_dt >= today_mid - timedelta(minutes=15):
                    continue

                if rng.random() < 0.9:
                    start_dt = reserve_dt + timedelta(minutes=rng.uniform(1, 10))
                    minutes, kwh_x100 = gen_session(rng, ptype, power)
                    end_dt = start_dt + timedelta(minutes=minutes)
                    if end_dt >= today_mid:
                        # 慢充会话跨过零点：截断到区间内，而不是让历史数据"溢出"到今天。
                        # 今天这条线正是红线自检用来分辨"真实数据"的分界，历史数据不能碰它。
                        capped = (today_mid - start_dt).total_seconds() / 60 - 1
                        if capped < 5:
                            continue
                        kwh_x100 = max(1, round(kwh_x100 * capped / minutes))
                        minutes = capped
                        end_dt = start_dt + timedelta(minutes=minutes)
                    settle_dt = min(end_dt + timedelta(minutes=rng.uniform(0, 5)),
                                     today_mid - timedelta(seconds=1))
                    amount = round(price * kwh_x100 / 100)
                    orders.append((order_no, user_id, pile_id, station_id, 3, price, kwh_x100, amount,
                                    reserve_dt, start_dt, end_dt, settle_dt))
                else:
                    orders.append((order_no, user_id, pile_id, station_id, 4, price, 0, 0,
                                    reserve_dt, None, None, None))
        d += timedelta(days=1)

    for station_id, piles in piles_by_station.items():
        for pile_id, ptype, power in piles:
            onboard = datetime.combine(window_start, datetime.min.time())
            pile_logs.append((pile_id, 0, None, 1, "system", "历史数据：首次上线", onboard))
            if rng.random() < 0.3:
                reboot_dt = onboard + timedelta(days=rng.uniform(1, max(days - 1, 1)))
                pile_logs.append((pile_id, 3, None, None, "admin", "历史数据：模拟远程重启", reboot_dt))

    return orders, pile_logs, day_features, window_start


def self_check(con: sqlite3.Connection, window_end: datetime) -> int:
    end_str = window_end.strftime("%Y-%m-%d %H:%M:%S")
    row = con.execute(
        "SELECT COUNT(*) FROM t_order WHERE "
        "(settle_time IS NOT NULL AND settle_time >= ?) OR "
        "(settle_time IS NULL AND reserve_time >= ?)",
        (end_str, end_str),
    ).fetchone()
    return row[0]


def apply_reset(con: sqlite3.Connection, window_end: datetime):
    end_str = window_end.strftime("%Y-%m-%d %H:%M:%S")
    con.execute(
        "DELETE FROM t_order WHERE "
        "(settle_time IS NOT NULL AND settle_time < ?) OR "
        "(settle_time IS NULL AND reserve_time < ?)",
        (end_str, end_str),
    )
    con.execute("DELETE FROM t_pile_log WHERE create_time < ?", (end_str,))


def write_db(con: sqlite3.Connection, orders, pile_logs):
    fmt = lambda dt: dt.strftime("%Y-%m-%d %H:%M:%S") if dt else None
    con.executemany(
        "INSERT INTO t_order (order_no, user_id, pile_id, station_id, status, price, "
        "kwh_x100, amount, reserve_time, start_time, end_time, settle_time) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?)",
        [(no, uid, pid, sid, st, price, kwh, amt, fmt(rt), fmt(stt), fmt(et), fmt(settle))
         for no, uid, pid, sid, st, price, kwh, amt, rt, stt, et, settle in orders],
    )
    con.executemany(
        "INSERT INTO t_pile_log (pile_id, event, old_status, new_status, operator, detail, create_time) "
        "VALUES (?,?,?,?,?,?,?)",
        [(pid, ev, olds, news, op, detail, fmt(ct))
         for pid, ev, olds, news, op, detail, ct in pile_logs],
    )


def recompute_pile_aggregates(con: sqlite3.Connection):
    # 用 SET 而非 += 重新聚合，保证脚本可重复执行（无论 dry-run/commit 反复跑多少次）不会重复累加。
    rows = con.execute(
        "SELECT pile_id, COUNT(*), "
        "IFNULL(SUM((julianday(end_time) - julianday(start_time)) * 86400.0), 0) "
        "FROM t_order WHERE status = 3 GROUP BY pile_id"
    ).fetchall()
    stats = {pid: (cnt, int(round(dur))) for pid, cnt, dur in rows}
    all_piles = [r[0] for r in con.execute("SELECT pile_id FROM t_pile").fetchall()]
    con.executemany(
        "UPDATE t_pile SET charge_count = ?, charge_duration = ? WHERE pile_id = ?",
        [(stats.get(pid, (0, 0))[0], stats.get(pid, (0, 0))[1], pid) for pid in all_piles],
    )


def write_day_features(rows):
    DAY_FEATURES_OUT.parent.mkdir(parents=True, exist_ok=True)
    with DAY_FEATURES_OUT.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["date", "weekday", "is_holiday", "weather", "weather_factor"])
        w.writeheader()
        w.writerows(rows)


def summarize(orders, pile_logs, days):
    settled = sum(1 for o in orders if o[4] == 3)
    cancelled = len(orders) - settled
    total_amount = sum(o[7] for o in orders) / 100.0
    total_kwh = sum(o[6] for o in orders) / 100.0
    print(f"生成区间：过去 {days} 天（不含今天）")
    print(f"订单：共 {len(orders)} 条（已结算 {settled} / 已取消 {cancelled}）")
    print(f"合计电量 {total_kwh:.1f} 度，合计金额 {total_amount:.2f} 元")
    print(f"设备日志：{len(pile_logs)} 条")


def main() -> int:
    args = parse_args()
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"数据库不存在：{db_path}\n请先执行：sqlite3 {db_path} < docs/db-schema.sql", file=sys.stderr)
        return 1

    # dry-run 强制只读连接：即便代码有 bug 也不可能写库，这是结构性保证而非靠自觉。
    if args.commit:
        con = sqlite3.connect(str(db_path))
    else:
        con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)

    stations, piles_by_station, users = fetch_context(con)
    orders, pile_logs, day_features, window_start = build_dataset(
        args.days, args.seed, stations, piles_by_station, users)
    window_end = datetime.combine(datetime.now().date(), datetime.min.time())

    dirty = self_check(con, window_end)
    if dirty:
        msg = (f"检测到 {dirty} 条 settle_time/reserve_time 落在今日或以后的订单，"
               f"怀疑库中已有真实联调数据。")
        if args.commit:
            print(msg + " 已中止，不执行任何写入。", file=sys.stderr)
            con.close()
            return 2
        print("⚠ " + msg + "（dry-run 继续预览，--commit 时会中止）")

    summarize(orders, pile_logs, args.days)

    if not args.commit:
        print("\ndry-run：未写入数据库。加 --commit 才会落库（需 CR-002 批准，见 docs/conventions.md）。")
        write_day_features(day_features)
        print(f"已写出 {DAY_FEATURES_OUT}（不涉及 charging.db，无需审批）")
        con.close()
        return 0

    if args.reset:
        apply_reset(con, window_end)
    try:
        write_db(con, orders, pile_logs)
        recompute_pile_aggregates(con)
        con.commit()
    except sqlite3.IntegrityError as e:
        con.rollback()
        # 最常见诱因：不带 --reset 重跑，同一个 --seed 生成了同一批 order_no。
        print(f"写入失败（已回滚）：{e}", file=sys.stderr)
        print("多半是重跑没加 --reset，同一 --seed 生成了同一批订单号。"
              "要么加 --reset 清空历史区间重来，要么换一个 --seed 再追加一批。", file=sys.stderr)
        return 3
    except Exception:
        con.rollback()
        raise
    finally:
        con.close()

    write_day_features(day_features)
    print(f"\n已落库 {db_path}，并写出 {DAY_FEATURES_OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""
ml/gen_history.py  —  历史数据生成器　归属 L5

大屏与时序模型共同的燃料：当前 charging.db 的 t_order / t_pile_log 为空，
大屏营收趋势与站点排行两张图全零，模型没有任何训练数据。

⚠ 权限边界（docs/conventions.md CR-002，L2 已于 2026-09-04 批复）：
    只写：t_order（INSERT）/ t_pile_log（INSERT）/ t_pile.charge_count,charge_duration（UPDATE 聚合重算）
    不碰：t_user / t_station / t_admin / t_wallet_tx / t_sys_config；不改任何表结构
    金额一律整数分，与 1204 结算规则一致（批复第 1 条）
    --reset 只允许对可丢弃副本执行，脚本硬性拒绝 charging.db（批复第 2 条）；
        且只删 ml/data/seed_manifest.json 记录过的批次，绝不按时间区间盲删
    播种批次由 ML 侧的 seed_manifest.json 维护，不写 t_sys_config（批复第 3 条）
    对 charging.db 落库前：停服务 → 备份 → 先 dry-run，执行人限 L5/SCML

天气/节假日特征在 t_order 表中没有对应列（db-schema.sql 冻结，不能加列），
因此按天写入独立的 ml/data/day_features.csv，供后续特征工程按日期 join。

用法：
    python3 ml/gen_history.py                          # dry-run，只打印统计，不写 db
    python3 ml/gen_history.py --commit                  # 落库（追加）
    python3 ml/gen_history.py ml/data/dev.db --commit --reset   # 按播种记录清除后重播（仅限副本）
    python3 ml/gen_history.py charging.db --days 90 --commit
"""
import argparse
import bisect
import csv
import json
import math
import random
import sqlite3
import sys
from datetime import date, datetime, timedelta
from pathlib import Path

DB_DEFAULT = Path("charging.db")
DAY_FEATURES_OUT = Path("ml/data/day_features.csv")
SEED_MANIFEST = Path("ml/data/seed_manifest.json")

# CR-002 批复第 2 条：--reset 只允许在可丢弃的演示副本上执行，
# 不得对共享开发库 / 正式库 / 含成员测试数据的 charging.db 执行。
RESET_PROTECTED = {"charging.db"}

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

# 天气持续性：以 WEATHER_PERSISTENCE 的概率沿用昨天，否则按上表重抽。
# 这样构造的转移矩阵 P(i→j) = α·[i=j] + (1−α)·π_j，平稳分布恰好还是 π（可验证
# Σ_i π_i·P(i,j) = π_j），也就是说加持续性不会把晴雨天的长期占比带偏。
# 为什么必须改：独立抽样下「昨天下雨」对今天毫无信息量，滞后特征学不到东西；
# 真实天气是连续过程，一场连阴雨会连着压低好几天的充电需求。
WEATHER_PERSISTENCE = 0.55

# ---- 站点画像 [说明书] 1.4「各站点」---------------------------------------
# 改造前六个站共用同一条 HOUR_WEIGHTS，除电桩数外没有任何区别，
# 「分站预测」在数据层面是空的——模型能学到的只有那条写死的常量曲线。
#
# 归类依据是站名语义。用关键字匹配而不是写死 station_id：t_station 的种子数据
# 归 L2（db-schema.sql 冻结），将来加站时未命中的自动落到「混合型」，不会崩。
PROFILE_KEYWORDS = [
    (("福田", "科技园"), "office"),       # 办公型：工作日早高峰强，周末大幅回落
    (("宝安", "龙岗"), "residential"),    # 住宅型：晚间 19-23 强 + 慢充过夜，周末平缓
    (("公园",), "leisure"),               # 休闲型：周末午间强，工作日弱
]
DEFAULT_PROFILE = "mixed"                 # 混合型：介于办公与住宅之间

# 每种画像的 24 小时相对权重（工作日 / 周末各一条）与周末总量系数。
# 权重只决定「形状」，总量由 weekend_vol 与后面的归一化控制。
STATION_PROFILES = {
    "office": {
        "weekday": [1, 1, 1, 1, 1, 2, 5, 14, 28, 24, 14, 10,
                    9, 11, 10, 9, 9, 12, 14, 10, 6, 4, 3, 2],
        "weekend": [1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 6, 6,
                    6, 6, 6, 5, 5, 5, 5, 5, 4, 3, 2, 2],
        "weekend_vol": 0.45,
    },
    "residential": {
        "weekday": [6, 5, 4, 4, 3, 3, 4, 6, 7, 5, 4, 4,
                    5, 4, 4, 4, 5, 8, 14, 24, 26, 22, 16, 10],
        "weekend": [6, 5, 4, 4, 3, 3, 3, 4, 6, 8, 10, 11,
                    11, 10, 10, 10, 11, 13, 16, 20, 20, 17, 12, 8],
        "weekend_vol": 0.95,
    },
    "leisure": {
        "weekday": [1, 1, 1, 1, 1, 1, 2, 3, 4, 5, 6, 7,
                    7, 7, 7, 7, 8, 9, 10, 10, 8, 5, 3, 2],
        "weekend": [2, 1, 1, 1, 1, 1, 3, 6, 10, 16, 22, 26,
                    26, 24, 24, 24, 22, 18, 14, 11, 8, 5, 4, 3],
        "weekend_vol": 1.55,
    },
    "mixed": {
        "weekday": [3, 2, 2, 2, 2, 2, 4, 9, 16, 14, 10, 9,
                    9, 9, 9, 9, 9, 11, 15, 18, 16, 12, 8, 5],
        "weekend": [3, 2, 2, 2, 2, 2, 3, 5, 8, 11, 14, 15,
                    15, 14, 13, 13, 13, 14, 15, 15, 13, 9, 6, 4],
        "weekend_vol": 0.85,
    },
}

# AR(1) 潜在需求水平：level[t] = ρ·level[t−1] + ε，在对数空间上乘进当天期望。
# 这是「忙日成簇」的来源，也是 t−24h / t−168h 自相关的来源——改造前跨日相关系数
# 去季节后是 −0.03（等于没有），滞后特征全是噪声。
# 拆成「全市共同冲击」+「站点特有冲击」两层：前者制造站间同涨同跌（油价、限行、
# 大型活动），后者保证各站不是同一条曲线的复制品。
AR_RHO = 0.7
AR_SIGMA_CITY = 0.15
AR_SIGMA_STATION = 0.20

# EV 保有量年增速。60 天窗口上约 +6%，量级不大但给模型一点周期之外的东西。
TREND_ANNUAL = 0.45

# 法定节假日额外加成（周末效应已由各画像的 weekend_vol 承担，不重复计）
HOLIDAY_BOOST = 1.10

# 每根桩每天的**预约尝试数**，按桩型分档。分档而非统一速率的原因：
# 快充 20-60 分钟一单，一天跑 8 单也才占用 5 小时（利用率 22%）；
# 慢充 2-8 小时一单，跑 8 单需要 30 多个小时，物理上不可能。
# 改造前统一 1.6 的后果是快充桩每天只用 1.0 小时（利用率 4%），闲得不真实，
# 也正是分站 t−24h 自相关上不去的根因——每站每小时期望不到 0.25 单，
# 泊松噪声方差压过一切结构信号。
RATE_FAST = 10.0
RATE_SLOW = 2.5

# 总量归一化的锚。站点画像 / AR / 趋势只重新分配这些订单落在哪个站、哪一天、
# 哪个小时，不改变全局总量——否则大屏营收数字会相对基准整体漂移一个量级。
# 只缩放全局总量，不做逐站/逐日归一化：后者会把画像和 AR 的结构一起抹平，
# 而结构正是这次改造要造出来的东西。
LEGACY_WEEKEND_FACTOR = 0.75
LEGACY_HOLIDAY_FACTOR = 1.15

# 夜间偏慢充（回家过夜），白天偏快充（补电走人）。
# 不做这层区分的话，凌晨的慢充过夜负荷出不来，住宅型画像就只剩晚高峰一个鼓包。
NIGHT_SLOW_BIAS = 3.0
DAY_FAST_BIAS = 2.0


def parse_args():
    ap = argparse.ArgumentParser(description="生成历史充电订单，供大屏与时序模型训练使用")
    ap.add_argument("db", nargs="?", default=str(DB_DEFAULT))
    ap.add_argument("--days", type=int, default=60, help="生成过去多少天历史，不含今天（默认 60）")
    ap.add_argument("--commit", action="store_true", help="实际写库；缺省仅 dry-run 预览")
    ap.add_argument("--reset", action="store_true", help="配合 --commit：先清空历史区间内的 t_order/t_pile_log")
    ap.add_argument("--rate", type=float, default=1.0, dest="rate_mult",
                    help=f"需求全局倍率，作用于 RATE_FAST={RATE_FAST}/RATE_SLOW={RATE_SLOW}（默认 1.0）")
    ap.add_argument("--seed", type=int, default=42, help="随机种子，便于复现")
    return ap.parse_args()


def is_statutory(d: date) -> bool:
    """仅法定节假日，不含周末。

    改造前这个函数是「周末 or 法定节假日」，写进 day_features.csv 的 is_holiday
    因此和 weekday 列高度共线——weekday≥5 完全蕴含 is_holiday=1，等于白给一列。
    收窄成法定节假日后它才是一个独立特征。
    """
    return d.strftime("%m-%d") in SAMPLE_HOLIDAYS


def is_rest_day(d: date) -> bool:
    """休息日：周末或法定节假日，决定用哪条日内曲线。"""
    return d.weekday() >= 5 or is_statutory(d)


def resolve_profile(name: str) -> str:
    for keywords, profile in PROFILE_KEYWORDS:
        if any(k in name for k in keywords):
            return profile
    return DEFAULT_PROFILE


def weather_series(rng: random.Random, days: int):
    """马尔可夫天气序列，见 WEATHER_PERSISTENCE 的说明。"""
    def draw():
        r, acc = rng.random(), 0.0
        for name, p, factor in WEATHERS:
            acc += p
            if r <= acc:
                return name, factor
        return WEATHERS[-1][0], WEATHERS[-1][2]

    out, cur = [], draw()
    for _ in range(days):
        if out and rng.random() < WEATHER_PERSISTENCE:
            pass                      # 沿用昨天
        else:
            cur = draw()
        out.append(cur)
    return out


def ar1_series(rng: random.Random, days: int, sigma: float):
    """AR(1) 序列，从平稳分布起步（否则前几十天是一段无意义的暖机期）。"""
    stationary_sd = sigma / math.sqrt(1 - AR_RHO ** 2)
    level, out = rng.gauss(0.0, stationary_sd), []
    for _ in range(days):
        level = AR_RHO * level + rng.gauss(0.0, sigma)
        out.append(level)
    return out


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
    stations = {sid: (price, name) for sid, price, name in
                con.execute("SELECT station_id, price, name FROM t_station").fetchall()}
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


def rank_piles(rng: random.Random, piles, hour: int):
    """按小时偏好加权、无放回地给电桩排个优先顺序。

    夜间偏慢充（回家过夜），白天偏快充（补电走人）。不做这层区分的话，
    凌晨的慢充过夜负荷出不来，住宅型画像就只剩晚高峰一个鼓包。
    piles 元素为 (pile_id, ptype, power)，ptype 0=快充 1=慢充。
    """
    if hour >= 21 or hour <= 5:
        pool = [(p, NIGHT_SLOW_BIAS if p[1] == 1 else 1.0) for p in piles]
    elif 7 <= hour <= 18:
        pool = [(p, 1.0 if p[1] == 1 else DAY_FAST_BIAS) for p in piles]
    else:
        pool = [(p, 1.0) for p in piles]

    out = []
    while pool:
        r, acc = rng.random() * sum(w for _, w in pool), 0.0
        for idx, (pile, w) in enumerate(pool):
            acc += w
            if r <= acc:
                out.append(pile)
                pool.pop(idx)
                break
        else:
            out.append(pool.pop()[0])
    return out


def is_free(intervals, start_ts: float, end_ts: float) -> bool:
    """区间列表按起点有序，判断 [start_ts, end_ts) 是否与已有占用重叠。"""
    i = bisect.bisect_left(intervals, (start_ts, start_ts))
    if i > 0 and intervals[i - 1][1] > start_ts:
        return False
    if i < len(intervals) and intervals[i][0] < end_ts:
        return False
    return True


def place_session(rng: random.Random, candidates, reserve_dt, today_mid, busy):
    """在候选桩里找一根这段时间空闲的，返回落位结果；站点占满则返回 None。

    改造前不检查占用，同一根桩上可以并发任意多单，站点小时负荷能画到装机功率的
    两倍以上；而 t_load_forecast 要预测的 idle_pile / congestion 正是以「桩被占用」
    为语义前提的，没有这层约束只能靠 clamp 兜底，预测的是个假量。
    占满即取消也是真实行为：桩位排满，用户就走了。
    """
    start_dt = reserve_dt + timedelta(minutes=rng.uniform(1, 10))
    for pile_id, ptype, power in candidates:
        minutes, kwh_x100 = gen_session(rng, ptype, power)
        end_dt = start_dt + timedelta(minutes=minutes)
        if end_dt >= today_mid:
            # 慢充会话跨过零点：截断到区间内，而不是让历史数据"溢出"到今天。
            # 今天这条线正是红线自检用来分辨"真实数据"的分界，历史数据不能碰它。
            capped = (today_mid - start_dt).total_seconds() / 60 - 1
            if capped < 5:
                return "skip", None
            kwh_x100 = max(1, round(kwh_x100 * capped / minutes))
            minutes = capped
            end_dt = start_dt + timedelta(minutes=minutes)
        s_ts, e_ts = start_dt.timestamp(), end_dt.timestamp()
        if is_free(busy[pile_id], s_ts, e_ts):
            bisect.insort(busy[pile_id], (s_ts, e_ts))
            settle_dt = min(end_dt + timedelta(minutes=rng.uniform(0, 5)),
                            today_mid - timedelta(seconds=1))
            return "ok", (pile_id, kwh_x100, start_dt, end_dt, settle_dt)
    return "full", None


def build_dataset(days: int, seed: int, stations, piles_by_station, users, rate_mult: float = 1.0):
    """纯内存生成，不接触数据库。返回 orders / pile_logs / day_features / 统计 四项。"""
    rng = random.Random(seed)
    today_mid = datetime.combine(datetime.now().date(), datetime.min.time())
    window_start = (today_mid - timedelta(days=days)).date()
    dates = [window_start + timedelta(days=i) for i in range(days)]
    sids = [sid for sid in piles_by_station if sid in stations]

    weather = weather_series(rng, days)
    city_level = ar1_series(rng, days, AR_SIGMA_CITY)
    station_level = {sid: ar1_series(rng, days, AR_SIGMA_STATION) for sid in sids}
    profiles = {sid: resolve_profile(stations[sid][1]) for sid in sids}
    # 站点日承载力：快充桩与慢充桩按各自速率折算，这是总量归一化的基准刻度。
    capacity = {sid: rate_mult * sum(RATE_FAST if ptype == 0 else RATE_SLOW
                                     for _, ptype, _ in piles_by_station[sid])
                for sid in sids}

    # ---- 第一遍：只算相对期望，再整体缩放到目标总量 ----------------------
    raw, target_total = {}, 0.0
    for i, d in enumerate(dates):
        rest, stat = is_rest_day(d), is_statutory(d)
        _, wfactor = weather[i]
        trend = (1 + TREND_ANNUAL) ** (i / 365.0)
        for sid in sids:
            vol = STATION_PROFILES[profiles[sid]]["weekend_vol"] if rest else 1.0
            level = math.exp(city_level[i] + station_level[sid][i])
            # raw 只是「相对形状」，故意不乘 capacity 以外的量纲——目标总量在下面单独算。
            # 两边用同一套系数的话它们会在 scale 里约掉，旋钮就成了摆设
            # （这个坑踩过：五个不同 rate 输出逐字节一致）。
            raw[(i, sid)] = (capacity[sid] * vol * level * trend * wfactor
                             * (HOLIDAY_BOOST if stat else 1.0))
            target_total += (capacity[sid] * wfactor
                             * (LEGACY_WEEKEND_FACTOR if d.weekday() >= 5 else 1.0)
                             * (LEGACY_HOLIDAY_FACTOR if rest else 1.0))
    raw_total = sum(raw.values())
    scale = target_total / raw_total if raw_total > 0 else 1.0

    # ---- 第二遍：落订单 ----------------------------------------------------
    orders, pile_logs, day_features, used_no = [], [], [], set()
    busy = {pile_id: [] for piles in piles_by_station.values() for pile_id, _, _ in piles}
    stats = {"user_cancel": 0, "full_cancel": 0, "skipped": 0}

    for i, d in enumerate(dates):
        rest, stat = is_rest_day(d), is_statutory(d)
        wname, wfactor = weather[i]
        day_features.append({
            "date": d.isoformat(), "weekday": d.weekday(), "is_holiday": int(stat),
            "weather": wname, "weather_factor": round(wfactor, 2),
        })

        for sid in sids:
            price = stations[sid][0]
            piles = piles_by_station[sid]
            hour_weights = STATION_PROFILES[profiles[sid]]["weekend" if rest else "weekday"]
            for _ in range(poisson(rng, raw[(i, sid)] * scale)):
                hour = rng.choices(range(24), weights=hour_weights)[0]
                reserve_dt = (datetime.combine(d, datetime.min.time())
                              + timedelta(hours=hour, minutes=rng.randint(0, 59)))
                # 预约时刻已贴着生成区间上界（今天零点），跳过——慢充最长 8 小时，
                # 硬凑一条会把 reserve_time 都推到今天，和红线自检的判定边界打架。
                if reserve_dt >= today_mid - timedelta(minutes=15):
                    continue

                candidates = rank_piles(rng, piles, hour)
                order_no = make_order_no(reserve_dt, rng, used_no)

                # 历史订单只落两种终态：已结算/已取消。
                # 绝不能残留 0/1/2（预约中/充电中/待结算）——1201 会把这些当成
                # “未完成订单”强制拦截，测试者一登录就被弹窗堵死，还以为链路坏了。
                if rng.random() >= 0.9:                       # 用户主动取消
                    stats["user_cancel"] += 1
                    orders.append((order_no, rng.choice(users), candidates[0][0], sid, 4,
                                    price, 0, 0, reserve_dt, None, None, None))
                    continue

                outcome, placed = place_session(rng, candidates, reserve_dt, today_mid, busy)
                if outcome == "skip":
                    stats["skipped"] += 1
                    used_no.discard(order_no)
                    continue
                if outcome == "full":                          # 站点占满，用户走了
                    stats["full_cancel"] += 1
                    orders.append((order_no, rng.choice(users), candidates[0][0], sid, 4,
                                    price, 0, 0, reserve_dt, None, None, None))
                    continue

                pile_id, kwh_x100, start_dt, end_dt, settle_dt = placed
                # 金额一律整数分（CR-002 批复第 1 条：与 1204 结算的整数分规则一致）。
                # price×kwh_x100 的单位是「分×100」，+50 再整除 100 即四舍五入到分。
                # 不能写 round(x/100)——Python 的 round 是四舍六入五成双，且中间转浮点，
                # 与服务端整数运算对不上，对账时会差几分钱。
                amount = (price * kwh_x100 + 50) // 100
                orders.append((order_no, rng.choice(users), pile_id, sid, 3, price,
                                kwh_x100, amount, reserve_dt, start_dt, end_dt, settle_dt))

    for station_id, piles in piles_by_station.items():
        for pile_id, ptype, power in piles:
            onboard = datetime.combine(window_start, datetime.min.time())
            pile_logs.append((pile_id, 0, None, 1, "system", "历史数据：首次上线", onboard))
            if rng.random() < 0.3:
                reboot_dt = onboard + timedelta(days=rng.uniform(1, max(days - 1, 1)))
                pile_logs.append((pile_id, 3, None, None, "admin", "历史数据：模拟远程重启", reboot_dt))

    stats["busy_hours"] = {pid: sum(e - s for s, e in iv) / 3600.0 for pid, iv in busy.items()}
    return orders, pile_logs, day_features, window_start, stats


def self_check(con: sqlite3.Connection, window_end: datetime) -> int:
    end_str = window_end.strftime("%Y-%m-%d %H:%M:%S")
    row = con.execute(
        "SELECT COUNT(*) FROM t_order WHERE "
        "(settle_time IS NOT NULL AND settle_time >= ?) OR "
        "(settle_time IS NULL AND reserve_time >= ?)",
        (end_str, end_str),
    ).fetchone()
    return row[0]


def load_manifest() -> dict:
    if SEED_MANIFEST.exists():
        return json.loads(SEED_MANIFEST.read_text(encoding="utf-8"))
    return {"batches": []}


def record_batch(db_path: Path, order_nos, log_id_from: int, log_id_to: int):
    # CR-002 批复第 3 条：L2 不同意在 t_sys_config 加 data_seed_version（单一版本号
    # 既分不清哪些订单是脚本生成的，也保证不了 reset 安全），改由 ML 侧维护播种记录。
    m = load_manifest()
    m["batches"].append({
        "db": db_path.name,
        "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "order_count": len(order_nos),
        "pile_log_id_from": log_id_from,
        "pile_log_id_to": log_id_to,
        "order_no": sorted(order_nos),
    })
    SEED_MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    SEED_MANIFEST.write_text(json.dumps(m, ensure_ascii=False, indent=2), encoding="utf-8")


def apply_reset(con: sqlite3.Connection, db_path: Path) -> int:
    """只删本脚本播种过的行——按播种记录精确定位，绝不按时间区间盲删。

    CR-002 批复第 2 条要求 --reset 只在可丢弃的副本上跑。但「副本」是人的承诺，
    脚本挡不住手滑，所以这里再加一道：即便真在有成员测试数据的库上执行，
    没记进播种清单的行也一条都不会被碰。
    """
    m = load_manifest()
    mine = [b for b in m["batches"] if b["db"] == db_path.name]
    if not mine:
        print(f"播种记录里没有 {db_path.name} 的批次，--reset 无可删除内容"
              f"（不会退化成按时间区间盲删）")
        return 0

    nos = [(n,) for b in mine for n in b["order_no"]]
    con.executemany("DELETE FROM t_order WHERE order_no = ?", nos)
    deleted = con.total_changes
    for b in mine:
        # 必须用闭区间：只记起点、删 ">= 起点" 会连带删掉本批次之后别人插入的日志。
        # 本批次是单事务连续插入，区间内不会夹杂他人的行。
        con.execute("DELETE FROM t_pile_log WHERE log_id BETWEEN ? AND ?",
                    (b["pile_log_id_from"], b.get("pile_log_id_to", b["pile_log_id_from"])))

    m["batches"] = [b for b in m["batches"] if b["db"] != db_path.name]
    SEED_MANIFEST.write_text(json.dumps(m, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"已按播种记录清除 {len(nos)} 条订单及对应设备日志")
    return deleted


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
    """写出按天的天气/节假日特征。

    ⚠ 即便是 dry-run 也会写这个文件——它不属于数据库，不受 --commit 约束。
    但天气序列必须与库里已落的订单同源：`predict.py` 会 join 它来取目标时刻的天气。
    换了 --days / --seed 再跑一次（哪怕只是 dry-run），本文件就被换成另一条天气序列，
    而库里的订单还是老的，两者就对不上了，且不会有任何报错。
    所以这里在覆盖前比一次窗口长度，不一致就明确警告。
    """
    DAY_FEATURES_OUT.parent.mkdir(parents=True, exist_ok=True)
    if DAY_FEATURES_OUT.exists():
        old = sum(1 for _ in DAY_FEATURES_OUT.open(encoding="utf-8")) - 1
        if old != len(rows):
            print(f"⚠ {DAY_FEATURES_OUT} 的窗口由 {old} 天变为 {len(rows)} 天，天气序列已被替换。"
                  f"\n  它与库里订单的同源关系已断开——若要继续用，请按同一组 --days/--seed "
                  f"重新走一遍：build_features → train_forecast → predict。", file=sys.stderr)
    with DAY_FEATURES_OUT.open("w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=["date", "weekday", "is_holiday", "weather", "weather_factor"])
        w.writeheader()
        w.writerows(rows)


def summarize(orders, pile_logs, days, stats):
    settled = sum(1 for o in orders if o[4] == 3)
    total_amount = sum(o[7] for o in orders) / 100.0
    total_kwh = sum(o[6] for o in orders) / 100.0
    bh = stats["busy_hours"]
    avg_busy = sum(bh.values()) / len(bh) / days if bh else 0.0
    print(f"生成区间：过去 {days} 天（不含今天）")
    print(f"订单：共 {len(orders)} 条（已结算 {settled} / 用户取消 {stats['user_cancel']}"
          f" / 站点占满取消 {stats['full_cancel']}）")
    print(f"合计电量 {total_kwh:.1f} 度，合计金额 {total_amount:.2f} 元")
    print(f"电桩平均占用 {avg_busy:.1f} 小时/桩/天（利用率 {avg_busy / 24 * 100:.0f}%）")
    print(f"设备日志：{len(pile_logs)} 条")


def main() -> int:
    args = parse_args()
    db_path = Path(args.db)
    if not db_path.exists():
        print(f"数据库不存在：{db_path}\n请先执行：sqlite3 {db_path} < docs/db-schema.sql", file=sys.stderr)
        return 1

    if args.reset and db_path.name in RESET_PROTECTED:
        print(f"拒绝对 {db_path.name} 执行 --reset。", file=sys.stderr)
        print("CR-002 批复第 2 条：--reset 只允许在可丢弃的演示副本上执行，"
              "不得对共享开发库 / 正式库 / 含成员测试数据的库执行。\n"
              "如需重播，请先 cp 一份副本（例如 ml/data/dev.db）再对副本操作。", file=sys.stderr)
        return 4

    # dry-run 强制只读连接：即便代码有 bug 也不可能写库，这是结构性保证而非靠自觉。
    if args.commit:
        con = sqlite3.connect(str(db_path))
    else:
        con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)

    stations, piles_by_station, users = fetch_context(con)
    orders, pile_logs, day_features, window_start, stats = build_dataset(
        args.days, args.seed, stations, piles_by_station, users, args.rate_mult)
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

    summarize(orders, pile_logs, args.days, stats)

    if not args.commit:
        print(f"\ndry-run：未写入数据库。加 --commit 才会落库。"
              f"\n对 {DB_DEFAULT.name} 落库前请按 CR-002 批复：停服务 → 备份 → 先 dry-run。")
        write_day_features(day_features)
        print(f"已写出 {DAY_FEATURES_OUT}（不涉及 charging.db，无需审批）")
        con.close()
        return 0

    if args.reset:
        apply_reset(con, db_path)
    try:
        log_id_from = (con.execute("SELECT IFNULL(MAX(log_id), 0) FROM t_pile_log").fetchone()[0]) + 1
        write_db(con, orders, pile_logs)
        recompute_pile_aggregates(con)
        log_id_to = con.execute("SELECT IFNULL(MAX(log_id), 0) FROM t_pile_log").fetchone()[0]
        con.commit()
        record_batch(db_path, [o[0] for o in orders], log_id_from, log_id_to)
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

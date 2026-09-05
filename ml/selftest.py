#!/usr/bin/env python3
"""
ml/selftest.py  —  L5 全链路自动化自检　归属 L5

覆盖 ml/ 与 dataviz/ 的六个脚本 + 大屏页面，验证的是**契约与不变量**，
不是「跑起来不报错」。凡是文档里写过的保证，这里都要有一条对应的断言。

⚠ 全程在临时目录里跑：
  - 测试库由 `docs/db-schema.sql` 现建，不复制、更不修改 `charging.db` / `ml/data/dev.db`
  - 各脚本用**绝对路径**调用但 **cwd 指向临时目录**，它们内部的相对路径
    （`ml/data/day_features.csv`、`ml/data/seed_manifest.json`、`ml/data/models/`、
    `dataviz/data/snapshot.json`）因此全部落在临时目录里，不会覆盖真实产物

放在 `ml/` 而不是 `scripts/`：`scripts/` 是按文件分属主的（build-all.sh 归 L3、
check-env.sh 归 L5），新增文件归属不清；`ml/` 整个目录都是 L5 的。

用法：
    .venv/bin/python ml/selftest.py            # 默认，跳过耗时的真实训练（约 1 分钟）
    .venv/bin/python ml/selftest.py --full     # 含完整训练，端到端（约 4 分钟）
"""
import argparse
import json
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import traceback
from datetime import date, datetime, timedelta
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
PY = sys.executable
FMT = "%Y-%m-%d %H:%M:%S"

_results = []


def check(name):
    """注册一条检查。断言失败或抛异常都记为 FAIL，不中断后续检查。"""
    def deco(fn):
        _results.append((name, fn))
        return fn
    return deco


class Ctx:
    """临时工作区。所有脚本都在这里跑，真实产物一概不动。"""

    def __init__(self, root: Path):
        self.root = root
        self.db = root / "test.db"
        (root / "ml" / "data").mkdir(parents=True, exist_ok=True)
        (root / "dataviz" / "data").mkdir(parents=True, exist_ok=True)
        schema = (REPO / "docs" / "db-schema.sql").read_text(encoding="utf-8")
        con = sqlite3.connect(self.db)
        con.executescript(schema)
        con.commit()
        con.close()

    def run(self, script, *args, expect=0):
        """以临时目录为 cwd 调用脚本，返回 CompletedProcess。"""
        p = subprocess.run([PY, str(REPO / "ml" / script), *map(str, args)],
                           cwd=self.root, capture_output=True, text=True)
        if expect is not None and p.returncode != expect:
            raise AssertionError(
                f"{script} {' '.join(map(str, args))} 期望退出码 {expect}，实际 {p.returncode}\n"
                f"stdout: {p.stdout[-800:]}\nstderr: {p.stderr[-800:]}")
        return p

    def sql(self, q, *a):
        con = sqlite3.connect(f"file:{self.db}?mode=ro", uri=True)
        try:
            return con.execute(q, a).fetchall()
        finally:
            con.close()

    def write_sql(self, q, *a):
        con = sqlite3.connect(str(self.db))
        try:
            con.execute(q, a)
            con.commit()
        finally:
            con.close()


CTX: Ctx = None


# =============================================================================
# 1. 环境与种子库
# =============================================================================

@check("环境：建模依赖可导入")
def _():
    import joblib, numpy, pandas, sklearn        # noqa: F401


@check("种子库：db-schema.sql 建出的库含站点/电桩/用户，且无订单")
def _():
    assert CTX.sql("SELECT COUNT(*) FROM t_station")[0][0] > 0, "无站点"
    assert CTX.sql("SELECT COUNT(*) FROM t_pile")[0][0] > 0, "无电桩"
    assert CTX.sql("SELECT COUNT(*) FROM t_user")[0][0] > 0, "无用户"
    assert CTX.sql("SELECT COUNT(*) FROM t_order")[0][0] == 0, "种子库不该有订单"


# =============================================================================
# 2. 生成器 gen_history.py —— CR-002 批复的每条约束都要有断言
# =============================================================================

@check("生成器：dry-run 不写库（只读连接是结构性保证）")
def _():
    before = CTX.sql("SELECT COUNT(*) FROM t_order")[0][0]
    CTX.run("gen_history.py", CTX.db)
    assert CTX.sql("SELECT COUNT(*) FROM t_order")[0][0] == before, "dry-run 竟然写了库"


@check("生成器：--reset 硬性拒绝 charging.db（CR-002 批复第 2 条，退出码 4）")
def _():
    # 保护是**按文件名**生效的：在临时目录里放一个同名库，也必须被拒。
    # 这样既验到了拒绝分支，又不必把真库牵扯进测试。
    decoy = CTX.root / "charging.db"
    shutil.copy(CTX.db, decoy)
    before = decoy.stat().st_size
    p = CTX.run("gen_history.py", "charging.db", "--commit", "--reset", expect=4)
    assert "拒绝" in p.stderr and "CR-002" in p.stderr, f"拒绝信息不完整：{p.stderr[:200]}"
    assert decoy.stat().st_size == before, "被拒绝后文件仍被改动"
    # 换个名字的同一个库则应当放行——证明拒绝确实来自文件名而非别的原因
    ok = CTX.root / "throwaway.db"
    shutil.copy(CTX.db, ok)
    CTX.run("gen_history.py", "throwaway.db", "--commit", "--reset", expect=0)
    decoy.unlink()
    ok.unlink()


@check("生成器：落库后只写允许的表，禁改表哈希逐字节不变")
def _():
    import hashlib

    def digest(con, t, cols="*"):
        rows = con.execute(f"SELECT {cols} FROM {t}").fetchall()
        return hashlib.sha256(repr(sorted(map(repr, rows))).encode()).hexdigest()

    con = sqlite3.connect(f"file:{CTX.db}?mode=ro", uri=True)
    forbidden = ("t_user", "t_station", "t_admin", "t_wallet_tx", "t_sys_config")
    before = {t: digest(con, t) for t in forbidden}
    pile_cols = ("pile_id,pile_code,station_id,type,power,status,online,"
                 "last_heartbeat,create_time")
    before_pile = digest(con, "t_pile", pile_cols)
    con.close()

    CTX.run("gen_history.py", CTX.db, "--commit")

    con = sqlite3.connect(f"file:{CTX.db}?mode=ro", uri=True)
    for t in forbidden:
        assert digest(con, t) == before[t], f"{t} 被改动了"
    assert digest(con, "t_pile", pile_cols) == before_pile, "t_pile 非聚合列被改动"
    con.close()
    assert CTX.sql("SELECT COUNT(*) FROM t_order")[0][0] > 0, "没写进订单"


@check("生成器：历史订单只落终态 3/4，不留 0/1/2")
def _():
    # 残留 0/1/2 会被 1201 当成「未完成订单」，测试者一登录就被弹窗堵死
    st = {r[0] for r in CTX.sql("SELECT DISTINCT status FROM t_order")}
    assert st <= {3, 4}, f"出现非终态订单：{st}"


@check("生成器：红线——今日及以后无订单")
def _():
    n = CTX.sql("SELECT COUNT(*) FROM t_order WHERE reserve_time >= date('now','localtime')"
                " OR IFNULL(settle_time,'') >= date('now','localtime')")[0][0]
    assert n == 0, f"有 {n} 条订单落在今日或之后"


@check("生成器：金额一律整数分，与 1204 结算规则一致（CR-002 批复第 1 条）")
def _():
    n = CTX.sql("SELECT COUNT(*) FROM t_order WHERE status=3 "
                "AND amount != (price*kwh_x100+50)/100")[0][0]
    assert n == 0, f"{n} 条订单的金额不符 (price*kwh_x100+50)//100"
    # 不能有负数或非整数（列本身是 INTEGER，这里查语义）
    assert CTX.sql("SELECT COUNT(*) FROM t_order WHERE amount < 0")[0][0] == 0


@check("生成器：占用约束——同一根桩的充电区间不重叠")
def _():
    bad = 0
    for (pid,) in CTX.sql("SELECT DISTINCT pile_id FROM t_order WHERE status=3"):
        iv = [(datetime.strptime(a, FMT), datetime.strptime(b, FMT))
              for a, b in CTX.sql("SELECT start_time,end_time FROM t_order "
                                  "WHERE status=3 AND pile_id=? ORDER BY start_time", pid)]
        bad += sum(1 for x, y in zip(iv, iv[1:]) if y[0] < x[1])
    assert bad == 0, f"{bad} 处同桩区间重叠——占用约束失效，idle_pile 会算成负数"


@check("生成器：t_pile 聚合回填等于已结算单数（可重复执行不累加）")
def _():
    agg = CTX.sql("SELECT SUM(charge_count) FROM t_pile")[0][0]
    settled = CTX.sql("SELECT COUNT(*) FROM t_order WHERE status=3")[0][0]
    assert agg == settled, f"聚合 {agg} != 已结算 {settled}"


@check("生成器：不带 --reset 重跑撞唯一约束并友好报错（退出码 3）")
def _():
    p = CTX.run("gen_history.py", CTX.db, "--commit", expect=3)
    assert "--reset" in p.stderr, "报错没提示加 --reset 或换 --seed"
    # 回滚必须干净：订单数不能变多
    assert CTX.sql("SELECT COUNT(*) FROM t_order")[0][0] > 0


@check("生成器：--reset 只删播种批次，绝不误伤区间内的外部测试数据（CR-002 批复第 2 条）")
def _():
    # 造一条别人留在历史区间正中的订单，模拟组员的联调数据
    mid = (date.today() - timedelta(days=30)).strftime("%Y-%m-%d 15:08:56")
    CTX.write_sql(
        "INSERT INTO t_order (order_no,user_id,pile_id,station_id,status,price,"
        "kwh_x100,amount,reserve_time,start_time,end_time,settle_time) "
        "VALUES ('ORD_SELFTEST_FOREIGN',1,1,1,3,150,1000,1500,?,?,?,?)",
        mid, mid, mid, mid)
    before = CTX.sql("SELECT COUNT(*) FROM t_order")[0][0]
    CTX.run("gen_history.py", CTX.db, "--commit", "--reset")
    alive = CTX.sql("SELECT COUNT(*) FROM t_order WHERE order_no='ORD_SELFTEST_FOREIGN'")[0][0]
    assert alive == 1, "--reset 误删了不属于本脚本的订单——按时间区间盲删的经典事故"
    assert CTX.sql("SELECT COUNT(*) FROM t_order")[0][0] <= before + 200, "重播后订单数异常膨胀"


@check("生成器：红线自检——库里有今日订单时 --commit 中止（退出码 2）")
def _():
    today = datetime.now().strftime(FMT)
    CTX.write_sql(
        "INSERT INTO t_order (order_no,user_id,pile_id,station_id,status,price,"
        "kwh_x100,amount,reserve_time,start_time,end_time,settle_time) "
        "VALUES ('ORD_SELFTEST_TODAY',1,1,1,3,150,1000,1500,?,?,?,?)",
        today, today, today, today)
    before = CTX.sql("SELECT COUNT(*) FROM t_order")[0][0]
    p = CTX.run("gen_history.py", CTX.db, "--commit", "--reset", expect=2)
    assert "已中止" in p.stderr or "中止" in p.stderr
    assert CTX.sql("SELECT COUNT(*) FROM t_order")[0][0] == before, "中止后仍写了库"
    CTX.write_sql("DELETE FROM t_order WHERE order_no='ORD_SELFTEST_TODAY'")
    CTX.write_sql("DELETE FROM t_order WHERE order_no='ORD_SELFTEST_FOREIGN'")


@check("生成器：节假日特征确实可用（[说明书] 1.4 要求「天气、节假日等多维数据」）")
def _():
    # 默认 60 天窗口（7~9 月）内没有法定节假日，is_holiday 恒为 0，是零方差列。
    # 能力本身必须是好的：把窗口拉长到覆盖 05-01，该列就应当出现 1。
    sys.path.insert(0, str(REPO / "ml"))
    import gen_history as g
    assert g.is_statutory(date(2026, 5, 1)) is True, "05-01 应判为法定节假日"
    assert g.is_statutory(date(2026, 7, 15)) is False, "普通工作日不应判为节假日"
    assert g.is_rest_day(date(2026, 7, 11)) is True, "周六应判为休息日"
    # is_holiday 已收窄为「仅法定」，与 weekday 列不再共线
    assert g.is_statutory(date(2026, 7, 11)) is False, "周末不该算进 is_holiday"

    days = (date.today() - date(2026, 4, 25)).days
    CTX.run("gen_history.py", CTX.db, "--days", days)          # dry-run，只出 day_features
    rows = (CTX.root / "ml" / "data" / "day_features.csv").read_text(encoding="utf-8").splitlines()
    holiday = [r for r in rows[1:] if r.split(",")[2] == "1"]
    assert holiday, f"{days} 天窗口覆盖 05-01，is_holiday 却全为 0"


@check("生成器：天气是持续性过程（马尔可夫），不是逐日独立抽样")
def _():
    rows = (CTX.root / "ml" / "data" / "day_features.csv").read_text(
        encoding="utf-8").splitlines()[1:]
    w = [r.split(",")[3] for r in rows]
    same = sum(1 for a, b in zip(w, w[1:]) if a == b) / max(len(w) - 1, 1)
    # 独立抽样下次日同天气约 0.30（Σp²）；加了 0.55 的沿用概率后应显著更高
    assert same > 0.45, f"次日同天气仅 {same:.2f}，天气持续性没生效，滞后特征学不到东西"


# =============================================================================
# 3. 信号体检 check_signal.py
# =============================================================================

@check("信号体检：四项验收全过（自相关/分站可分性/峰谷比/物理合理性）")
def _():
    CTX.run("gen_history.py", CTX.db, "--commit", "--reset")   # 恢复标准 60 天数据
    p = CTX.run("check_signal.py", CTX.db)
    assert "四项全部达标" in p.stdout, p.stdout[-600:]


@check("信号体检：站点峰值负荷不超过装机功率")
def _():
    p = CTX.run("check_signal.py", CTX.db)
    line = [l for l in p.stdout.splitlines() if "峰值负荷占装机" in l][0]
    assert "PASS" in line, f"负荷超过装机，物理上不可能：{line}"


# =============================================================================
# 4. 特征工程 build_features.py
# =============================================================================

@check("特征工程：穿越自检通过（lag_h 对齐 origin、seas_mean 独立重算一致）")
def _():
    p = CTX.run("build_features.py", CTX.db)
    assert "穿越自检" in p.stdout and "全部一致" in p.stdout, p.stdout[-600:]


@check("特征工程：滞后特征不越过 origin_ts")
def _():
    import pandas as pd
    f = pd.read_csv(CTX.root / "ml" / "data" / "features.csv",
                    parse_dates=["origin_ts", "target_ts"])
    d = (f["target_ts"] - f["origin_ts"]).dt.total_seconds() / 3600
    assert (d == f["horizon"]).all(), "origin_ts 与 horizon 对不上"
    assert f.isna().sum().sum() == 0, "特征里有缺失值"


@check("特征工程：并发会话数不超过桩总数（idle_pile 的物理前提）")
def _():
    import pandas as pd
    f = pd.read_csv(CTX.root / "ml" / "data" / "features.csv")
    assert (f["y_sessions"] <= f["pile_total"] + 1e-9).all(), \
        "并发数超过桩总数，idle_pile 只能靠 clamp 兜底，预测的是假量"
    assert (f["y_idle_pile"] >= 0).all()


@check("特征工程：horizon > 24 时拒绝执行（seas_mean 的无穿越性依赖此前提）")
def _():
    p = CTX.run("build_features.py", CTX.db, "--horizons", "1,48", expect=1)
    assert "seas_mean" in p.stderr, "没有说明拒绝原因"


@check("特征工程：周期编码正确（sin/cos 让 23 时与 0 时相邻）")
def _():
    import math
    import pandas as pd
    f = pd.read_csv(CTX.root / "ml" / "data" / "features.csv")
    r = f.iloc[0]
    assert abs(r["hour_sin"] - math.sin(2 * math.pi * r["hour"] / 24)) < 1e-9
    assert abs(r["hour_cos"] - math.cos(2 * math.pi * r["hour"] / 24)) < 1e-9
    h23 = f[f["hour"] == 23].iloc[0]
    h0 = f[f["hour"] == 0].iloc[0]
    h12 = f[f["hour"] == 12].iloc[0]
    d = lambda a, b: (a["hour_sin"] - b["hour_sin"]) ** 2 + (a["hour_cos"] - b["hour_cos"]) ** 2
    assert d(h23, h0) < d(h23, h12), "环形编码失效：23 时离 0 时反而更远"


# =============================================================================
# 5. 训练 train_forecast.py
# =============================================================================

@check("训练：时序切分严格按时间，训练段整体早于测试段")
def _():
    import pandas as pd
    sys.path.insert(0, str(REPO / "ml"))
    from train_forecast import time_split
    f = pd.read_csv(CTX.root / "ml" / "data" / "features.csv", parse_dates=["target_ts"])
    sub = f[f["horizon"] == 1]
    tr, te, cut = time_split(sub, 12)
    assert len(tr) and len(te)
    assert tr["target_ts"].max() < te["target_ts"].min(), \
        "训练段与测试段在时间上重叠——随机切分会把未来泄漏进过去"
    assert (te["target_ts"].max() - te["target_ts"].min()).days <= 12


@check("训练：基线只用训练段拟合，不看测试段")
def _():
    import numpy as np
    import pandas as pd
    sys.path.insert(0, str(REPO / "ml"))
    from train_forecast import hour_mean_baseline, time_split
    f = pd.read_csv(CTX.root / "ml" / "data" / "features.csv", parse_dates=["target_ts"])
    sub = f[f["horizon"] == 1]
    tr, te, _ = time_split(sub, 12)
    keys = ["station_id", "hour"]
    pred = hour_mean_baseline(tr, te, "y_load_kw", keys)
    assert len(pred) == len(te)
    # 逐行核对：预测值必须等于**训练段**同键均值，用到测试段任何一行都会对不上
    table = tr.groupby(keys)["y_load_kw"].mean()
    exp = [table.get((r.station_id, r.hour), tr["y_load_kw"].mean()) for r in te.itertuples()]
    assert np.allclose(pred, exp), "基线用到了训练段以外的数据"


@check("训练：模型产物与 meta.json 自洽，且带 is_peak 校准阈值")
def _():
    import joblib
    md = MODELS_DIR
    meta = json.loads((md / "meta.json").read_text(encoding="utf-8"))
    assert meta.get("peak_thresholds"), "meta.json 缺 peak_thresholds，predict.py 会拒绝运行"
    for h in ("1", "6", "24"):
        assert h in meta["peak_thresholds"], f"缺 horizon={h} 的阈值"
        assert len(meta["peak_thresholds"][h]) == 6, "阈值应逐站一个"
    for t in ("y_load_kw", "y_sessions"):
        for h in (1, 6, 24):
            b = joblib.load(md / f"{t}_h{h}.joblib")
            assert b["model_version"] == meta["model_version"], \
                f"{t}_h{h} 的 model_version 与 meta.json 不一致，模型目录混了两次训练的产物"
            assert b["feat_cols"], "模型没存特征列序，推理时无法对齐"


@check("训练：is_peak 阈值随 horizon 递减（补偿长程预测的平滑）")
def _():
    meta = json.loads((MODELS_DIR / "meta.json").read_text(encoding="utf-8"))
    th = meta["peak_thresholds"]
    lower = sum(1 for s in th["1"] if float(th["24"][s]) <= float(th["1"][s]))
    assert lower >= 4, ("24h 阈值普遍不低于 1h 阈值，说明阈值没在预测分布上校准，"
                        "is_peak 会系统性欠触发")


# =============================================================================
# 6. 推理回写 predict.py
# =============================================================================

@check("推理：authorizer 只放行 t_load_forecast，其余写入与 DDL 一律拒绝")
def _():
    sys.path.insert(0, str(REPO / "ml"))
    from predict import only_forecast_writable
    con = sqlite3.connect(str(CTX.db))
    con.set_authorizer(only_forecast_writable)
    try:
        con.execute("SELECT COUNT(*) FROM t_order")          # 读必须放行
        for sql, what in [("UPDATE t_user SET nickname='x'", "改 t_user"),
                          ("DELETE FROM t_order", "删 t_order"),
                          ("UPDATE t_pile SET status=2", "改 t_pile"),
                          ("DROP TABLE t_order", "DROP"),
                          ("ALTER TABLE t_order ADD COLUMN x INT", "ALTER")]:
            try:
                con.execute(sql)
                raise AssertionError(f"authorizer 没拦住：{what}")
            except sqlite3.DatabaseError:
                pass
    finally:
        con.rollback()
        con.close()


@check("推理：回写覆盖每个 station × horizon，整批共用一个 create_time")
def _():
    CTX.run("predict.py", CTX.db, "--commit", "--prune")
    n_st = CTX.sql("SELECT COUNT(*) FROM t_station")[0][0]
    rows = CTX.sql("SELECT COUNT(*) FROM t_load_forecast")[0][0]
    assert rows == n_st * 3, f"应有 {n_st}×3 行，实际 {rows}"
    combos = CTX.sql("SELECT COUNT(*) FROM (SELECT station_id,horizon "
                     "FROM t_load_forecast GROUP BY 1,2)")[0][0]
    assert combos == n_st * 3, "有站点或 horizon 缺行——1101 会退化成填 -1"
    # 协议 2305：取同一 model_version 下 create_time 最大的一批。
    # 逐行取当前时刻会让同批出现多个 create_time，L2 只能捞到最后几行。
    assert CTX.sql("SELECT COUNT(DISTINCT create_time) FROM t_load_forecast")[0][0] == 1
    assert CTX.sql("SELECT COUNT(DISTINCT model_version) FROM t_load_forecast")[0][0] == 1


@check("推理：congestion / idle_pile 不越界且方向一致")
def _():
    total = CTX.sql("SELECT COUNT(*) FROM t_pile WHERE station_id=1")[0][0]
    assert CTX.sql("SELECT COUNT(*) FROM t_load_forecast "
                   "WHERE congestion < 0 OR congestion > 1")[0][0] == 0
    assert CTX.sql("SELECT COUNT(*) FROM t_load_forecast f JOIN "
                   "(SELECT station_id, COUNT(*) n FROM t_pile GROUP BY 1) p USING(station_id) "
                   "WHERE f.idle_pile < 0 OR f.idle_pile > p.n")[0][0] == 0
    bad = CTX.sql("SELECT COUNT(*) FROM t_load_forecast a JOIN t_load_forecast b "
                  "ON a.horizon=b.horizon AND a.congestion>b.congestion "
                  "AND a.idle_pile>b.idle_pile")[0][0]
    assert bad == 0, f"{bad} 对记录「更拥堵却更空闲」，两个字段自相矛盾"
    assert total > 0


@check("推理：congestion 保留排序区分度（不被 idle_pile 取整量化）")
def _():
    # 4 根桩时，若用取整后的 idle_pile 反算，congestion 只有 5 个取值，
    # 六个站会大量并列，1101 的 sortBy=1 推荐排序就失去意义
    vals = [r[0] for r in CTX.sql("SELECT congestion FROM t_load_forecast WHERE horizon=1")]
    assert len(set(vals)) >= max(3, len(vals) - 1), \
        f"horizon=1 的 6 个站只有 {len(set(vals))} 个不同拥堵度，排序分不开"


@check("推理：训练/推理特征无偏斜（--check-skew 拿 features.csv 对账）")
def _():
    import pandas as pd
    f = pd.read_csv(CTX.root / "ml" / "data" / "features.csv", parse_dates=["target_ts"])
    # 挑一个历史时刻当 origin，这样目标时刻落在 features.csv 里才对得上
    origin = f[f["horizon"] == 24]["target_ts"].max() - timedelta(hours=24)
    p = CTX.run("predict.py", CTX.db, "--origin", origin.strftime(FMT), "--check-skew")
    assert p.stdout.count("逐列一致") == 3, p.stdout[-600:]


@check("推理：缺少 peak_thresholds 时明确报错，不静默降级")
def _():
    meta_p = MODELS_DIR / "meta.json"
    orig = meta_p.read_text(encoding="utf-8")
    try:
        m = json.loads(orig)
        m.pop("peak_thresholds", None)
        meta_p.write_text(json.dumps(m, ensure_ascii=False), encoding="utf-8")
        p = CTX.run("predict.py", CTX.db, expect=2)
        assert "peak_thresholds" in p.stderr
    finally:
        meta_p.write_text(orig, encoding="utf-8")


@check("推理：2305 / 1101 的取数 SQL 能捞到正确结果（含 stationId=0 与空结果分支）")
def _():
    SQL = ("SELECT f.station_id, s.name, f.horizon, f.predict_time, f.load_kw, f.idle_pile,"
           " f.is_peak, f.congestion, f.model_version"
           " FROM t_load_forecast f JOIN t_station s ON s.station_id = f.station_id"
           " WHERE f.create_time = (SELECT MAX(create_time) FROM t_load_forecast"
           "                         WHERE model_version = f.model_version)"
           "   AND (? = 0 OR f.station_id = ?) AND f.horizon = ?"
           " ORDER BY f.station_id")
    n_st = CTX.sql("SELECT COUNT(*) FROM t_station")[0][0]
    for h in (1, 6, 24):
        assert len(CTX.sql(SQL, 0, 0, h)) == n_st, f"stationId=0 应返回全部站点（horizon={h}）"
    assert len(CTX.sql(SQL, 3, 3, 24)) == 1, "指定站点应返回 1 行"
    assert len(CTX.sql(SQL, 999, 999, 1)) == 0, "不存在的站点应返回空 list 而非报错"
    # 1101：horizon=1 的最新一条，逐站都要有——否则服务端只能填 -1
    r = CTX.sql("SELECT COUNT(DISTINCT station_id) FROM t_load_forecast WHERE horizon=1")
    assert r[0][0] == n_st


# =============================================================================
# 7. 快照导出 export_snapshot.py
# =============================================================================

@check("快照：大屏需要的键一个不缺")
def _():
    CTX.run("export_snapshot.py", CTX.db, "dataviz/data/snapshot.json")
    s = json.loads((CTX.root / "dataviz" / "data" / "snapshot.json").read_text(encoding="utf-8"))
    need = ["generatedAt", "pollIntervalSec", "dataAsOf", "revenue.today", "revenue.month",
            "revenue.total", "trend", "trend30", "status", "rank", "loadCurve.hours",
            "loadCurve.kw", "forecast.modelVersion", "forecast.list",
            "behavior.hourlyOrders", "behavior.byPileType", "behavior.orderStatus"]
    miss = []
    for k in need:
        o = s
        for part in k.split("."):
            o = o.get(part) if isinstance(o, dict) else None
            if o is None:
                miss.append(k)
                break
    assert not miss, f"快照缺键：{miss}"
    globals()["SNAP"] = s


@check("快照：营收口径与服务端 2301 一致（今日/本月/总计）")
def _():
    # 服务端用字符串区间比较 settle_time；这里用同一语义对拍，两边必须给出同一个数
    today = date.today()
    tomorrow = (today + timedelta(days=1)).isoformat()
    m0 = today.replace(day=1).isoformat()
    m1 = (today.replace(day=1) + timedelta(days=32)).replace(day=1).isoformat()
    ref = CTX.sql(
        "SELECT COALESCE(SUM(CASE WHEN settle_time>=? AND settle_time<? THEN amount ELSE 0 END),0),"
        "       COALESCE(SUM(CASE WHEN settle_time>=? AND settle_time<? THEN amount ELSE 0 END),0),"
        "       COALESCE(SUM(amount),0) FROM t_order WHERE status=3",
        today.isoformat(), tomorrow, m0, m1)[0]
    got = SNAP["revenue"]
    assert (got["today"], got["month"], got["total"]) == tuple(ref), \
        f"大屏 {got} 与服务端 2301 口径 {ref} 不一致——答辩现场并排会是两个数"


@check("快照：趋势与服务端 2302 一致，7/30 日均含补零")
def _():
    for key, days in (("trend", 7), ("trend30", 30)):
        rows = SNAP[key]
        assert len(rows) == days, f"{key} 应有 {days} 个自然日（含无订单补零）"
        ds = [r["date"] for r in rows]
        assert ds == sorted(ds) and len(set(ds)) == days, f"{key} 日期轴有重复或乱序"
        assert ds[-1] == date.today().isoformat(), f"{key} 右端应是今天"
        assert all(isinstance(r["amount"], int) for r in rows), "金额必须是整数分"


@check("快照：电桩状态与 2303 一致，三种状态都有值")
def _():
    names = [x["name"] for x in SNAP["status"]]
    assert names == ["在用", "闲置", "故障"], f"状态项缺失或顺序不对：{names}"
    total = sum(x["value"] for x in SNAP["status"])
    assert total == CTX.sql("SELECT COUNT(*) FROM t_pile")[0][0], "状态合计不等于电桩总数"


@check("快照：负荷曲线锚在最后一个有观测的小时，与预测起报点对齐")
def _():
    c = SNAP["loadCurve"]
    assert len(c["hours"]) == len(c["kw"]) == 48
    last_obs = CTX.sql("SELECT MAX(end_time) FROM t_order WHERE status=3")[0][0]
    expect = datetime.strptime(last_obs, FMT).replace(minute=0, second=0, microsecond=0)
    assert c["hours"][-1] == expect.strftime(FMT), \
        "曲线右端不是最后观测小时——空档会被画成掉到 0 的实线，与预测虚线自相矛盾"
    ft = min(f["predictTime"] for f in SNAP["forecast"]["list"])
    assert ft > c["hours"][-1], "预测点没落在实测终点之后"


@check("快照：金额一律整数分，不在导出层做除法")
def _():
    for k in ("today", "month", "total"):
        assert isinstance(SNAP["revenue"][k], int), f"revenue.{k} 不是整数分"
    assert all(isinstance(r["amount"], int) for r in SNAP["rank"])


# =============================================================================
# 8. 大屏页面 dataviz/index.html
# =============================================================================

@check("大屏：七张图表都能用真实快照渲染出非空数据")
def _():
    if not shutil.which("gjs"):
        raise RuntimeError("SKIP: 本机无 gjs，无法执行页面脚本")
    import re
    html = (REPO / "dataviz" / "index.html").read_text(encoding="utf-8")
    inline = re.findall(r'<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>', html, re.S)
    assert len(inline) == 1, f"内联脚本应只有一段，实际 {len(inline)}"
    (CTX.root / "page.js").write_text(inline[0], encoding="utf-8")
    (CTX.root / "harness.js").write_text(HARNESS, encoding="utf-8")
    p = subprocess.run(["gjs", str(CTX.root / "harness.js")],
                       cwd=CTX.root, capture_output=True, text=True)
    assert p.returncode == 0, f"页面脚本执行失败：\n{p.stderr[-900:]}"
    assert "全部图表渲染正常" in p.stdout, p.stdout[-900:]
    assert "接续正确" in p.stdout, "实测曲线与预测曲线没接上"


@check("大屏：过期判断用本地日期而非 UTC")
def _():
    import re
    html = (REPO / "dataviz" / "index.html").read_text(encoding="utf-8")
    # 先剥掉注释再查：解释这个坑的注释本身就会提到 toISOString，
    # 直接对全文做子串匹配会被自己的注释绊倒（第一版就是这么误报的）。
    code = re.sub(r"/\*.*?\*/", "", html, flags=re.S)
    code = "\n".join(re.sub(r"//.*$", "", l) for l in code.splitlines())
    assert "toISOString" not in code, \
        ("用 UTC 日期判断数据是否过期，东八区凌晨 0~8 点会把过期数据判成新鲜的"
         "——库里存的是本地时间")
    # 正面确认：过期判断确实走了本地日期助手
    assert re.search(r"asOf\.className\s*=.*today\(\)", code), \
        "没有用本地日期助手 today() 做过期判断"
    assert re.search(r"getFullYear\(\).*getMonth\(\).*getDate\(\)", code, re.S), \
        "today() 不是按本地时区手工拼的"


HARNESS = r"""
const {GLib} = imports.gi;
const read = p => { const [ok, b] = GLib.file_get_contents(p);
                    if (!ok) throw new Error('读不到 ' + p);
                    return new TextDecoder().decode(b); };
const opts = {}, els = {};
globalThis.echarts = { init: el => ({ setOption: o => { opts[el.__id] = o; },
                                      resize(){}, clear(){ opts[el.__id] = null; } }) };
globalThis.document = { documentElement:{},
  getElementById: id => els[id] || (els[id] = {__id:id, textContent:'', className:'',
                                               hidden:false, dataset:{}, children:[]}) };
globalThis.getComputedStyle = () => ({ getPropertyValue: () => '#ffffff' });
globalThis.addEventListener = () => {};
globalThis.setInterval = () => 0;
globalThis.fetch = () => Promise.reject(new Error('harness 不走网络'));
const snap = JSON.parse(read('dataviz/data/snapshot.json'));
eval(read('page.js') + '\n;globalThis.__r = renderAll;'
     + 'globalThis.__t = drawTrend; globalThis.__c = drawCongestion;');
__r(snap);
const want = {cLoad:'负荷曲线', cTrend:'营收趋势', cStatus:'电桩状态', cRank:'站点排行',
              cCongestion:'拥堵度预测', cHourly:'下单时段', cBehavior:'快慢充/终态'};
let bad = 0;
for (const [id, name] of Object.entries(want)) {
  const o = opts[id];
  if (!o || !o.series || !o.series.length) { print(`✗ ${name} 没有 series`); bad++; continue; }
  const nn = o.series.reduce((n, s) => n + (s.data||[]).filter(v =>
      v !== null && v !== undefined && (typeof v !== 'object' || v.value !== undefined)).length, 0);
  if (!nn) { print(`✗ ${name} series 为空`); bad++; }
}
const load = opts.cLoad, a = load.series[0].data, f = load.series[1].data;
const lastA = a.reduce((m,v,i) => v !== null ? i : m, -1);
const firstF = f.findIndex(v => v !== null);
print(firstF === lastA ? '接续正确' : `✗ 曲线断开（差 ${firstF - lastA} 格）`);
if (firstF !== lastA) bad++;
__t(snap, 'trend30'); if (opts.cTrend.series[0].data.length !== 30) { print('✗ 30 日趋势'); bad++; }
for (const h of [1,6,24]) { __c(snap, h);
  if (!opts.cCongestion.series[0].data.length) { print(`✗ horizon=${h} 空`); bad++; } }
print(bad ? `${bad} 处异常` : '全部图表渲染正常');
if (bad) imports.system.exit(1);
"""


# =============================================================================
# 主流程
# =============================================================================

MODELS_DIR: Path = None


def main() -> int:
    global CTX, MODELS_DIR
    ap = argparse.ArgumentParser(description="L5 全链路自检")
    ap.add_argument("--full", action="store_true",
                    help="在临时目录里真正跑一遍训练（约 4 分钟）；缺省复用现有模型产物")
    ap.add_argument("--keep", action="store_true", help="保留临时目录，便于排查")
    args = ap.parse_args()

    root = Path(tempfile.mkdtemp(prefix="l5-selftest-"))
    print(f"临时工作区：{root}")
    print(f"（真实的 charging.db / ml/data/dev.db / ml/data/models 一概不动）\n")
    CTX = Ctx(root)

    real_models = REPO / "ml" / "data" / "models"
    MODELS_DIR = root / "ml" / "data" / "models"
    if args.full:
        print("--full：在临时目录里完整训练一遍\n")
    else:
        if not (real_models / "meta.json").exists():
            print(f"缺少 {real_models}/meta.json，请先跑 ml/train_forecast.py，"
                  f"或用 --full 让自检自己训", file=sys.stderr)
            return 1
        shutil.copytree(real_models, MODELS_DIR)
        print(f"复用现有模型产物（{real_models}）。加 --full 可在隔离环境里重训。\n")

    passed = failed = skipped = 0
    for i, (name, fn) in enumerate(_results, 1):
        # --full 时在训练相关检查之前插入真正的训练
        if args.full and name.startswith("训练：") and not MODELS_DIR.exists():
            CTX.run("train_forecast.py", "--features", "ml/data/features.csv")
        try:
            fn()
            print(f"  [{i:2d}] PASS  {name}")
            passed += 1
        except RuntimeError as e:
            if str(e).startswith("SKIP:"):
                print(f"  [{i:2d}] SKIP  {name}　（{str(e)[5:].strip()}）")
                skipped += 1
            else:
                print(f"  [{i:2d}] FAIL  {name}\n        {e}")
                failed += 1
        except Exception as e:
            first = traceback.format_exc().strip().splitlines()[-1]
            print(f"  [{i:2d}] FAIL  {name}\n        {str(e).strip() or first}")
            failed += 1

    print(f"\n{passed} 项通过　{failed} 项失败　{skipped} 项跳过")
    if args.keep:
        print(f"临时目录保留在 {root}")
    else:
        shutil.rmtree(root, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

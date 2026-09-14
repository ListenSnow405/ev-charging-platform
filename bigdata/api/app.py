"""T5：Flask 只读 API 层。

职责边界（CLAUDE.md 5.2 第 8 条）：
  · 只查 MySQL 里的**分析结果表**，不碰 charging.db，不写任何业务库；
  · 不现场触发 Spark job——大屏要的是毫秒级响应，重算是离线流水线的事；
  · 与 Qt 服务端完全无关，后者仍是纯 Socket（CLAUDE.md 2.1 那条推论）。

金额一律以**整数分**返回（5.2 第 7 条），除以 100 是前端展示层的事。
启动：.venv-phase2/bin/python bigdata/api/app.py
"""
from __future__ import annotations

import configparser
import logging
from decimal import Decimal
from datetime import datetime
from pathlib import Path

import pymysql
from flask import Flask, jsonify, request
from flask.json.provider import DefaultJSONProvider
from flask_cors import CORS

REPO_ROOT = Path(__file__).resolve().parents[2]
CFG = REPO_ROOT / "config" / "phase2.ini"

# 沿用第一阶段「错误先记日志再返回错误码，不用裸 bool」的口径（CLAUDE.md 第 7 节）
OK = 0
ERR_PARAM = 1001          # 入参非法
ERR_NOT_FOUND = 1002      # 维度不存在
ERR_DB = 1003             # 数据库不可用

logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("ecp-api")

class _JSON(DefaultJSONProvider):
    """把 Decimal 序列化成数字而不是字符串。

    MySQL 的 SUM()/AVG() 经 pymysql 返回的是 `decimal.Decimal`，
    Flask 默认把它转成**字符串**（如 "78.11"）。前端图表库拿到字符串
    会静默画不出来——这类 bug 在浏览器里只表现为"图是空的"，很难定位。
    在序列化层统一处理，比在每个接口里记得 float() 可靠。
    """

    @staticmethod
    def default(o):
        if isinstance(o, Decimal):
            return float(o)
        return DefaultJSONProvider.default(o)


app = Flask(__name__)
app.json = _JSON(app)
CORS(app)                 # Vue 开发服务器与本服务不同源

_dimensions: dict[str, str] = {}     # 表名 → 说明，启动时从 information_schema 读


def db_config() -> dict:
    if not CFG.exists():
        raise FileNotFoundError(
            f"缺少 {CFG}，先跑 bash scripts/init-mysql-phase2.sh")
    cfg = configparser.ConfigParser()
    cfg.read(CFG, encoding="utf-8")
    m = cfg["mysql"]
    return dict(host=m["host"], port=int(m["port"]), user=m["user"],
                password=m["password"], database=m["database"],
                charset="utf8mb4", cursorclass=pymysql.cursors.DictCursor)


def connect():
    """每请求一条连接。结果表都是只读小表，连接开销可以忽略，
    换来的是不必处理连接池在多 worker 下的状态问题。"""
    return pymysql.connect(**db_config())


def ok(data, **extra):
    return jsonify({"code": OK, "msg": "ok", "data": data, **extra})


def fail(code: int, msg: str, status: int = 400):
    log.warning("请求失败 code=%s msg=%s path=%s", code, msg, request.path)
    return jsonify({"code": code, "msg": msg, "data": None}), status


def load_catalog() -> dict[str, str]:
    """启动时把可用维度表读进白名单。

    **白名单是安全边界**：表名会拼进 SQL，绝不能接受用户传什么查什么。
    从 information_schema 取而不是硬编码，这样 analysis.py 增删维度后无需改本文件。
    """
    try:
        conn = connect()
    except Exception as e:
        log.error("数据库不可用，维度目录为空：%s", e)
        return {}
    try:
        with conn.cursor() as cur:
            cur.execute("""
                SELECT TABLE_NAME AS name, TABLE_COMMENT AS comment
                FROM information_schema.TABLES
                WHERE TABLE_SCHEMA = DATABASE()
                  AND (TABLE_NAME LIKE 'd%%' OR TABLE_NAME LIKE 'c%%')
                ORDER BY TABLE_NAME""")
            cat = {r["name"]: r["comment"] for r in cur.fetchall()}
        log.info("维度目录载入 %d 个：%s", len(cat), "、".join(sorted(cat)))
        return cat
    finally:
        conn.close()


# ---------------------------------------------------------------- 接口

@app.get("/api/health")
def health():
    """健康检查：顺带探一次数据库，避免「服务活着但库连不上」这种假健康。"""
    try:
        conn = connect()
        with conn.cursor() as cur:
            cur.execute("SELECT 1 AS ok")
            cur.fetchone()
        conn.close()
        db_ok = True
    except Exception as e:
        log.error("健康检查触库失败：%s", e)
        db_ok = False
    return ok({"service": "ecp-bigdata-api", "database": db_ok,
               "dimensions": len(_dimensions),
               "time": datetime.now().strftime("%Y-%m-%d %H:%M:%S")})


@app.get("/api/dimensions")
def dimensions():
    """维度目录。前端据此渲染，不必在代码里写死维度清单。"""
    if not _dimensions:
        return fail(ERR_DB, "维度目录为空，检查 MySQL 是否已装载分析结果", 503)
    items = [{"name": k, "description": v,
              "kind": "对比分析" if k.startswith("c") else "分析维度"}
             for k, v in sorted(_dimensions.items())]
    return ok(items, total=len(items),
              dimension_count=sum(1 for i in items if i["kind"] == "分析维度"),
              comparison_count=len({k.split("_")[0] for k in _dimensions
                                    if k.startswith("c")}))


@app.get("/api/dimension/<name>")
def dimension(name: str):
    """取一个维度的全部数据。

    结果表最多 60 行，一次返回完；分页只会让前端画图变复杂而没有收益。
    """
    if name not in _dimensions:                       # 白名单校验，见 load_catalog
        return fail(ERR_NOT_FOUND,
                    f"维度 {name} 不存在。可用维度见 /api/dimensions", 404)
    try:
        conn = connect()
    except Exception as e:
        log.error("连接数据库失败：%s", e)
        return fail(ERR_DB, "数据库不可用", 503)
    try:
        with conn.cursor() as cur:
            cur.execute(f"SELECT * FROM `{name}`")    # name 已过白名单
            rows = cur.fetchall()
        return ok(rows, dimension=name,
                  description=_dimensions[name], row_count=len(rows))
    finally:
        conn.close()


@app.get("/api/overview")
def overview():
    """大屏头部 KPI。由已落库的维度表**再聚合**，不重算明细——
    这样 KPI 与各图表必然同源，不会出现「总数对不上分项之和」。"""
    try:
        conn = connect()
    except Exception as e:
        log.error("连接数据库失败：%s", e)
        return fail(ERR_DB, "数据库不可用", 503)
    try:
        with conn.cursor() as cur:
            cur.execute("""SELECT SUM(revenue_fen) AS revenue_fen,
                                  SUM(order_cnt)   AS settled_cnt,
                                  SUM(kwh_x100)    AS kwh_x100,
                                  COUNT(*)         AS day_cnt
                           FROM d1_revenue_trend""")
            rev = cur.fetchone()
            cur.execute("SELECT SUM(order_cnt) AS total FROM d6_order_status")
            total = cur.fetchone()["total"]
            cur.execute("""SELECT SUM(pile_cnt) AS piles, SUM(online_cnt) AS online
                           FROM d7_pile_status""")
            pile = cur.fetchone()
            cur.execute("SELECT COUNT(*) AS stations FROM d2_station_rank")
            stations = cur.fetchone()["stations"]
            cur.execute("SELECT SUM(emission_g) AS emission_g FROM d9_carbon_daily")
            carbon = cur.fetchone()["emission_g"]

        settled = int(rev["settled_cnt"] or 0)
        return ok({
            "revenue_fen": int(rev["revenue_fen"] or 0),   # 分，前端除 100
            "kwh_x100": int(rev["kwh_x100"] or 0),         # 度×100
            "order_total": int(total or 0),
            "order_settled": settled,
            "settle_rate_pct": round(settled / total * 100, 2) if total else 0.0,
            "day_count": int(rev["day_cnt"] or 0),
            "pile_total": int(pile["piles"] or 0),
            "pile_online": int(pile["online"] or 0),
            "station_total": int(stations or 0),
            "emission_g": int(carbon or 0),
        })
    finally:
        conn.close()


@app.errorhandler(404)
def not_found(_):
    return fail(ERR_NOT_FOUND, "接口不存在，可用接口见 /api/health", 404)


@app.errorhandler(500)
def server_error(e):
    log.exception("未处理异常")
    return fail(ERR_DB, f"服务内部错误：{e}", 500)


_dimensions = load_catalog()

if __name__ == "__main__":
    log.info("启动 ecp-bigdata-api，维度 %d 个", len(_dimensions))
    # 只监听回环：本服务无鉴权，不应暴露到局域网（[说明书] 2.2 数据安全）
    app.run(host="127.0.0.1", port=5000, debug=False)

"""DWD 层的 Schema 契约：列顺序、类型、可空与字段含义。　归属 L5

**这是 DWD 的数据字典，也是唯一事实来源。** `cleaning.py` 按它转类型与排列，
`validation.py` 按它断言产物，`quality_report.py` 按它渲染字典章节——
三处共用一份声明，改一处不会漏改另外两处。

为什么需要它（2026-09-15 评审提出）：
  · 原先 `t_carbon_daily`/`t_wallet_tx`/`t_load_forecast` 等表只做伪缺失归一就落盘，
    `balance`、`amount`、`*_kwh_x100`、`emission_g` 这些**金额与电量字段全是字符串**。
    CLAUDE.md 5.2 第 7 条要求整数聚合，字符串留在这里等于给浮点开了后门——
    下游谁写一句 sum(col("balance"))，Spark 会隐式转 double。
    证据是 `analysis.py` 到处写 `.cast("long")` 做自保，那正是契约缺位的代价。
  · `cast_with_capture()` 会把转换过的列挪到表尾，DWD 列序随实现细节漂移，
    没有声明就无从校验。

类型口径一律以 `docs/db-schema.sql` 与 `docs/db-schema-ext-08.sql`（冻结契约）为准，
不自行加戏；派生列标 [派生]，由 `cleaning.py` 按 `03_rules.md` 的规则生成。
"""
from __future__ import annotations

#  六种目标类型。TS/DATE 走格式化解析（见 cleaning.to_ts / to_date），
#  其余走 cast；BOOL 是派生列专用，不从源库来。
LONG, DBL, TS, DATE, STR, BOOL = "long", "double", "ts", "date", "str", "bool"

TS_FMT = "yyyy-MM-dd HH:mm:ss"
DATE_FMT = "yyyy-MM-dd"

#  每列四元组：(列名, 类型, 可空, 说明)
#  「可空」描述的是 **DWD 的事实**，不是照抄源库约束——两者会不一致，原因是 R001：
#  源库里 `TEXT NOT NULL DEFAULT ''` 的列（avatar / output_path / remark …）存的是**空串**，
#  而 R001 伪缺失归一按 SOP 6.1 把空串转成了 NULL。照抄源库的 NOT NULL 就会和自己的清洗规则打架
#  ——2026-09-15 首次跑契约断言时实测抓到（dwd_user.avatar 5 行、dwd_carbon_report.output_path 2 行）。
#  这类列在下面标可空并写明出处，免得下次有人"修正"回去。
#  派生列按业务语义判定——取消单没有充电时长，`charge_minutes` 必须可空，填充它就是伪造事实。
DWD_SCHEMA: dict[str, list[tuple[str, str, bool, str]]] = {
    "dwd_order": [
        ("order_id",       LONG, False, "源库自增主键"),
        ("order_no",       STR,  False, "业务主键：ORD + yyyyMMddHHmmss + 4 位随机"),
        ("user_id",        LONG, False, "外键 → t_user"),
        ("pile_id",        LONG, False, "外键 → t_pile"),
        ("station_id",     LONG, False, "外键 → t_station（源库冗余字段，便于按站统计）"),
        ("status",         LONG, False, "0=已预约 1=充电中 2=待结算 3=已结算 4=已取消"),
        ("price",          LONG, False, "下单时单价快照，单位**分/度**"),
        ("kwh_x100",       LONG, False, "充电量 × 100，整数避免浮点"),
        ("amount",         LONG, False, "应付金额，单位**分**"),
        ("reserve_time",   TS,   False, "预约时刻"),
        ("start_time",     TS,   True,  "开始充电时刻；取消单为空（合理缺失）"),
        ("end_time",       TS,   True,  "结束充电时刻；同上"),
        ("settle_time",    TS,   True,  "结算时刻；同上"),
        ("status_label",   STR,  False, "[派生] R005 码表归一，原码值保留在 status"),
        ("charge_minutes", LONG, True,  "[派生] R010 end−start，整数分钟；取消单为空"),
        ("wait_minutes",   LONG, True,  "[派生] R010 start−reserve，整数分钟；取消单为空"),
        ("unit_price_fen", LONG, True,  "[派生] R010 amount×100÷kwh_x100，整数分/度；电量为 0 时空"),
        ("order_date",     DATE, False, "[派生] R010 预约日期，供按日聚合"),
        ("order_hour",     LONG, False, "[派生] R010 预约小时 0–23，供负荷分析"),
        ("is_weekend",     BOOL, False, "[派生] R010 是否周末，供工作日/周末对比"),
    ],
    "dwd_user": [
        ("user_id",      LONG, False, "源库自增主键"),
        ("phone",        STR,  False, "11 位手机号，用户端免密登录主键"),
        ("nickname",     STR,  False, "默认「用户」+ 后 4 位"),
        ("avatar",       STR,  True,  "图片路径。源库 NOT NULL DEFAULT ''，**空串=默认灰色头像**；空串经 R001 归一为 NULL，语义仍是「用默认头像」而非「未知」"),
        ("balance",      LONG, False, "钱包余额，单位**分**"),
        ("status",       LONG, False, "0=正常 1=冻结"),
        ("create_time",  TS,   False, "注册时间"),
        ("update_time",  TS,   False, "更新时间"),
        ("phone_valid",  BOOL, False, "[派生] R006 手机号是否合规；false 者进待核，不删不改"),
        ("status_label", STR,  False, "[派生] R005 正常 / 冻结"),
    ],
    "dwd_pile": [
        ("pile_id",         LONG, False, "源库自增主键"),
        ("pile_code",       STR,  False, "电桩编号，如 SZ001-03"),
        ("station_id",      LONG, False, "外键 → t_station"),
        ("type",            LONG, False, "0=快充 1=慢充"),
        ("power",           DBL,  False, "额定功率，单位 kW"),
        ("status",          LONG, False, "0=在用 1=闲置 2=故障"),
        ("charge_count",    LONG, False, "累计充电次数"),
        ("charge_duration", LONG, False, "累计充电时长，单位**秒**"),
        ("online",          LONG, False, "0=离线 1=在线"),
        ("last_heartbeat",  TS,   True,  "最近心跳；离线桩本就没有（合理缺失）"),
        ("create_time",     TS,   False, "建档时间"),
        ("type_label",      STR,  False, "[派生] R005 快充 / 慢充"),
        ("status_label",    STR,  False, "[派生] R005 在用 / 闲置 / 故障"),
    ],
    "dwd_station": [
        ("station_id",  LONG, False, "源库自增主键"),
        ("name",        STR,  False, "站名"),
        ("address",     STR,  False, "详细地址"),
        ("lng",         DBL,  False, "经度"),
        ("lat",         DBL,  False, "纬度"),
        ("price",       LONG, False, "充电价格，单位**分/度**"),
        ("status",      LONG, False, "0=营业 1=停业"),
        ("create_time", TS,   False, "建站时间"),
    ],
    #  old_status / new_status 已按 R008 列级剔除（缺失 100% / 98.67%），故不在契约内。
    "dwd_pile_log": [
        ("log_id",      LONG, False, "源库自增主键"),
        ("pile_id",     LONG, False, "外键 → t_pile"),
        ("event",       LONG, False, "0=上线 1=离线 2=状态变更 3=远程重启 4=故障上报"),
        ("operator",    STR,  False, "管理员账号或 system"),
        ("detail",      STR,  True,  "事件详情。源库 NOT NULL DEFAULT ''，空串经 R001 归一为 NULL"),
        ("create_time", TS,   False, "事件时刻"),
        ("event_label", STR,  False, "[派生] R005 事件中文标签"),
    ],
    "dwd_carbon_daily": [
        ("daily_id",            LONG, False, "源库自增主键"),
        ("station_id",          LONG, False, "外键 → t_station"),
        ("stat_date",           DATE, False, "统计日期"),
        ("total_kwh_x100",      LONG, False, "当日总电量 × 100"),
        ("peak_kwh_x100",       LONG, False, "峰段电量 × 100"),
        ("flat_kwh_x100",       LONG, False, "平段电量 × 100"),
        ("valley_kwh_x100",     LONG, False, "谷段电量 × 100"),
        ("unalloc_kwh_x100",    LONG, False, "未分配到峰平谷的电量 × 100"),
        ("order_cnt",           LONG, False, "当日订单数"),
        ("emission_g",          LONG, False, "碳排放量，单位**克**"),
        ("intensity_g_per_kwh", LONG, False, "碳强度 克/度；-1 表示不可计算"),
        ("completeness",        LONG, False, "数据完整度 0–100；-1 表示未知"),
        ("estimated_pct",       LONG, False, "估算占比 0–100；-1 表示未知"),
        ("quality",             STR,  False, "METER_INTERVAL / UNIFORM_ESTIMATE / UNAVAILABLE"),
        ("factor_version",      STR,  False, "所用排放因子版本"),
        ("algo_version",        STR,  False, "算法版本"),
        ("cutoff_time",         TS,   False, "数据截止时刻"),
        ("update_time",         TS,   False, "更新时刻"),
    ],
    "dwd_carbon_factor": [
        ("factor_id",        LONG, False, "源库自增主键"),
        ("region",           STR,  False, "区域"),
        ("version",          STR,  False, "因子版本"),
        ("effect_from",      TS,   False, "生效起"),
        ("effect_to",        TS,   True,  "生效止；**当前生效的因子为空（开口区间，设计如此）**"),
        ("factor_g_per_kwh", LONG, False, "排放因子 克/度"),
        ("source",           STR,  False, "因子来源"),
        ("enabled",          LONG, False, "0=停用 1=启用"),
        ("create_time",      TS,   False, "创建时间"),
    ],
    "dwd_carbon_report": [
        ("report_id",           LONG, False, "源库自增主键"),
        ("scope",               STR,  False, "STATION / ALL"),
        ("station_id",          LONG, False, "0 表示全站范围"),
        ("date_from",           DATE, False, "报告起始日"),
        ("date_to",             DATE, False, "报告截止日"),
        ("version",             LONG, False, "报告版本，≥ 1"),
        ("status",              STR,  False, "GENERATING / READY / FAILED / STALE"),
        ("total_kwh_x100",      LONG, False, "总电量 × 100"),
        ("peak_kwh_x100",       LONG, False, "峰段电量 × 100"),
        ("flat_kwh_x100",       LONG, False, "平段电量 × 100"),
        ("valley_kwh_x100",     LONG, False, "谷段电量 × 100"),
        ("unalloc_kwh_x100",    LONG, False, "未分配电量 × 100"),
        ("emission_g",          LONG, False, "碳排放量，单位克"),
        ("intensity_g_per_kwh", LONG, False, "碳强度；-1 表示不可计算"),
        ("completeness",        LONG, False, "完整度 0–100；-1 表示未知"),
        ("factor_version",      STR,  True,  "因子版本。源库 DEFAULT ''，空串经 R001 归一为 NULL"),
        ("algo_version",        STR,  True,  "算法版本。源库 DEFAULT ''，空串经 R001 归一为 NULL"),
        ("tariff_mode",         STR,  True,  "峰平谷划分模式。源库 DEFAULT ''，空串经 R001 归一为 NULL"),
        ("cutoff_time",         STR,  True,  "数据截止时刻。按字符串保留；源库 DEFAULT ''，空串经 R001 归一为 NULL"),
        ("run_time",            TS,   False, "生成时刻"),
        ("output_path",         STR,  True,  "导出文件路径。**未导出的报告本就没有路径**（合理缺失，见质量报告 3.1）；源库 DEFAULT ''，空串经 R001 归一为 NULL"),
        ("req_id",              STR,  True,  "请求 id，可空"),
    ],
    "dwd_load_forecast": [
        ("forecast_id",   LONG, False, "源库自增主键"),
        ("station_id",    LONG, False, "外键 → t_station"),
        ("horizon",       LONG, False, "预测时长：1 / 6 / 24 小时"),
        ("predict_time",  TS,   False, "预测目标时刻"),
        ("load_kw",       DBL,  False, "预测充电负荷 kW"),
        ("idle_pile",     LONG, False, "预测空闲桩数"),
        ("is_peak",       LONG, False, "0=否 1=是高峰时段"),
        ("congestion",    DBL,  False, "拥堵度 0–1"),
        ("model_version", STR,  True,  "模型版本。源库 DEFAULT ''，空串经 R001 归一为 NULL"),
        ("create_time",   TS,   False, "生成时刻"),
    ],
    "dwd_wallet_tx": [
        ("tx_id",         LONG, False, "源库自增主键"),
        ("user_id",       LONG, False, "外键 → t_user"),
        ("type",          LONG, False, "0=充值 1=充电扣费 2=退款"),
        ("amount",        LONG, False, "变动金额，单位**分**，正数表示绝对值"),
        ("balance_after", LONG, False, "变动后余额，单位**分**"),
        ("order_id",      LONG, True,  "关联订单；**充值类流水不关联订单（合理缺失）**"),
        ("remark",        STR,  True,  "备注。源库 NOT NULL DEFAULT ''，空串经 R001 归一为 NULL"),
        ("create_time",   TS,   False, "发生时刻"),
    ],
    "dwd_station_review": [
        ("review_id",   LONG, False, "源库自增主键"),
        ("station_id",  LONG, False, "外键 → t_station"),
        ("user_id",     LONG, False, "外键 → t_user"),
        ("order_id",    LONG, True,  "关联订单，可空"),
        ("score",       LONG, False, "评分 1–5"),
        ("content",     STR,  True,  "评价内容。源库 NOT NULL DEFAULT ''，空串经 R001 归一为 NULL"),
        ("create_time", TS,   False, "评价时刻"),
    ],
}

#  源表 → DWD 表。三张平台运维表（t_sys_config / t_admin / t_admin_oplog）
#  有意不进 DWD，只在 ODS 留存，见 03_rules.md R009。
ODS_TO_DWD = {
    "t_order": "dwd_order", "t_user": "dwd_user", "t_pile": "dwd_pile",
    "t_station": "dwd_station", "t_pile_log": "dwd_pile_log",
    "t_carbon_daily": "dwd_carbon_daily", "t_carbon_factor": "dwd_carbon_factor",
    "t_carbon_report": "dwd_carbon_report", "t_load_forecast": "dwd_load_forecast",
    "t_wallet_tx": "dwd_wallet_tx", "t_station_review": "dwd_station_review",
}

#  派生列不在 ODS 里，转类型时要跳过——去 cast 一个不存在的列只会得到全 NULL。
DERIVED = {c for cols in DWD_SCHEMA.values()
           for c, _, _, note in cols if note.startswith("[派生]")}


def columns(table: str) -> list[str]:
    """声明的列顺序。DWD 落盘前按它 select，列序就不再随实现细节漂移。"""
    return [c for c, _, _, _ in DWD_SCHEMA[table]]


def cast_plan(table: str) -> dict[str, list[str]]:
    """转类型计划，只含来自 ODS 的列；派生列由 cleaning.py 自己生成。"""
    plan: dict[str, list[str]] = {LONG: [], DBL: [], TS: [], DATE: []}
    for c, t, _, note in DWD_SCHEMA[table]:
        if note.startswith("[派生]") or t in (STR, BOOL):
            continue
        plan[t].append(c)
    return plan


def non_null(table: str) -> list[str]:
    return [c for c, _, nullable, _ in DWD_SCHEMA[table] if not nullable]


#  契约类型 → Spark simpleString，用于断言实际落盘的 schema。
SPARK_TYPE = {LONG: "bigint", DBL: "double", TS: "timestamp",
              DATE: "date", STR: "string", BOOL: "boolean"}


def expected_types(table: str) -> dict[str, str]:
    return {c: SPARK_TYPE[t] for c, t, _, _ in DWD_SCHEMA[table]}


def as_dict() -> dict:
    """导出数据字典，供质量报告与归档使用。"""
    return {
        t: [{"列": c, "类型": SPARK_TYPE[ty], "可空": nullable, "说明": note}
            for c, ty, nullable, note in cols]
        for t, cols in DWD_SCHEMA.items()
    }


# ---------------------------------------------------------------------------
#  主键 / 外键 / 取值范围
#
#  同样是**声明而非散落在校验脚本里的 if**。来源全是源库的 PRIMARY KEY、
#  FOREIGN KEY 与 CHECK 约束——SQLite 建库时这些约束是生效的，但数据一旦离开
#  业务库进了 ODS 就没人替我们守了，DWD 必须自己查一遍。
# ---------------------------------------------------------------------------

#  (代理主键, 业务主键)。业务主键为 None 表示该表没有独立业务键，只查代理键。
KEYS: dict[str, tuple[str, str | None]] = {
    "dwd_order":          ("order_id", "order_no"),
    "dwd_user":           ("user_id", "phone"),
    "dwd_pile":           ("pile_id", "pile_code"),
    "dwd_station":        ("station_id", "name"),
    "dwd_pile_log":       ("log_id", None),
    "dwd_carbon_daily":   ("daily_id", None),
    "dwd_carbon_factor":  ("factor_id", None),
    "dwd_carbon_report":  ("report_id", None),
    "dwd_load_forecast":  ("forecast_id", None),
    "dwd_wallet_tx":      ("tx_id", None),
    "dwd_station_review": ("review_id", None),
}

#  (本表列, 目标表, 目标列)。列可空时，NULL 不算违规（如充值流水不关联订单）。
#  `dwd_carbon_report.station_id` **不是外键**——它用 0 表示「全站范围」，
#  照着建外键会把 5 条报告里的全站行全判成孤儿。
FOREIGN_KEYS: dict[str, list[tuple[str, str, str]]] = {
    "dwd_order": [("user_id", "dwd_user", "user_id"),
                  ("pile_id", "dwd_pile", "pile_id"),
                  ("station_id", "dwd_station", "station_id")],
    "dwd_pile": [("station_id", "dwd_station", "station_id")],
    "dwd_pile_log": [("pile_id", "dwd_pile", "pile_id")],
    "dwd_carbon_daily": [("station_id", "dwd_station", "station_id")],
    "dwd_load_forecast": [("station_id", "dwd_station", "station_id")],
    "dwd_wallet_tx": [("user_id", "dwd_user", "user_id"),
                      ("order_id", "dwd_order", "order_id")],
    "dwd_station_review": [("station_id", "dwd_station", "station_id"),
                           ("user_id", "dwd_user", "user_id"),
                           ("order_id", "dwd_order", "order_id")],
}

#  取值范围，直接抄源库 CHECK 约束，写成 Spark SQL 表达式。
#  **可空列的表达式必须自己处理 NULL**（写成 `x IS NULL OR …`）：
#  `NOT(expr)` 在 NULL 上得 NULL，会被 filter 丢掉，违规反而查不出来。
RANGE_RULES: dict[str, list[str]] = {
    "dwd_order": ["status IN (0,1,2,3,4)", "amount >= 0", "kwh_x100 >= 0", "price > 0",
                  "length(order_no) = 21",
                  "end_time IS NULL OR start_time IS NULL OR end_time >= start_time",
                  "start_time IS NULL OR start_time >= reserve_time",
                  "settle_time IS NULL OR end_time IS NULL OR settle_time >= end_time",
                  "order_hour BETWEEN 0 AND 23"],
    "dwd_user": ["status IN (0,1)", "balance >= 0", "length(phone) = 11"],
    "dwd_pile": ["type IN (0,1)", "status IN (0,1,2)", "online IN (0,1)", "power > 0",
                 "charge_count >= 0", "charge_duration >= 0"],
    "dwd_station": ["price > 0", "status IN (0,1)",
                    "lng BETWEEN -180 AND 180", "lat BETWEEN -90 AND 90"],
    "dwd_pile_log": ["event IN (0,1,2,3,4)"],
    "dwd_carbon_daily": [
        "completeness BETWEEN -1 AND 100", "estimated_pct BETWEEN -1 AND 100",
        "quality IN ('METER_INTERVAL','UNIFORM_ESTIMATE','UNAVAILABLE')",
        "total_kwh_x100 >= 0", "emission_g >= 0",
        #  源库 CHECK 里的等式：峰平谷 + 未分配必须恰好等于总量，差一度都不行
        "peak_kwh_x100 + flat_kwh_x100 + valley_kwh_x100 + unalloc_kwh_x100 = total_kwh_x100"],
    "dwd_carbon_factor": ["factor_g_per_kwh > 0", "enabled IN (0,1)",
                          "effect_to IS NULL OR effect_to > effect_from"],
    "dwd_carbon_report": ["scope IN ('STATION','ALL')",
                          "status IN ('GENERATING','READY','FAILED','STALE')",
                          "version >= 1", "date_to >= date_from",
                          "completeness BETWEEN -1 AND 100"],
    "dwd_load_forecast": ["horizon IN (1,6,24)", "is_peak IN (0,1)",
                          "load_kw >= 0", "congestion BETWEEN 0 AND 1"],
    "dwd_wallet_tx": ["type IN (0,1,2)", "amount > 0", "balance_after >= 0"],
    "dwd_station_review": ["score BETWEEN 1 AND 5"],
}

#  时间连续性：(日期列, 分组列或 None, 期望频率说明)。
#  按业务频率建立完整时间索引、识别缺失时间点——SOP 6.8 的要求。
TIME_SERIES: dict[str, tuple[str, str | None, str]] = {
    "dwd_order": ("order_date", None, "每日应有订单"),
    "dwd_carbon_daily": ("stat_date", "station_id", "每站每日一条"),
}

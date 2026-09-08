-- =============================================================================
--  docs/db-schema-ext-08.sql  —  扩展模块 08「碳减排与能源报告」建表脚本
--
--  属主 L5（模块 08 认领人），L2 复核。
--  规则见 docs/expand/00-扩展功能模块实施路径推荐.md 第 4.4 节：
--    只允许 CREATE TABLE IF NOT EXISTS / CREATE INDEX IF NOT EXISTS / INSERT OR IGNORE
--    禁止 DROP，禁止改动 docs/db-schema.sql 已有表
--    必须幂等：重复执行不报错、不产生重复种子数据
--
--  口径见 docs/expand/08-实现规划.md 第 2 节（已冻结）：
--    电量 kwh_x100（度 × 100 整数）  排放 emission_g（整数克）
--    因子 factor_g_per_kwh（整数克/度）  完整度 0..100 整数，-1 = 不适用
--    时间 TEXT 'YYYY-MM-DD HH:MM:SS' 本地时间，不做时区转换
--
--  只读业务表，只写 t_carbon_*。本脚本不修改任何既有表结构。
--
--  用法：sqlite3 charging.db < docs/db-schema-ext-08.sql
--        （scripts/build-all.sh 建库后会自动按序执行全部 db-schema-ext-*.sql）
-- =============================================================================

PRAGMA foreign_keys = ON;

-- -----------------------------------------------------------------------------
-- 08-1. t_carbon_factor  排放因子表
--       [本组自定] 整表为自定。08 文档第 4.2 节不变量 5：因子按生效区间选择，
--       区间统一左闭右开 [effect_from, effect_to)，effect_to 为 NULL 表示右开无穷。
--
--       ⚠ 区间重叠 SQLite 的表约束表达不了，由服务端 3742 CMD_EXT_FACTOR_SET
--         在写入前校验并返回 6702 ERR_CARBON_FACTOR_OVERLAP。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS t_carbon_factor (
    factor_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    region           TEXT    NOT NULL,                    -- 电网区域名
    version          TEXT    NOT NULL,                    -- 因子版本号，随结果一起追溯
    effect_from      TEXT    NOT NULL,                    -- 生效起（左闭）
    effect_to        TEXT,                                -- 生效止（右开），NULL = 无穷
    factor_g_per_kwh INTEGER NOT NULL,                    -- 整数克 CO2e / 度
    source           TEXT    NOT NULL,                    -- 来源说明，报告页必须展示
    enabled          INTEGER NOT NULL DEFAULT 1,          -- 0=停用 1=启用
    create_time      TEXT    NOT NULL,
    UNIQUE (region, version),
    CHECK (factor_g_per_kwh > 0),
    CHECK (enabled IN (0, 1)),
    CHECK (effect_to IS NULL OR effect_to > effect_from)
);
CREATE INDEX IF NOT EXISTS idx_carbon_factor_eff ON t_carbon_factor(region, effect_from);

-- -----------------------------------------------------------------------------
-- 08-2. t_carbon_daily  日聚合指标表
--       [本组自定] 整表为自定。
--
--       幂等：UNIQUE(station_id, stat_date, factor_version, algo_version)
--             重算一律 INSERT OR REPLACE，同一范围重跑结果不变、不产生重复行。
--
--       ⚠ station_id = 0 不使用。全站数字一律由分站行求和得到 ——
--         存两份可能对不上的总量，是对账灾难的开始。
--
--       ⚠ 日归属：整单按 date(settle_time)，与 2301/2302 营收同源。
--         峰平谷则按订单真实钟点切分，可跨两天（实测 553 单跨午夜，占 8.5%）。
--         见 docs/expand/08-实现规划.md 裁决 D3 —— 这不是 bug，不要"修正"。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS t_carbon_daily (
    daily_id            INTEGER PRIMARY KEY AUTOINCREMENT,
    station_id          INTEGER NOT NULL,
    stat_date           TEXT    NOT NULL,                 -- 'YYYY-MM-DD'
    total_kwh_x100      INTEGER NOT NULL DEFAULT 0,       -- = SUM(t_order.kwh_x100 WHERE status=3)
    peak_kwh_x100       INTEGER NOT NULL DEFAULT 0,
    flat_kwh_x100       INTEGER NOT NULL DEFAULT 0,
    valley_kwh_x100     INTEGER NOT NULL DEFAULT 0,
    unalloc_kwh_x100    INTEGER NOT NULL DEFAULT 0,       -- 无法分摊（缺起止时间等）
    order_cnt           INTEGER NOT NULL DEFAULT 0,       -- 懒聚合据此判断该日是否需要重算
    emission_g          INTEGER NOT NULL DEFAULT 0,       -- (kwh_x100 * factor + 50) / 100
    intensity_g_per_kwh INTEGER NOT NULL DEFAULT -1,      -- 总量为 0 时 -1 = 不适用
    completeness        INTEGER NOT NULL DEFAULT -1,      -- 0..100，总量为 0 时 -1 = 不适用
    estimated_pct       INTEGER NOT NULL DEFAULT -1,      -- UNIFORM_ESTIMATE 电量占比
    quality             TEXT    NOT NULL,                 -- 见下 CHECK
    factor_version      TEXT    NOT NULL,
    algo_version        TEXT    NOT NULL,
    cutoff_time         TEXT    NOT NULL,                 -- 数据截止时间，结果可追溯
    update_time         TEXT    NOT NULL,
    UNIQUE (station_id, stat_date, factor_version, algo_version),
    FOREIGN KEY (station_id) REFERENCES t_station(station_id),
    -- METER_INTERVAL 表计区间实测；UNIFORM_ESTIMATE 按时长估算；UNAVAILABLE 无法分摊
    -- ⚠ MVP 中 METER_INTERVAL 永不出现：t_order 没有表计区间数据。列予保留，
    --   待模块 02 / 06 接入 t_energy_sample 后才可能取到该值。
    CHECK (quality IN ('METER_INTERVAL', 'UNIFORM_ESTIMATE', 'UNAVAILABLE')),
    CHECK (completeness  BETWEEN -1 AND 100),
    CHECK (estimated_pct BETWEEN -1 AND 100),
    -- 分时守恒：四项之和必须等于总量，逐日成立（最大余数法保证，见实现规划第 2 节）
    CHECK (peak_kwh_x100 + flat_kwh_x100 + valley_kwh_x100 + unalloc_kwh_x100 = total_kwh_x100)
);
CREATE INDEX IF NOT EXISTS idx_carbon_daily_date ON t_carbon_daily(stat_date, station_id);

-- -----------------------------------------------------------------------------
-- 08-3. t_carbon_report  报告表（不可变版本快照）
--       [本组自定] 整表为自定。08 文档第 4.2 节不变量 6：
--       迟到或纠正数据不能静默覆盖已生成报告，只能把旧版标记 STALE 并生成新版本。
--
--       req_id UNIQUE 实现写幂等（00 第 4.6 节）：重复请求命中唯一约束时
--       读回首次结果原样返回，不报错。
-- -----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS t_carbon_report (
    report_id           INTEGER PRIMARY KEY AUTOINCREMENT,
    scope               TEXT    NOT NULL,                 -- 'STATION' | 'ALL'
    station_id          INTEGER NOT NULL DEFAULT 0,       -- scope='ALL' 时为 0
    date_from           TEXT    NOT NULL,
    date_to             TEXT    NOT NULL,
    version             INTEGER NOT NULL DEFAULT 1,       -- 同一范围重算时递增，旧版保留
    status              TEXT    NOT NULL,                 -- 见下 CHECK
    -- 生成当时的指标快照。源数据变化后本行数字不变，只把 status 置 STALE
    total_kwh_x100      INTEGER NOT NULL DEFAULT 0,
    peak_kwh_x100       INTEGER NOT NULL DEFAULT 0,
    flat_kwh_x100       INTEGER NOT NULL DEFAULT 0,
    valley_kwh_x100     INTEGER NOT NULL DEFAULT 0,
    unalloc_kwh_x100    INTEGER NOT NULL DEFAULT 0,
    emission_g          INTEGER NOT NULL DEFAULT 0,
    intensity_g_per_kwh INTEGER NOT NULL DEFAULT -1,
    completeness        INTEGER NOT NULL DEFAULT -1,
    factor_version      TEXT    NOT NULL DEFAULT '',
    algo_version        TEXT    NOT NULL DEFAULT '',
    tariff_mode         TEXT    NOT NULL DEFAULT '',      -- 'FIXED_RANGE' 固定时段口径（模块 05 未完成）
    cutoff_time         TEXT    NOT NULL DEFAULT '',
    run_time            TEXT    NOT NULL,
    output_path         TEXT    NOT NULL DEFAULT '',      -- 导出文件相对路径，文件不走 Socket
    req_id              TEXT    UNIQUE,                   -- 写幂等键（00 第 4.6 节）
    CHECK (scope IN ('STATION', 'ALL')),
    CHECK (status IN ('GENERATING', 'READY', 'FAILED', 'STALE')),
    CHECK (version >= 1),
    CHECK (date_to >= date_from)
);
CREATE INDEX IF NOT EXISTS idx_carbon_report_scope
    ON t_carbon_report(scope, station_id, date_from, date_to, version);

-- =============================================================================
--  种子数据（INSERT OR IGNORE，可重复执行）
-- =============================================================================

-- 演示排放因子。
-- ⚠ 来源写明是"演示因子"是硬性要求，见 08 文档第 9 节风险表「排放因子来源不可靠」：
--   有明确来源才允许在报告里展示，且全程标注非认证。
INSERT OR IGNORE INTO t_carbon_factor
    (region, version, effect_from, effect_to, factor_g_per_kwh, source, enabled, create_time)
VALUES
    ('全国电网平均', 'demo-2022', '2000-01-01 00:00:00', NULL, 581,
     '生态环境部 2022 年全国电网平均二氧化碳排放因子 0.5810 tCO2/MWh；课程项目演示用，非认证数据',
     1, datetime('now','localtime'));

-- 功能开关与固定时段常量。
-- ⚠ 这是本脚本唯一写既有表的地方，00 第 4.4 节明文允许 INSERT OR IGNORE；
--   运行期一律只读（ml/CLAUDE.md 禁止 L5 运行期脚本写 t_sys_config）。
-- ⚠ 平段不单独配：= 24 小时扣掉峰段与谷段。三个都配会出现"配不满 24 小时"的无解状态。
-- ⚠ 谷段 23:00-07:00 跨午夜，解析器必须处理回绕。
INSERT OR IGNORE INTO t_sys_config (cfg_key, cfg_value, remark, update_time) VALUES
('feat_08_carbon',      '1',                    '[本组自定] 扩展模块 08 碳减排与能源报告功能开关，0=关闭时服务端不注册 handler', datetime('now','localtime')),
('carbon_peak_ranges',  '10:00-15:00,18:00-21:00', '[本组自定] 模块 08 峰段（左闭右开）。模块 05 分时电价完成前的降级口径，页面须标注"固定时段口径"', datetime('now','localtime')),
('carbon_valley_ranges','23:00-07:00',          '[本组自定] 模块 08 谷段（左闭右开，跨午夜）。平段 = 24h 扣除峰段与谷段，不单独配置',        datetime('now','localtime'));

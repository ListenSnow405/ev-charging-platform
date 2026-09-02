-- =============================================================================
--  docs/db-schema.sql  —  电动汽车充电桩应用管理平台 数据库结构
--
--  冻结契约 · 属主 L2，其他人只读。变更走 CLAUDE.md 第 3 节流程。
--
--  标记约定（见 CLAUDE.md 第 0 节）：
--    [说明书]    说明书正文明文要求，不得擅自更改
--    [本组自定]  说明书未规定，本组决定，可评审后调整
--
--  [说明书] 1.6  数据库使用 QSQLite
--  [说明书] 1.4  数据库端需存储：用户信息、充电站信息、充电桩信息、充电订单、管理员账号
--  [说明书] 2.2  数据库设计，能够实现数据库开发与管理；合理考虑数据安全问题
--
--  [本组自定] 全部表名、字段名、字段类型、枚举取值编号、索引
--  [本组自定] 金额一律用 INTEGER 存「分」，禁止 REAL —— 避免浮点误差导致对账差额
--  [本组自定] 时间一律用 TEXT 'YYYY-MM-DD HH:MM:SS' 本地时间
--             理由：便于用 date(x) 直接按日聚合近 7 / 30 日营收（说明书 1.4 要求）
--
--  用法：sqlite3 charging.db < docs/db-schema.sql
-- =============================================================================

PRAGMA foreign_keys = ON;

-- -----------------------------------------------------------------------------
-- 1. t_user  用户信息表
--    [说明书] 1.4 用户列表需含：用户ID、手机号、昵称、钱包余额、注册时间、状态
--    [说明书] 1.4 手机号免密登录；不存在则自动注册，默认昵称「用户」+ 手机号后 4 位
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_user;
CREATE TABLE t_user (
    user_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    phone        TEXT    NOT NULL UNIQUE,              -- [说明书] 11 位手机号
    nickname     TEXT    NOT NULL,                     -- [说明书] 默认「用户」+ 后 4 位
    avatar       TEXT    NOT NULL DEFAULT '',          -- [本组自定] 本地图片路径，空串表示默认灰色头像
    balance      INTEGER NOT NULL DEFAULT 0,           -- [本组自定] 钱包余额，单位：分
    status       INTEGER NOT NULL DEFAULT 0,           -- [说明书] 状态：0=正常 1=冻结
    create_time  TEXT    NOT NULL,                     -- [说明书] 注册时间
    update_time  TEXT    NOT NULL,
    CHECK (length(phone) = 11),
    CHECK (balance >= 0),
    CHECK (status IN (0, 1))
);
CREATE INDEX idx_user_phone ON t_user(phone);          -- [说明书] 1.4 支持手机号模糊搜索

-- -----------------------------------------------------------------------------
-- 2. t_admin  管理员表
--    [说明书] 1.4 账号密码存储在数据库管理员表中，默认初始账号 admin / 123456
--    [本组自定] 密码存 SHA-256 摘要而非明文（对应说明书 2.2「合理考虑数据安全问题」）
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_admin;
CREATE TABLE t_admin (
    admin_id     INTEGER PRIMARY KEY AUTOINCREMENT,
    account      TEXT    NOT NULL UNIQUE,
    password     TEXT    NOT NULL,                     -- [本组自定] SHA-256 十六进制小写
    real_name    TEXT    NOT NULL DEFAULT '',
    status       INTEGER NOT NULL DEFAULT 0,           -- [本组自定] 0=正常 1=停用
    create_time  TEXT    NOT NULL,
    last_login   TEXT,
    CHECK (status IN (0, 1))
);

-- -----------------------------------------------------------------------------
-- 3. t_station  充电站表
--    [说明书] 1.4 电站列表需含：充电站ID、站名、详细地址、经纬度、总电桩数、当前在线率
--    [说明书] 1.4 用户端站点卡片需含：站名、充电价格（元/度）、电桩总数/空闲数、距离
--    [本组自定] 在线率与空闲数为实时统计值，不落库，由 t_pile 现算
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_station;
CREATE TABLE t_station (
    station_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    name         TEXT    NOT NULL,                     -- [说明书] 站名
    address      TEXT    NOT NULL,                     -- [说明书] 详细地址
    lng          REAL    NOT NULL,                     -- [说明书] 经度（腾讯地图 API 解析所得）
    lat          REAL    NOT NULL,                     -- [说明书] 纬度
    price        INTEGER NOT NULL,                     -- [说明书] 充电价格；[本组自定] 单位：分/度
    status       INTEGER NOT NULL DEFAULT 0,           -- [本组自定] 0=营业 1=停业
    create_time  TEXT    NOT NULL,
    CHECK (price > 0),
    CHECK (status IN (0, 1))
);

-- -----------------------------------------------------------------------------
-- 4. t_pile  充电桩表
--    [说明书] 1.4 电桩列表需含：电桩编号、所属电站、类型（快充/慢充）、功率(kW)、
--                 当前状态、累计充电次数、累计充电时长
--    [说明书] 1.4 电桩状态三种：在用 / 闲置 / 故障
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_pile;
CREATE TABLE t_pile (
    pile_id         INTEGER PRIMARY KEY AUTOINCREMENT,
    pile_code       TEXT    NOT NULL UNIQUE,           -- [说明书] 电桩编号，如 SZ001-03
    station_id      INTEGER NOT NULL,                  -- [说明书] 所属电站
    type            INTEGER NOT NULL,                  -- [说明书] 类型：0=快充 1=慢充
    power           REAL    NOT NULL,                  -- [说明书] 额定功率，单位 kW
    status          INTEGER NOT NULL DEFAULT 1,        -- [说明书] 状态：0=在用 1=闲置 2=故障
    charge_count    INTEGER NOT NULL DEFAULT 0,        -- [说明书] 累计充电次数
    charge_duration INTEGER NOT NULL DEFAULT 0,        -- [说明书] 累计充电时长；[本组自定] 单位：秒
    online          INTEGER NOT NULL DEFAULT 0,        -- [本组自定] 0=离线 1=在线，用于统计在线率与重启下发
    last_heartbeat  TEXT,                              -- [本组自定] 最近心跳时间
    create_time     TEXT    NOT NULL,
    FOREIGN KEY (station_id) REFERENCES t_station(station_id),
    CHECK (type   IN (0, 1)),
    CHECK (status IN (0, 1, 2)),
    CHECK (online IN (0, 1)),
    CHECK (power > 0)
);
CREATE INDEX idx_pile_station ON t_pile(station_id);
CREATE INDEX idx_pile_status  ON t_pile(status);

-- -----------------------------------------------------------------------------
-- 5. t_order  充电订单表
--    [说明书] 1.4 完整流程：预约 → 开始充电 → 计费 → 订单结算
--    [说明书] 1.4 进入充电页需查询是否存在「充电中」状态的未完成订单
--    [本组自定] 状态编号；金额单位分；电量放大 100 倍存整数避免浮点误差
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_order;
CREATE TABLE t_order (
    order_id     INTEGER PRIMARY KEY AUTOINCREMENT,
    order_no     TEXT    NOT NULL UNIQUE,              -- [本组自定] 业务单号 ORD + yyyyMMddHHmmss + 4 位随机
    user_id      INTEGER NOT NULL,
    pile_id      INTEGER NOT NULL,
    station_id   INTEGER NOT NULL,                     -- [本组自定] 冗余，便于按站统计营收
    status       INTEGER NOT NULL DEFAULT 0,           -- [本组自定] 0=已预约 1=充电中 2=待结算 3=已结算 4=已取消
    price        INTEGER NOT NULL,                     -- [本组自定] 下单时单价快照，单位分/度
    kwh_x100     INTEGER NOT NULL DEFAULT 0,           -- [本组自定] 充电量 × 100，避免浮点
    amount       INTEGER NOT NULL DEFAULT 0,           -- [本组自定] 应付金额，单位：分
    reserve_time TEXT    NOT NULL,                     -- 预约时刻
    start_time   TEXT,                                 -- 开始充电时刻
    end_time     TEXT,                                 -- 结束充电时刻
    settle_time  TEXT,                                 -- 结算时刻
    FOREIGN KEY (user_id)    REFERENCES t_user(user_id),
    FOREIGN KEY (pile_id)    REFERENCES t_pile(pile_id),
    FOREIGN KEY (station_id) REFERENCES t_station(station_id),
    CHECK (status IN (0, 1, 2, 3, 4)),
    CHECK (amount >= 0)
);
-- [说明书] 1.4 未结算订单校验：按 user_id + status IN (0,1,2) 查询
CREATE INDEX idx_order_user_status ON t_order(user_id, status);
-- [说明书] 1.4 近 7/30 日营收趋势：按结算时刻聚合
CREATE INDEX idx_order_settle      ON t_order(settle_time);
CREATE INDEX idx_order_station     ON t_order(station_id);

-- -----------------------------------------------------------------------------
-- 6. t_wallet_tx  钱包流水表
--    [说明书] 1.4 余额充值（模拟支付成功，余额实时更新）
--    [本组自定] 整表为自定；充值与扣费均留痕，便于对账与答辩演示
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_wallet_tx;
CREATE TABLE t_wallet_tx (
    tx_id         INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id       INTEGER NOT NULL,
    type          INTEGER NOT NULL,                    -- [本组自定] 0=充值 1=充电扣费 2=退款
    amount        INTEGER NOT NULL,                    -- [本组自定] 单位分，正数表示金额绝对值
    balance_after INTEGER NOT NULL,                    -- [本组自定] 变动后余额，单位分
    order_id      INTEGER,                             -- 充值时为 NULL
    remark        TEXT    NOT NULL DEFAULT '',
    create_time   TEXT    NOT NULL,
    FOREIGN KEY (user_id) REFERENCES t_user(user_id),
    CHECK (type IN (0, 1, 2)),
    CHECK (amount > 0)
);
CREATE INDEX idx_tx_user ON t_wallet_tx(user_id, create_time);

-- -----------------------------------------------------------------------------
-- 7. t_pile_log  设备日志表
--    [说明书] 1.4 远程重启（模拟向电桩发送重启指令），用于处理死机等异常
--    [本组自定] 表结构；同时承载状态变更与指令下发记录，供大屏与机器学习取数
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_pile_log;
CREATE TABLE t_pile_log (
    log_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    pile_id     INTEGER NOT NULL,
    event       INTEGER NOT NULL,                      -- [本组自定] 0=上线 1=离线 2=状态变更 3=远程重启 4=故障上报
    old_status  INTEGER,
    new_status  INTEGER,
    operator    TEXT    NOT NULL DEFAULT 'system',     -- [本组自定] 管理员账号或 system
    detail      TEXT    NOT NULL DEFAULT '',
    create_time TEXT    NOT NULL,
    FOREIGN KEY (pile_id) REFERENCES t_pile(pile_id),
    CHECK (event IN (0, 1, 2, 3, 4))
);
CREATE INDEX idx_pilelog_pile ON t_pile_log(pile_id, create_time);

-- -----------------------------------------------------------------------------
-- 8. t_admin_oplog  管理员操作日志表
--    [本组自定] 整表为自定，对应说明书 2.2「合理考虑数据安全问题」：
--               冻结/解冻、远程重启、新增电站等敏感操作留痕
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_admin_oplog;
CREATE TABLE t_admin_oplog (
    op_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    admin_id    INTEGER NOT NULL,
    action      TEXT    NOT NULL,                      -- 如 FREEZE_USER / REBOOT_PILE / ADD_STATION
    target      TEXT    NOT NULL DEFAULT '',
    detail      TEXT    NOT NULL DEFAULT '',
    create_time TEXT    NOT NULL,
    FOREIGN KEY (admin_id) REFERENCES t_admin(admin_id)
);

-- -----------------------------------------------------------------------------
-- 9. t_station_review  站点评价表
--    [本组自定] 说明书未要求，作为加分项预留（说明书 1.4 注：可增加额外功能并加分）
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_station_review;
CREATE TABLE t_station_review (
    review_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    station_id  INTEGER NOT NULL,
    user_id     INTEGER NOT NULL,
    order_id    INTEGER,
    score       INTEGER NOT NULL,                      -- 1..5
    content     TEXT    NOT NULL DEFAULT '',
    create_time TEXT    NOT NULL,
    FOREIGN KEY (station_id) REFERENCES t_station(station_id),
    FOREIGN KEY (user_id)    REFERENCES t_user(user_id),
    CHECK (score BETWEEN 1 AND 5)
);

-- -----------------------------------------------------------------------------
-- 10. t_load_forecast  负荷预测结果表
--     [说明书] 1.4 预测未来 1h / 6h / 24h 各站点的充电负荷、空闲桩数量、高峰时段；
--                  用户端据此优先推荐低拥堵高空闲率站点，运营端负荷预警
--     [本组自定] 表结构。由 L5 的 Python 脚本写入，服务端与大屏只读
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_load_forecast;
CREATE TABLE t_load_forecast (
    forecast_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    station_id    INTEGER NOT NULL,
    horizon       INTEGER NOT NULL,                    -- [说明书] 预测时长：1 / 6 / 24（小时）
    predict_time  TEXT    NOT NULL,                    -- 预测目标时刻
    load_kw       REAL    NOT NULL,                    -- [说明书] 预测充电负荷
    idle_pile     INTEGER NOT NULL,                    -- [说明书] 预测空闲桩数量
    is_peak       INTEGER NOT NULL DEFAULT 0,          -- [说明书] 是否高峰时段：0=否 1=是
    congestion    REAL    NOT NULL DEFAULT 0,          -- [本组自定] 拥堵度 0..1，供用户端推荐排序
    model_version TEXT    NOT NULL DEFAULT '',
    create_time   TEXT    NOT NULL,
    FOREIGN KEY (station_id) REFERENCES t_station(station_id),
    CHECK (horizon IN (1, 6, 24)),
    CHECK (is_peak IN (0, 1))
);
CREATE INDEX idx_forecast_station ON t_load_forecast(station_id, horizon, predict_time);

-- -----------------------------------------------------------------------------
-- 11. t_sys_config  系统配置表
--     [本组自定] 整表为自定。避免把参数写死在代码里，便于演示时调整
-- -----------------------------------------------------------------------------
DROP TABLE IF EXISTS t_sys_config;
CREATE TABLE t_sys_config (
    cfg_key     TEXT PRIMARY KEY,
    cfg_value   TEXT NOT NULL,
    remark      TEXT NOT NULL DEFAULT '',
    update_time TEXT NOT NULL
);

-- =============================================================================
--  种子数据
-- =============================================================================

-- [说明书] 1.4 默认初始账号 admin / 123456
-- password 为 SHA-256('123456') 的十六进制小写：
--   8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92
INSERT INTO t_admin (account, password, real_name, status, create_time) VALUES
('admin', '8d969eef6ecad3c29a3a629280e686cf0c3f5d5a86aff3ca12020c923adc6c92',
 '系统管理员', 0, datetime('now','localtime'));

-- [本组自定] 演示用充电站（坐标取深圳，与界面参考图一致）
INSERT INTO t_station (name, address, lng, lat, price, status, create_time) VALUES
('福田CBD充电站',   '深圳市福田区福华三路 88 号',   114.0579, 22.5410, 152, 0, datetime('now','localtime')),
('南山科技园充电站', '深圳市南山区科苑南路 3009 号', 113.9455, 22.5390, 148, 0, datetime('now','localtime')),
('深圳市民中心充电站','深圳市福田区福中三路 1 号',   114.0650, 22.5470, 160, 0, datetime('now','localtime')),
('深圳湾公园充电站', '深圳市南山区望海路 99 号',     113.9720, 22.5130, 145, 0, datetime('now','localtime')),
('宝安中心充电站',   '深圳市宝安区新湖路 99 号',     113.8830, 22.5550, 140, 0, datetime('now','localtime')),
('龙岗大运充电站',   '深圳市龙岗区龙翔大道 8 号',    114.2180, 22.6900, 138, 0, datetime('now','localtime'));

-- [本组自定] 每站 4 个电桩：2 快充 + 2 慢充；状态覆盖 在用/闲置/故障 三种
INSERT INTO t_pile (pile_code, station_id, type, power, status, charge_count, charge_duration, online, create_time)
SELECT
    printf('SZ%03d-%02d', s.station_id, n.i)                     AS pile_code,
    s.station_id,
    CASE WHEN n.i <= 2 THEN 0 ELSE 1 END                         AS type,   -- 0=快充 1=慢充
    CASE WHEN n.i <= 2 THEN 120.0 ELSE 7.0 END                   AS power,
    CASE WHEN (s.station_id + n.i) % 7 = 0 THEN 2                           -- 故障
         WHEN (s.station_id + n.i) % 3 = 0 THEN 0                           -- 在用
         ELSE 1 END                                              AS status, -- 闲置
    0, 0, 1,
    datetime('now','localtime')
FROM t_station s
CROSS JOIN (SELECT 1 AS i UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4) n;

-- [本组自定] 演示用户，与界面参考图的提示账号一致
INSERT INTO t_user (phone, nickname, avatar, balance, status, create_time, update_time) VALUES
('13800138001', '用户8001', '', 20000, 0, datetime('now','localtime'), datetime('now','localtime')),
('13800138002', '用户8002', '',  5000, 0, datetime('now','localtime'), datetime('now','localtime')),
('13800138004', '用户8004', '',   150, 0, datetime('now','localtime'), datetime('now','localtime')),
('13800138006', '用户8006', '',  8000, 1, datetime('now','localtime'), datetime('now','localtime'));
--                                        ↑ status=1 冻结，用于演示 [说明书] 1.4 风控场景

INSERT INTO t_sys_config (cfg_key, cfg_value, remark, update_time) VALUES
('token_ttl_sec',    '7200', '[本组自定] token 有效期（秒），与 docs/protocol.md 第 6 节一致', datetime('now','localtime')),
('thread_pool_size', '8',    '[本组自定] pthread 线程池大小，与 docs/protocol.md 第 7 节一致', datetime('now','localtime')),
('recharge_max',     '100000','[本组自定] 单次充值上限（分）',                                  datetime('now','localtime')),
('server_port',      '9527', '[本组自定] 服务端监听端口',                                      datetime('now','localtime'));

-- =============================================================================
--  常用查询参考（供 L2 实现统计服务时对照，[说明书] 1.4 管理端指标）
-- =============================================================================
--  今日营收：
--    SELECT IFNULL(SUM(amount),0) FROM t_order
--     WHERE status = 3 AND date(settle_time) = date('now','localtime');
--
--  近 7 日营收趋势：
--    SELECT date(settle_time) AS d, IFNULL(SUM(amount),0) AS amt FROM t_order
--     WHERE status = 3 AND settle_time >= datetime('now','localtime','-7 day')
--     GROUP BY d ORDER BY d;
--
--  电桩状态分布（在用/闲置/故障）：
--    SELECT status, COUNT(*) FROM t_pile GROUP BY status;
--
--  某用户未完成订单（进入充电页必查）：
--    SELECT * FROM t_order WHERE user_id = ? AND status IN (0,1,2) LIMIT 1;
-- =============================================================================

#pragma once
// =============================================================================
//  common/protocol.h  —  命令字与报文信封
//
//  冻结契约 · 属主 L1，其他人只读。变更走 CLAUDE.md 第 3 节流程。
//  本文件必须与 docs/protocol.md 第 3、4 节 **完全一致**。
//
//  [说明书] 1.6  网络通信由 Socket 编程实现
//  [本组自定]    命令字编号、信封字段名
// =============================================================================

#include <QJsonDocument>
#include <QJsonObject>
#include "error_code.h"

namespace ecp {

// ---- 命令字（docs/protocol.md 第 4 节）----------------------------------------
enum Cmd {
    // 用户端 · 用户与鉴权 1000–1099
    CMD_USER_LOGIN          = 1001,   // [说明书] 手机号免密登录 / 首次自动注册
    CMD_USER_INFO           = 1002,
    CMD_USER_SET_NICKNAME   = 1003,
    CMD_USER_SET_AVATAR     = 1004,
    CMD_USER_RECHARGE       = 1005,   // [说明书] 钱包充值（模拟支付）
    CMD_WALLET_TX_LIST      = 1006,

    // 用户端 · 充电站与电桩 1100–1199
    CMD_STATION_NEARBY      = 1101,   // [说明书] 附近充电站，按距离升序
    CMD_STATION_PILES       = 1102,   // [说明书] 站内电桩详情

    // 用户端 · 充电与订单 1200–1299
    CMD_ORDER_UNFINISHED    = 1201,   // [说明书] 进入充电页必调
    CMD_ORDER_RESERVE       = 1202,
    CMD_ORDER_START         = 1203,
    CMD_ORDER_STOP          = 1204,
    CMD_ORDER_SETTLE        = 1205,
    CMD_ORDER_CANCEL        = 1206,
    CMD_ORDER_LIST          = 1207,
    CMD_ORDER_PUSH          = 1208,   // 服务端推送：充电中实时数据

    // 管理端 2000–2399
    CMD_ADMIN_LOGIN         = 2001,   // [说明书] 默认 admin / 123456
    CMD_STATION_LIST        = 2101,
    CMD_STATION_ADD         = 2102,
    CMD_STATION_DETAIL      = 2103,
    CMD_PILE_LIST           = 2111,
    CMD_PILE_REBOOT         = 2112,   // [说明书] 远程重启
    CMD_ADMIN_USER_LIST     = 2201,   // [说明书] 含手机号模糊搜索
    CMD_ADMIN_USER_STATUS   = 2202,   // [说明书] 冻结 / 解冻
    CMD_STAT_REVENUE        = 2301,   // [说明书] 今日 / 本月 / 总营收
    CMD_STAT_REVENUE_TREND  = 2302,   // [说明书] 近 7 / 30 日趋势
    CMD_STAT_PILE_STATUS    = 2303,   // [说明书] 在用 / 闲置 / 故障分布
    CMD_ADMIN_ORDER_LIST    = 2304,
    CMD_STAT_LOAD_FORECAST  = 2305,   // [说明书] 站点负荷预测 / 负荷预警

    // 设备侧 · 电桩模拟器 9000–9099
    CMD_DEV_REGISTER        = 9001,
    CMD_DEV_REPORT          = 9002,
    CMD_DEV_REBOOT          = 9003,   // [说明书] 重启指令下发
    CMD_DEV_HEARTBEAT       = 9004
};

// ---- 业务枚举（与 docs/db-schema.sql 一致）--------------------------------------
enum PileStatus { PILE_IN_USE = 0, PILE_IDLE = 1, PILE_FAULT = 2 };   // [说明书] 在用/闲置/故障
enum PileType   { PILE_FAST   = 0, PILE_SLOW = 1 };                   // [说明书] 快充/慢充
enum UserStatus { USER_NORMAL = 0, USER_FROZEN = 1 };                 // [说明书] 正常/冻结
enum OrderStatus {                                                    // [说明书] 预约→充电→计费→结算
    ORDER_RESERVED = 0, ORDER_CHARGING = 1, ORDER_TO_SETTLE = 2,
    ORDER_SETTLED  = 3, ORDER_CANCELLED = 4
};

// ---- 报文信封（docs/protocol.md 第 3 节）--------------------------------------
inline QByteArray buildRequest(int cmd, int seq, const QString &token,
                               const QJsonObject &data = QJsonObject())
{
    QJsonObject o;
    o["cmd"]   = cmd;
    o["seq"]   = seq;
    o["token"] = token;
    o["data"]  = data;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

inline QByteArray buildResponse(int cmd, int seq, int code,
                                const QJsonObject &data = QJsonObject())
{
    QJsonObject o;
    o["cmd"]  = cmd;
    o["seq"]  = seq;
    o["code"] = code;
    o["msg"]  = errMsg(code);
    o["data"] = data;
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

// 服务端主动推送：seq 与 code 固定为 0
inline QByteArray buildPush(int cmd, const QJsonObject &data = QJsonObject())
{
    return buildResponse(cmd, 0, ERR_OK, data);
}

// 解析报文。失败返回 false，上层应回 ERR_FRAME。
inline bool parseEnvelope(const QByteArray &payload, QJsonObject &out)
{
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(payload, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return false;
    out = doc.object();
    return out.contains("cmd");
}

} // namespace ecp

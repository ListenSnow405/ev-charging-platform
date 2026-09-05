#pragma once
// =============================================================================
//  common/error_code.h  —  全项目统一错误码
//
//  冻结契约 · 属主 L1，其他人只读。变更走 CLAUDE.md 第 3 节流程。
//  本文件必须与 docs/protocol.md 第 5 节的错误码表 **完全一致**，改一处必须同步另一处。
//
//  [说明书] 2.3  需设计完整的错误处理机制
//  [本组自定]    具体错误码编号与分段规则
// =============================================================================

#include <QString>

namespace ecp {   // ecp = EV Charging Platform

enum ErrCode {
    ERR_OK                  = 0,      // 成功

    // ---- 1000 段 · 通用 ----
    ERR_PARAM               = 1001,   // 参数缺失或格式错误
    ERR_NOT_LOGIN           = 1002,   // 未登录 / token 为空
    ERR_TOKEN_INVALID       = 1003,   // token 无效或已过期
    ERR_NO_PERMISSION       = 1004,   // 无权限
    ERR_CMD_UNKNOWN         = 1005,   // 未知命令字
    ERR_FRAME               = 1006,   // 报文格式错误
    ERR_INTERNAL            = 1099,   // 服务端内部错误

    // ---- 2000 段 · 用户 ----
    ERR_PHONE_FORMAT        = 2001,   // [说明书] 手机号格式错误
    ERR_USER_NOT_FOUND      = 2002,   // 用户不存在
    ERR_USER_FROZEN         = 2003,   // [说明书] 账号已冻结
    ERR_BALANCE_NOT_ENOUGH  = 2004,   // 钱包余额不足
    ERR_AMOUNT_INVALID      = 2005,   // 金额非法

    // ---- 3000 段 · 电站与电桩 ----
    ERR_STATION_NOT_FOUND   = 3001,   // 充电站不存在
    ERR_PILE_NOT_FOUND      = 3002,   // 电桩不存在
    ERR_PILE_BUSY           = 3003,   // 电桩已被占用
    ERR_PILE_FAULT          = 3004,   // [说明书] 电桩故障
    ERR_PILE_OFFLINE        = 3005,   // 电桩未上线，指令无法下发

    // ---- 4000 段 · 订单 ----
    ERR_ORDER_NOT_FOUND     = 4001,   // 订单不存在
    ERR_ORDER_UNFINISHED    = 4002,   // [说明书] 存在未结算订单，请先结算
    ERR_ORDER_STATUS        = 4003,   // 订单状态不允许此操作
    ERR_ORDER_SETTLED       = 4004,   // 订单已结算，不可重复结算

    // ---- 5000 段 · 管理员 ----
    ERR_ADMIN_AUTH          = 5001    // [说明书] 账号或密码错误
};

// 错误码 → 面向用户的中文描述。
// [说明书] 2.3：错误提示必须说清发生了什么，不要只回「操作失败」。
inline QString errMsg(int code)
{
    switch (code) {
    case ERR_OK:                 return QStringLiteral("ok");
    case ERR_PARAM:              return QStringLiteral("参数错误，请检查输入");
    case ERR_NOT_LOGIN:          return QStringLiteral("尚未登录，请先登录");
    case ERR_TOKEN_INVALID:      return QStringLiteral("登录已过期，请重新登录");
    case ERR_NO_PERMISSION:      return QStringLiteral("无操作权限");
    case ERR_CMD_UNKNOWN:        return QStringLiteral("不支持的请求类型");
    case ERR_FRAME:              return QStringLiteral("报文格式错误");
    case ERR_INTERNAL:           return QStringLiteral("服务器内部错误，请稍后重试");
    case ERR_PHONE_FORMAT:       return QStringLiteral("请输入 11 位手机号");
    case ERR_USER_NOT_FOUND:     return QStringLiteral("用户不存在");
    case ERR_USER_FROZEN:        return QStringLiteral("该账号已被冻结，请联系客服");
    case ERR_BALANCE_NOT_ENOUGH: return QStringLiteral("钱包余额不足，请先充值");
    case ERR_AMOUNT_INVALID:     return QStringLiteral("金额不合法");
    case ERR_STATION_NOT_FOUND:  return QStringLiteral("充电站不存在");
    case ERR_PILE_NOT_FOUND:     return QStringLiteral("充电桩不存在");
    case ERR_PILE_BUSY:          return QStringLiteral("该充电桩正在使用中");
    case ERR_PILE_FAULT:         return QStringLiteral("该充电桩故障，请选择其他电桩");
    case ERR_PILE_OFFLINE:       return QStringLiteral("充电桩离线，指令无法下发");
    case ERR_ORDER_NOT_FOUND:    return QStringLiteral("订单不存在");
    case ERR_ORDER_UNFINISHED:   return QStringLiteral("您有未完成的充电订单，请先结算");
    case ERR_ORDER_STATUS:       return QStringLiteral("当前订单状态不允许该操作");
    case ERR_ORDER_SETTLED:      return QStringLiteral("订单已结算，请勿重复操作");
    case ERR_ADMIN_AUTH:         return QStringLiteral("账号或密码错误");
    default:                     return QStringLiteral("未知错误(%1)").arg(code);
    }
}

} // namespace ecp

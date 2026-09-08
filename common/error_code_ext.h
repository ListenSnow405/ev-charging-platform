#pragma once
// =============================================================================
//  common/error_code_ext.h  —  扩展模块错误码
//
//  ⚠ 本文件**不是**冻结契约。属主与规则同 common/protocol_ext.h。
//    每模块一个百号段，见 docs/expand/00 第 4.3 节。
//
//  ⚠ 重要 —— errMsgExt() 只用于**服务端日志**，不进响应报文。
//    common/protocol.h 的 buildResponse() 写死了 msg = errMsg(code)，
//    而 errMsg() 在冻结的 common/error_code.h 里，对 6000 段一律返回「未知错误(NNNN)」。
//    为三个错误码去动 L1 的冻结契约不划算，因此：
//      · 服务端：用 errMsgExt() 写日志，客户端拿到的 code 仍然准确
//      · 客户端：各扩展页面自带 code → 文案 映射表（见 admin-client/ext_08_carbon_page.cpp）
//    若日后希望服务端 msg 也返回中文，走 CR 给 errMsg() 加一条 fallback。
//    已登记为遗留项，见 docs/expand/08-实现规划.md 裁决 D5 与第 10 节。
// =============================================================================

#include <QString>

namespace ecp {

enum ErrCodeExt {
    // ---- 6700 段 · 08 碳减排与能源报告 ----
    ERR_CARBON_NO_FACTOR      = 6701,   // 该时段无有效排放因子
    ERR_CARBON_FACTOR_OVERLAP = 6702,   // 因子生效区间与既有因子重叠
    ERR_CARBON_REPORT_STALE   = 6703,   // 源数据已变，报告需重算（导出时可显式确认后继续）
    ERR_CARBON_REPORT_NOT_FOUND = 6704  // 报告不存在（段内自治新增，见 08 实现规划第 4 节）
};

// 扩展错误码 → 中文描述。仅供服务端日志与客户端本地映射参考，理由见文件头。
inline QString errMsgExt(int code)
{
    switch (code) {
    case ERR_CARBON_NO_FACTOR:      return QStringLiteral("该时间段没有生效的排放因子，请先配置");
    case ERR_CARBON_FACTOR_OVERLAP: return QStringLiteral("因子生效区间与已有因子重叠，请调整生效时间");
    case ERR_CARBON_REPORT_STALE:   return QStringLiteral("源数据已变更，该报告需重新生成");
    case ERR_CARBON_REPORT_NOT_FOUND: return QStringLiteral("报告不存在或已被删除");
    default:                        return QStringLiteral("未知扩展错误(%1)").arg(code);
    }
}

} // namespace ecp

#pragma once
// =============================================================================
//  common/error_code_ext.h  —  扩展模块错误码
//
//  ⚠ 本文件**不是**冻结契约。属主与规则同 common/protocol_ext.h。
//    每模块一个百号段，见 docs/expand/00 第 4.3 节。
//
//  ⚠ errMsgExt() 同时充当 error_code.h 的**扩展文案提供者**。
//    2026-09-08 L1 授权后，common/error_code.h 加了一个 ExtMsgProvider 挂钩：
//    冻结契约不 include 本文件（否则 L1 的契约会被各扩展模块的进度绑架），
//    改由扩展侧在进程启动时注册一次：
//        ecp::registerExtMsgProvider(&ecp::errMsgExt);   // server/main.cpp
//    于是 buildResponse() 的 msg 对 6000 段也能给出中文，客户端不必再靠本地映射兜底。
//    （裁决 D5 的遗留项就此了结，见 docs/expand/08-实现规划.md 第 10 节。）
//
//  ⚠ 因此 **未知码必须返回空串**，不能返回「未知扩展错误(NNNN)」——
//    那会盖住 errMsg() 自己的兜底文案，别人的错误码也会被本文件冒名顶替。
//    需要人话的日志请自行判空。
// =============================================================================

#include <QString>

namespace ecp {

enum ErrCodeExt {
    // ---- 6700 段 · 08 碳减排与能源报告 ----
    ERR_CARBON_NO_FACTOR      = 6701,   // 该时段无有效排放因子
    ERR_CARBON_FACTOR_OVERLAP = 6702,   // 因子生效区间与既有因子重叠
    ERR_CARBON_REPORT_STALE   = 6703,   // 源数据已变，报告需重算（导出时可显式确认后继续）
    ERR_CARBON_REPORT_NOT_FOUND = 6704, // 报告不存在（段内自治新增，见 08 实现规划第 4 节）
    ERR_CARBON_FACTOR_NOT_FOUND = 6705, // 排放因子不存在
    ERR_CARBON_LAST_FACTOR      = 6706, // 不能撤销最后一个启用的因子（撤了就没有因子可用）
    ERR_CARBON_VERSION_SHARED   = 6707  // 版本号被多行因子共用，彻底删除会误伤别人的历史行
};

// 扩展错误码 → 中文描述。注册给 errMsg() 用，也可直接用于服务端日志。
// **不认识的码返回空串**，理由见文件头。
inline QString errMsgExt(int code)
{
    switch (code) {
    case ERR_CARBON_NO_FACTOR:      return QStringLiteral("该时间段没有生效的排放因子，请先配置");
    case ERR_CARBON_FACTOR_OVERLAP: return QStringLiteral("因子生效区间与已有因子重叠，请调整生效时间");
    case ERR_CARBON_REPORT_STALE:   return QStringLiteral("源数据已变更，该报告需重新生成");
    case ERR_CARBON_REPORT_NOT_FOUND: return QStringLiteral("报告不存在或已被删除");
    case ERR_CARBON_FACTOR_NOT_FOUND: return QStringLiteral("排放因子不存在或已被撤销");
    case ERR_CARBON_LAST_FACTOR:      return QStringLiteral("这是最后一个启用的排放因子，撤销后将无法计算任何排放，已拒绝");
    case ERR_CARBON_VERSION_SHARED:   return QStringLiteral("该版本号被多个区域的因子共用，彻底删除会误删别人的历史数据，已拒绝");
    default:                        return QString();   // 交回 errMsg() 兜底，勿改
    }
}

} // namespace ecp

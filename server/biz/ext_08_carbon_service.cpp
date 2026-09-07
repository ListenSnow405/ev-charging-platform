// -----------------------------------------------------------------------------
//  server/biz/ext_08_carbon_service.cpp  —  扩展模块 08「碳减排与能源报告」
//  归属 L5（模块 08 认领人）。接缝规则见 docs/expand/00 第 5.2 节：
//  本模块一律新建独立文件，不改他人既有 service；server/main.cpp 只加一行注册。
//
//  任务规格 docs/expand/08-碳减排与能源报告.md
//  落地方案 docs/expand/08-实现规划.md（口径已冻结，改动先改该文档）
//
//  ⚠ 只读业务表，只写 t_carbon_*。
//    本文件不得出现对 t_order / t_user / t_wallet_tx / t_pile 的
//    INSERT / UPDATE / DELETE —— 08 文档第 4.2 节不变量 1：
//    任何聚合、预览、导出或重算都不得修改订单、钱包、设备遥测、支付和正式营收。
//
//  实施进度：S1 底座（本文件当前状态）→ S2 计算核心 → S3 聚合与查询
//            → S4 管理端与因子管理 → S5 报告导出与大屏
// -----------------------------------------------------------------------------
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include "protocol.h"
#include "protocol_ext.h"
#include "error_code_ext.h"
#include "logger.h"
#include "net/dispatcher.h"
#include "dao/db.h"

namespace ecp {

// -----------------------------------------------------------------------------
//  3741 CMD_EXT_FACTOR_LIST  排放因子版本列表
//
//  纯只读，无计算。S1 用它把整条链路先打通：扩展头文件、鉴权门、功能开关、
//  ext 建表脚本是否真的执行过 —— 这些出问题都会在这里先暴露，
//  而不是等到 S3 的聚合逻辑里混着算错的数字一起排查。
// -----------------------------------------------------------------------------
static int handleFactorList(const Request &req, QJsonObject &out)
{
    // 与 2301 一致：管理端命令一律校验角色（server/biz/statistics_service.cpp）
    if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;

    QSqlDatabase db = threadDb();          // 硬性规则 2：禁止跨线程共享连接
    if (!db.isOpen()) {
        LOG_E(QStringLiteral("排放因子列表获取数据库连接失败: %1").arg(db.lastError().text()));
        return ERR_INTERNAL;
    }

    QSqlQuery query(db);                   // 硬性规则：SQL 一律 prepare + bindValue
    query.prepare(QStringLiteral(
        "SELECT factor_id, region, version, effect_from, effect_to,"
        " factor_g_per_kwh, source, enabled"
        " FROM t_carbon_factor ORDER BY region, effect_from, version"));
    if (!query.exec()) {
        LOG_E(QStringLiteral("查询排放因子列表失败: %1").arg(query.lastError().text()));
        return ERR_INTERNAL;
    }

    QJsonArray list;
    while (query.next()) {
        QJsonObject item;
        item["factorId"]      = query.value(0).toInt();
        item["region"]        = query.value(1).toString();
        item["version"]       = query.value(2).toString();
        item["effectFrom"]    = query.value(3).toString();
        // effect_to 为 NULL 表示右开无穷（08 文档第 4.2 节不变量 5：区间左闭右开）
        item["effectTo"]      = query.value(4).isNull() ? QString() : query.value(4).toString();
        item["factorGPerKwh"] = query.value(5).toInt();
        item["source"]        = query.value(6).toString();
        item["enabled"]       = query.value(7).toInt();
        list.append(item);
    }
    out["list"] = list;
    return ERR_OK;
}

// -----------------------------------------------------------------------------
//  注册入口。server/main.cpp 的 registerAllServices() 只加一行调用它，
//  且外面包着 feat_08_carbon 功能开关判断（00 第 4.7 节）。
// -----------------------------------------------------------------------------
void registerExt08CarbonService()
{
    Dispatcher::instance().registerHandler(CMD_EXT_FACTOR_LIST, handleFactorList);

    LOG_I(QStringLiteral("扩展模块 08 碳减排与能源报告已注册: 3741 排放因子列表"
                         "（3740/3742–3746 待 S3–S5 落地）"));
}

} // namespace ecp

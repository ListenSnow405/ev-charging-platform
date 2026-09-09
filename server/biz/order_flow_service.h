#pragma once

#include <QtGlobal>

#include <QString>

namespace ecp {

// 由 L1 在有效 9002 上报处理完成后调用，向对应用户推送一次 1208。
void pushChargingProgress(const QString &pileCode, qint64 kwhX100);

} // namespace ecp

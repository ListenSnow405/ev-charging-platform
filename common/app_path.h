#pragma once
// =============================================================================
//  common/app_path.h  —  资源路径解析
//
//  冻结契约 · 属主 L1，其他人只读。
//  [本组自定] 解决「从 Qt Creator 运行时工作目录不是项目根」的问题。
//
//  各 .pro 的 DESTDIR 固定为 <项目根>/build/bin，因此可执行文件上溯两级即项目根。
//  终端在项目根运行、Qt Creator 在构建目录运行，两种方式都能找到 config/ 与数据库。
// =============================================================================

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

namespace ecp {

// <项目根>/build/bin/app  →  <项目根>
inline QString projectRoot()
{
    QDir d(QCoreApplication::applicationDirPath());
    d.cdUp();          // build/
    d.cdUp();          // 项目根
    return d.absolutePath();
}

// 解析项目内的相对路径：先按当前工作目录找，找不到再按项目根找。
inline QString resPath(const QString &relative)
{
    if (QFileInfo::exists(relative)) return relative;
    return projectRoot() + QLatin1Char('/') + relative;
}

} // namespace ecp

// user-client/webengine_env.cpp  —  QtWebEngine 运行期可用性探测　归属 L4
#include "webengine_env.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QString>
#include <QStringList>

#include "logger.h"

namespace {

bool isUsable(const QString &path)
{
    const QFileInfo fi(path);
    return fi.isFile() && fi.isExecutable();
}

bool probeWebEngineProcess()
{
    // 用户显式指定优先，但仍要校验，指错了同样会崩。
    const QByteArray preset = qgetenv("QTWEBENGINEPROCESS_PATH");
    if (!preset.isEmpty()) {
        if (isUsable(QFile::decodeName(preset))) return true;
        LOG_W(QStringLiteral("QTWEBENGINEPROCESS_PATH 指向的文件不可用：%1")
                  .arg(QFile::decodeName(preset)));
        return false;
    }

    const QString base = QLibraryInfo::path(QLibraryInfo::LibraryExecutablesPath);
    const QStringList candidates = {
        base + QStringLiteral("/QtWebEngineProcess"),
        base + QStringLiteral("/../../libexec/QtWebEngineProcess"),
        QStringLiteral("/usr/lib/qt6/libexec/QtWebEngineProcess"),
        QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/libexec/QtWebEngineProcess"),
        QStringLiteral("/usr/lib64/qt6/libexec/QtWebEngineProcess")
    };

    for (const QString &path : candidates) {
        if (!isUsable(path)) continue;
        qputenv("QTWEBENGINEPROCESS_PATH", QFile::encodeName(QFileInfo(path).absoluteFilePath()));
        LOG_I(QStringLiteral("QtWebEngineProcess: %1").arg(QFileInfo(path).absoluteFilePath()));
        return true;
    }

    LOG_W(QStringLiteral("未找到 QtWebEngineProcess，导航页退回占位页。"
                         "安装：sudo apt install libqt6webenginecore6-bin"));
    return false;
}

} // namespace

namespace ecp {

bool webEngineAvailable()
{
    static const bool available = probeWebEngineProcess();
    return available;
}

} // namespace ecp

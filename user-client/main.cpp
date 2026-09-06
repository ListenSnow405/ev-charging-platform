// user-client/main.cpp  —  充电用户端入口　归属 L4
#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QLibraryInfo>
#include "login_window.h"

static void configureWebEngineProcessPath()
{
    if (!qEnvironmentVariableIsEmpty("QTWEBENGINEPROCESS_PATH")) return;

    const QString base = QLibraryInfo::path(QLibraryInfo::LibraryExecutablesPath);
    const QString candidates[] = {
        base + QStringLiteral("/QtWebEngineProcess"),
        base + QStringLiteral("/../../libexec/QtWebEngineProcess"),
        QStringLiteral("/usr/lib/qt6/libexec/QtWebEngineProcess"),
        QStringLiteral("/usr/lib/x86_64-linux-gnu/qt6/libexec/QtWebEngineProcess"),
        QStringLiteral("/usr/lib64/qt6/libexec/QtWebEngineProcess")
    };

    for (const QString &path : candidates) {
        if (QFileInfo::exists(path)) {
            qputenv("QTWEBENGINEPROCESS_PATH", QFile::encodeName(QFileInfo(path).absoluteFilePath()));
            break;
        }
    }
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ecp-user"));
    configureWebEngineProcessPath();
    LoginWindow w;
    w.show();
    return app.exec();
}

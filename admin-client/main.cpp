// admin-client/main.cpp  —  PC 服务器端（管理后台）入口　归属 L3
#include <QApplication>
#include <QFile>
#include "logger.h"
#include "login_window.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ecp-admin"));
    QFile theme(QStringLiteral(":/admin/admin_theme.qss"));
    if (theme.open(QIODevice::ReadOnly))
        app.setStyleSheet(QString::fromUtf8(theme.readAll()));
    else
        LOG_W(QStringLiteral("管理端主题加载失败，使用默认样式：%1").arg(theme.errorString()));
    LoginWindow w;
    w.show();
    return app.exec();
}

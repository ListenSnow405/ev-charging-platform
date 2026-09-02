// admin-client/main.cpp  —  PC 服务器端（管理后台）入口　归属 L3
#include <QApplication>
#include "login_window.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ecp-admin"));
    LoginWindow w;
    w.show();
    return app.exec();
}

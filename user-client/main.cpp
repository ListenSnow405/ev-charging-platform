// user-client/main.cpp  —  充电用户端入口　归属 L4
#include <QApplication>
#include "login_window.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ecp-user"));
    LoginWindow w;
    w.show();
    return app.exec();
}

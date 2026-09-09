// user-client/main.cpp  —  充电用户端入口　归属 L4
#include <QApplication>
#include "login_window.h"
#include "webengine_env.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ecp-user"));
    // 启动时先探测一次：结果被缓存，导航页据此决定用 WebEngine 还是占位页。
    ecp::webEngineAvailable();
    LoginWindow w;
    w.show();
    return app.exec();
}

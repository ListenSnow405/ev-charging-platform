#include "main_window.h"
#include <QVBoxLayout>
#include <QFont>

#ifdef HAVE_WEBENGINE
#include <QWebEngineView>
#endif

MainWindow::MainWindow(NetClient *net, QWidget *parent) : QWidget(parent), m_net(net)
{
    setWindowTitle(QStringLiteral("充电用户端"));
    resize(430, 780);                      // [说明书] 1.4 模拟手机端比例

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(makePlaceholder(QStringLiteral("附近充电站"),
        QStringLiteral("TODO(L4)：区域/地址定位 → 腾讯地图地理编码 → 按距离升序卡片\n"
                       "卡片含：站名 / 价格(元每度) / 电桩总数与空闲数 / 距离\n对应命令字 1101、1102")),
        QStringLiteral("找桩"));
    m_tabs->addTab(makeNavPage(), QStringLiteral("导航"));
    m_tabs->addTab(makePlaceholder(QStringLiteral("充电"),
        QStringLiteral("TODO(L4)：进入前必须先查未结算订单（命令字 1201），\n"
                       "有则弹窗提示并强制跳转结算页 —— [说明书] 1.4 明确要求\n"
                       "流程：预约 1202 → 开始 1203 → 计费 1204 → 结算 1205")),
        QStringLiteral("充电"));
    m_tabs->addTab(makePlaceholder(QStringLiteral("我的"),
        QStringLiteral("TODO(L4)：头像 / 昵称 / 钱包余额；修改昵称 1003、换头像 1004、充值 1005")),
        QStringLiteral("我的"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_tabs);
}

QWidget *MainWindow::makePlaceholder(const QString &title, const QString &todo)
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    auto *t = new QLabel(title, w);
    QFont f = t->font(); f.setPointSize(16); f.setBold(true); t->setFont(f);
    auto *d = new QLabel(todo, w);
    d->setWordWrap(true);
    d->setStyleSheet(QStringLiteral("color:#888"));
    lay->addWidget(t); lay->addWidget(d); lay->addStretch();
    return w;
}

QWidget *MainWindow::makeNavPage()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
#ifdef HAVE_WEBENGINE
    // [说明书] 1.4 调用腾讯地图 Web API（QWebEngineView 加载），驾车/步行路线规划
    auto *view = new QWebEngineView(w);
    view->setHtml(QStringLiteral(
        "<html><body style='font-family:sans-serif;padding:24px'>"
        "<h3>QWebEngineView 就绪</h3>"
        "<p>TODO(L4)：加载腾讯地图路线规划页面。<br>"
        "Key 从 config/app.ini 的 [map] key 读取，禁止硬编码。</p></body></html>"));
    lay->addWidget(view, 1);
#else
    auto *warn = new QLabel(QStringLiteral(
        "⚠ QtWebEngineWidgets 未安装，一键导航不可用。\n"
        "请执行：sudo apt install qt6-webengine-dev qt6-webengine-dev-tools\n"
        "然后重新 qmake6 && make（详见 docs/conventions.md 第 5 节）"), w);
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("color:#a33;border:1px dashed #a33;padding:16px"));
    lay->addWidget(warn);
    lay->addStretch();
#endif
    return w;
}

#include "main_window.h"
#include "order_page.h"
#include "overview_page.h"
#include "pile_page.h"
#include "station_page.h"
#include "user_page.h"
#include "error_code.h"
#include <QHBoxLayout>

MainWindow::MainWindow(NetClient *net, QWidget *parent) : QWidget(parent), m_net(net)
{
    setWindowTitle(QStringLiteral("充电桩运营管理后台"));
    resize(1200, 760);

    m_nav = new QListWidget(this);
    m_nav->setFixedWidth(180);
    // [说明书] 1.4 管理端六大功能
    m_nav->addItems({ QStringLiteral("数据总览"), QStringLiteral("电站管理"),
                      QStringLiteral("电桩管理"), QStringLiteral("订单管理"),
                      QStringLiteral("用户管理") });

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(new OverviewPage(m_net, m_pages));
    m_pages->addWidget(new StationPage(m_net, m_pages));
    m_pages->addWidget(new PilePage(m_net, m_pages));
    m_pages->addWidget(new OrderPage(m_net, m_pages));
    m_pages->addWidget(new UserPage(m_net, m_pages));

    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    connect(m_net, &NetClient::response, this,
            [this](int cmd, int, int code, const QString &msg, const QJsonObject &) {
        if (cmd < 2000 || cmd > 2399) return;
        if (code != ecp::ERR_NOT_LOGIN && code != ecp::ERR_TOKEN_INVALID) return;
        requestRelogin(msg.trimmed().isEmpty()
            ? QStringLiteral("登录已过期，请重新登录")
            : msg);
    });
    connect(m_net, &NetClient::disconnected, this, [this] {
        requestRelogin(QStringLiteral("与服务器的连接已断开，请重新登录"));
    });

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_nav);
    lay->addWidget(m_pages, 1);
}

void MainWindow::requestRelogin(const QString &reason)
{
    if (m_reloginPending) return;
    m_reloginPending = true;
    emit reloginRequested(reason);
    close();
}

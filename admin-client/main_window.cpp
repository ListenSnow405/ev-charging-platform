#include "main_window.h"
#include "forecast_page.h"
#include "order_page.h"
#include "overview_page.h"
#include "pile_page.h"
#include "station_page.h"
#include "user_page.h"
#include "ext_08_carbon_page.h"
#include "error_code.h"
#include <QHBoxLayout>

MainWindow::MainWindow(NetClient *net, QWidget *parent) : QWidget(parent), m_net(net)
{
    setWindowTitle(QStringLiteral("充电桩运营管理后台"));
    resize(1200, 760);

    m_nav = new QListWidget(this);
    m_nav->setFixedWidth(180);
    m_pages = new QStackedWidget(this);

    // 导航项与页面在同一张表里成对登记。
    // 侧栏切换靠的是 currentRowChanged -> setCurrentIndex 的下标对应，两处分开写时，
    // 谁在中间插一项而忘了同步另一处，整个侧栏就会错位一格 —— 而且编译不报错。
    // 成对登记后这种漂移在语法上就不可能发生，加模块也只需动一行。
    const struct { QString title; QWidget *page; } navEntries[] = {
        // [说明书] 1.4 管理端六大功能
        { QStringLiteral("数据总览"),   new OverviewPage(m_net, m_pages) },
        { QStringLiteral("电站管理"),   new StationPage(m_net, m_pages)  },
        { QStringLiteral("电桩管理"),   new PilePage(m_net, m_pages)     },
        { QStringLiteral("订单管理"),   new OrderPage(m_net, m_pages)    },
        { QStringLiteral("用户管理"),   new UserPage(m_net, m_pages)     },
        { QStringLiteral("负荷预测"),   new ForecastPage(m_net, m_pages) },
        // ---- 以下为扩展模块（加分项），服务端未开启对应开关时页面会提示未启用 ----
        { QStringLiteral("碳排放报告"), new Ext08CarbonPage(m_net, m_pages) },   // 08（L5）
    };
    for (const auto &entry : navEntries) {
        m_nav->addItem(entry.title);
        m_pages->addWidget(entry.page);
    }

    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    connect(m_net, &NetClient::response, this,
            [this](int cmd, int, int code, const QString &msg, const QJsonObject &) {
        // 管理端命令段：核心 2000–2399，扩展 3000–3999（docs/expand/00 第 4.3 节）。
        // 漏掉扩展段的话，在扩展页面上遇到 token 过期不会触发重登，只会静默报错。
        if ((cmd < 2000 || cmd > 2399) && (cmd < 3000 || cmd > 3999)) return;
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

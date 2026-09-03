#include "main_window.h"
#include "order_page.h"
#include "overview_page.h"
#include "pile_page.h"
#include "station_page.h"
#include "user_page.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFont>

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
    m_pages->addWidget(new OverviewPage(m_pages));
    m_pages->addWidget(new StationPage(m_net, m_pages));
    m_pages->addWidget(new PilePage(m_net, m_pages));
    m_pages->addWidget(new OrderPage(m_net, m_pages));
    m_pages->addWidget(new UserPage(m_net, m_pages));

    connect(m_nav, &QListWidget::currentRowChanged, m_pages, &QStackedWidget::setCurrentIndex);
    m_nav->setCurrentRow(0);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_nav);
    lay->addWidget(m_pages, 1);
}

QWidget *MainWindow::makePlaceholder(const QString &title, const QString &todo)
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    auto *t = new QLabel(title, w);
    QFont f = t->font(); f.setPointSize(18); f.setBold(true); t->setFont(f);
    auto *d = new QLabel(todo, w);
    d->setStyleSheet(QStringLiteral("color:#888"));
    lay->addWidget(t); lay->addWidget(d); lay->addStretch();
    return w;
}

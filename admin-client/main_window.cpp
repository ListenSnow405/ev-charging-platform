#include "main_window.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFont>

#ifdef HAVE_CHARTS
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#endif

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
    m_pages->addWidget(makeOverviewPage());
    m_pages->addWidget(makePlaceholder(QStringLiteral("电站管理"),
        QStringLiteral("TODO(L3)：电站列表 / 站内电桩详情 / 新增电站\n对应命令字 2101 / 2103 / 2102")));
    m_pages->addWidget(makePlaceholder(QStringLiteral("电桩管理"),
        QStringLiteral("TODO(L3)：电桩列表 + 远程重启\n对应命令字 2111 / 2112")));
    m_pages->addWidget(makePlaceholder(QStringLiteral("订单管理"),
        QStringLiteral("TODO(L3)：订单列表与筛选\n对应命令字 2304")));
    m_pages->addWidget(makePlaceholder(QStringLiteral("用户管理"),
        QStringLiteral("TODO(L3)：用户列表 / 手机号模糊搜索 / 冻结解冻\n对应命令字 2201 / 2202")));

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

QWidget *MainWindow::makeOverviewPage()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);

    auto *t = new QLabel(QStringLiteral("数据总览"), w);
    QFont f = t->font(); f.setPointSize(18); f.setBold(true); t->setFont(f);
    lay->addWidget(t);
    lay->addWidget(new QLabel(QStringLiteral(
        "TODO(L3)：今日/本月/累计营收三指标（命令字 2301）、"
        "近 7/30 日趋势（2302）、电桩状态分布（2303）"), w));

#ifdef HAVE_CHARTS
    // [说明书] 1.4 营收趋势折线图，使用 QT 的 QChart 组件
    auto *series = new QLineSeries;
    series->setName(QStringLiteral("营收(元)"));
    const double demo[7] = { 138, 126, 131, 155, 182, 96, 0 };   // 占位数据，待接 2302
    for (int i = 0; i < 7; ++i) series->append(i, demo[i]);
    auto *chart = new QChart;
    chart->addSeries(series);
    chart->createDefaultAxes();
    chart->setTitle(QStringLiteral("近 7 日营收趋势（占位数据）"));
    auto *view = new QChartView(chart, w);
    view->setMinimumHeight(300);
    lay->addWidget(view, 1);
#else
    auto *warn = new QLabel(QStringLiteral(
        "⚠ QtCharts 未安装，营收趋势图不可用。\n"
        "请执行：sudo apt install libqt6charts6-dev\n"
        "然后重新 qmake6 && make（详见 docs/conventions.md 第 5 节）"), w);
    warn->setStyleSheet(QStringLiteral("color:#a33;border:1px dashed #a33;padding:16px"));
    lay->addWidget(warn);
#endif
    lay->addStretch();
    return w;
}

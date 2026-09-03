#include "overview_page.h"
#include <QVBoxLayout>
#include <QLabel>
#include <QFont>

#ifdef HAVE_CHARTS
#include <QChart>
#include <QChartView>
#include <QLineSeries>
#endif

OverviewPage::OverviewPage(QWidget *parent) : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);

    auto *t = new QLabel(QStringLiteral("数据总览"), this);
    QFont f = t->font(); f.setPointSize(18); f.setBold(true); t->setFont(f);
    lay->addWidget(t);
    lay->addWidget(new QLabel(QStringLiteral(
        "TODO(L3)：今日/本月/累计营收三指标（命令字 2301）、"
        "近 7/30 日趋势（2302）、电桩状态分布（2303）"), this));

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
    auto *view = new QChartView(chart, this);
    view->setMinimumHeight(300);
    lay->addWidget(view, 1);
#else
    auto *warn = new QLabel(QStringLiteral(
        "⚠ QtCharts 未安装，营收趋势图不可用。\n"
        "请执行：sudo apt install libqt6charts6-dev\n"
        "然后重新 qmake6 && make（详见 docs/conventions.md 第 5 节）"), this);
    warn->setStyleSheet(QStringLiteral("color:#a33;border:1px dashed #a33;padding:16px"));
    lay->addWidget(warn);
#endif
    lay->addStretch();
}

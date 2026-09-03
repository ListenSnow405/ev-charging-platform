#include "overview_page.h"
#include "time_util.h"
#include <QDate>
#include <QDateTime>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QFont>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVector>

#ifdef HAVE_CHARTS
#include <QChart>
#include <QChartView>
#include <QDateTimeAxis>
#include <QLineSeries>
#include <QValueAxis>
#include <QPainter>
#endif

namespace {

#ifdef HAVE_CHARTS
// Mock 数据集中于此，待 2302 接入后替换为服务端响应。
QVector<qint64> mockRevenueTrend(int days)
{
    static const QVector<qint64> sevenDays = {
        13820, 12650, 13180, 15540, 18260, 16930, 19480
    };
    static const QVector<qint64> thirtyDays = {
        11240, 12680, 11950, 13820, 14560, 13240, 15180, 14720, 15960, 16840,
        15430, 17120, 18350, 17680, 19240, 18860, 20120, 19650, 21480, 20830,
        22160, 21740, 23420, 22680, 24150, 23820, 25240, 24760, 26380, 27120
    };
    return days == 30 ? thirtyDays : sevenDays;
}
#endif

QString percentageText(int count, int total)
{
    if (total <= 0) return QStringLiteral("0.0%");
    const int tenths = (count * 1000 + total / 2) / total;
    return QStringLiteral("%1.%2%").arg(tenths / 10).arg(tenths % 10);
}

} // namespace

OverviewPage::OverviewPage(QWidget *parent) : QWidget(parent)
{
    setupUi();
    loadMockData();
}

void OverviewPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("数据总览"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    pageLayout->addWidget(title);

    auto *metricsLayout = new QHBoxLayout;
    metricsLayout->setSpacing(16);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("今日营收"), m_todayRevenue), 1);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("本月营收"), m_monthRevenue), 1);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("累计营收"), m_totalRevenue), 1);
    pageLayout->addLayout(metricsLayout);

    auto *contentLayout = new QHBoxLayout;
    contentLayout->setSpacing(16);

    auto *trendGroup = new QGroupBox(QStringLiteral("营收趋势"), this);
    auto *trendLayout = new QVBoxLayout(trendGroup);
    auto *trendToolbar = new QHBoxLayout;
    trendToolbar->addStretch();
    auto *sevenDaysButton = new QPushButton(QStringLiteral("近7日"), trendGroup);
    auto *thirtyDaysButton = new QPushButton(QStringLiteral("近30日"), trendGroup);
    sevenDaysButton->setCheckable(true);
    thirtyDaysButton->setCheckable(true);
    sevenDaysButton->setAutoExclusive(true);
    thirtyDaysButton->setAutoExclusive(true);
    sevenDaysButton->setChecked(true);
    trendToolbar->addWidget(sevenDaysButton);
    trendToolbar->addWidget(thirtyDaysButton);
    trendLayout->addLayout(trendToolbar);

#ifdef HAVE_CHARTS
    // [说明书] 1.4 营收趋势折线图，使用 Qt 6 的 QChart 组件。
    m_revenueSeries = new QLineSeries;
    m_revenueSeries->setName(QStringLiteral("营收（元）"));
    m_revenueChart = new QChart;
    m_revenueChart->addSeries(m_revenueSeries);
    m_revenueChart->legend()->setVisible(false);

    m_revenueAxisX = new QDateTimeAxis;
    m_revenueAxisX->setFormat(QStringLiteral("MM-dd"));
    m_revenueAxisX->setTitleText(QStringLiteral("日期"));
    m_revenueAxisY = new QValueAxis;
    m_revenueAxisY->setLabelFormat(QStringLiteral("%.2f"));
    m_revenueAxisY->setTitleText(QStringLiteral("营收（元）"));
    m_revenueAxisY->setMin(0);
    m_revenueChart->addAxis(m_revenueAxisX, Qt::AlignBottom);
    m_revenueChart->addAxis(m_revenueAxisY, Qt::AlignLeft);
    m_revenueSeries->attachAxis(m_revenueAxisX);
    m_revenueSeries->attachAxis(m_revenueAxisY);

    auto *chartView = new QChartView(m_revenueChart, trendGroup);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setMinimumHeight(360);
    trendLayout->addWidget(chartView, 1);

    connect(sevenDaysButton, &QPushButton::clicked, this, [this] {
        updateRevenueTrend(7);
    });
    connect(thirtyDaysButton, &QPushButton::clicked, this, [this] {
        updateRevenueTrend(30);
    });
#else
    auto *warn = new QLabel(QStringLiteral(
        "⚠ QtCharts 未安装，营收趋势图不可用。\n"
        "请执行：sudo apt install libqt6charts6-dev\n"
        "然后重新 qmake6 && make（详见 docs/conventions.md 第 5 节）"), trendGroup);
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("color:#a33;border:1px dashed #a33;padding:16px"));
    trendLayout->addWidget(warn);
    trendLayout->addStretch();
    sevenDaysButton->setEnabled(false);
    thirtyDaysButton->setEnabled(false);
#endif

    auto *statusGroup = new QGroupBox(QStringLiteral("电桩状态"), this);
    auto *statusLayout = new QVBoxLayout(statusGroup);
    m_pileTotal = new QLabel(statusGroup);
    m_pileTotal->setStyleSheet(QStringLiteral("color:#666"));
    statusLayout->addWidget(m_pileTotal);

    m_pileStatusTable = new QTableWidget(3, 3, statusGroup);
    m_pileStatusTable->setHorizontalHeaderLabels({
        QStringLiteral("状态"), QStringLiteral("数量"), QStringLiteral("占比")
    });
    m_pileStatusTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_pileStatusTable->verticalHeader()->setVisible(false);
    m_pileStatusTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pileStatusTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_pileStatusTable->setAlternatingRowColors(true);
    statusLayout->addWidget(m_pileStatusTable);

    contentLayout->addWidget(trendGroup, 3);
    contentLayout->addWidget(statusGroup, 1);
    pageLayout->addLayout(contentLayout, 1);
}

QWidget *OverviewPage::createMetricCard(const QString &title, QLabel *&valueLabel)
{
    auto *card = new QFrame(this);
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet(QStringLiteral(
        "QFrame { background:#f7f9fc; border:1px solid #dfe5ec; border-radius:6px; }"
        "QLabel { border:none; }"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 14);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setStyleSheet(QStringLiteral("color:#667085"));
    valueLabel = new QLabel(card);
    QFont valueFont = valueLabel->font();
    valueFont.setPointSize(18);
    valueFont.setBold(true);
    valueLabel->setFont(valueFont);

    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    return card;
}

void OverviewPage::loadMockData()
{
    // Mock 数据集中于此，待 2301/2303 接入后替换为服务端响应。
    const qint64 todayRevenueFen = 19480;
    const qint64 monthRevenueFen = 486320;
    const qint64 totalRevenueFen = 5287460;
    m_todayRevenue->setText(QStringLiteral("¥ %1").arg(ecp::fenToYuan(todayRevenueFen)));
    m_monthRevenue->setText(QStringLiteral("¥ %1").arg(ecp::fenToYuan(monthRevenueFen)));
    m_totalRevenue->setText(QStringLiteral("¥ %1").arg(ecp::fenToYuan(totalRevenueFen)));

    updatePileStatusTable(12, 31, 5);

#ifdef HAVE_CHARTS
    updateRevenueTrend(7);
#endif
}

void OverviewPage::updatePileStatusTable(int inUse, int idle, int fault)
{
    const int total = inUse + idle + fault;
    m_pileTotal->setText(QStringLiteral("电桩总数：%1 台").arg(total));

    const QStringList statuses = {
        QStringLiteral("在用"), QStringLiteral("闲置"), QStringLiteral("故障")
    };
    const int counts[] = { inUse, idle, fault };
    for (int row = 0; row < 3; ++row) {
        auto *statusItem = new QTableWidgetItem(statuses.at(row));
        auto *countItem = new QTableWidgetItem(QString::number(counts[row]));
        auto *percentageItem = new QTableWidgetItem(percentageText(counts[row], total));
        statusItem->setTextAlignment(Qt::AlignCenter);
        countItem->setTextAlignment(Qt::AlignCenter);
        percentageItem->setTextAlignment(Qt::AlignCenter);
        m_pileStatusTable->setItem(row, 0, statusItem);
        m_pileStatusTable->setItem(row, 1, countItem);
        m_pileStatusTable->setItem(row, 2, percentageItem);
    }
}

#ifdef HAVE_CHARTS
void OverviewPage::updateRevenueTrend(int days)
{
    const QVector<qint64> revenueFen = mockRevenueTrend(days);
    const QDate firstDate = QDate::currentDate().addDays(1 - revenueFen.size());
    qint64 maxRevenueFen = 0;

    m_revenueSeries->clear();
    for (qsizetype i = 0; i < revenueFen.size(); ++i) {
        const qint64 amountFen = revenueFen.at(i);
        const QDateTime pointTime(firstDate.addDays(i), QTime(0, 0));
        // QLineSeries 需要数值坐标；金额仍以分保存，仅在展示层经统一函数转换为元。
        m_revenueSeries->append(pointTime.toMSecsSinceEpoch(),
                                ecp::fenToYuan(amountFen).toDouble());
        if (amountFen > maxRevenueFen) maxRevenueFen = amountFen;
    }

    const QDateTime firstTime(firstDate, QTime(0, 0));
    const QDateTime lastTime(QDate::currentDate(), QTime(23, 59, 59));
    m_revenueAxisX->setRange(firstTime, lastTime);
    m_revenueAxisX->setTickCount(days == 7 ? 7 : 6);
    m_revenueAxisY->setMax(ecp::fenToYuan(maxRevenueFen + maxRevenueFen / 10).toDouble());
    m_revenueChart->setTitle(days == 30
        ? QStringLiteral("近 30 日营收趋势")
        : QStringLiteral("近 7 日营收趋势"));
}
#endif

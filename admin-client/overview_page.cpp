#include "overview_page.h"
#include "net_client.h"
#include "protocol.h"
#include "time_util.h"
#include <QAbstractItemView>
#include <QDateTime>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QVBoxLayout>
#include <algorithm>

#ifdef HAVE_CHARTS
#include <QChart>
#include <QChartView>
#include <QDateTimeAxis>
#include <QLineSeries>
#include <QPainter>
#include <QValueAxis>
#endif

namespace {

QString percentageText(qint64 count, qint64 total)
{
    if (total <= 0) return QStringLiteral("0.0%");
    const qint64 tenths = (count * 1000 + total / 2) / total;
    return QStringLiteral("%1.%2%").arg(tenths / 10).arg(tenths % 10);
}

} // namespace

OverviewPage::OverviewPage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();
    connect(m_net, &NetClient::response, this, &OverviewPage::handleResponse);
    requestRevenue();
    requestPileStatus();
#ifdef HAVE_CHARTS
    requestRevenueTrend(7);
#endif
}

void OverviewPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("数据总览"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    pageLayout->addWidget(title);

    m_statusLabel = new QLabel(QStringLiteral("准备加载数据总览"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#667085"));
    m_statusLabel->setWordWrap(true);
    pageLayout->addWidget(m_statusLabel);

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
    m_revenueAxisY->setRange(0, 1);
    m_revenueChart->addAxis(m_revenueAxisX, Qt::AlignBottom);
    m_revenueChart->addAxis(m_revenueAxisY, Qt::AlignLeft);
    m_revenueSeries->attachAxis(m_revenueAxisX);
    m_revenueSeries->attachAxis(m_revenueAxisY);

    auto *chartView = new QChartView(m_revenueChart, trendGroup);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setMinimumHeight(360);
    trendLayout->addWidget(chartView, 1);

    connect(sevenDaysButton, &QPushButton::clicked, this, [this] {
        requestRevenueTrend(7);
    });
    connect(thirtyDaysButton, &QPushButton::clicked, this, [this] {
        requestRevenueTrend(30);
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
    m_pileTotal = new QLabel(QStringLiteral("电桩总数：—"), statusGroup);
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

void OverviewPage::requestRevenue()
{
    m_revenueState = LoadState::Loading;
    m_revenueError.clear();
    updateStatusLabel();
    const int seq = m_net->send(ecp::CMD_STAT_REVENUE);
    if (seq < 0) {
        m_revenueSeq = -1;
        m_revenueState = LoadState::Failed;
        m_revenueError = QStringLiteral("请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_revenueSeq = seq;
}

void OverviewPage::requestRevenueTrend(int days)
{
#ifdef HAVE_CHARTS
    m_revenueTrendState = LoadState::Loading;
    m_revenueTrendError.clear();
    updateStatusLabel();
    const int seq = m_net->send(ecp::CMD_STAT_REVENUE_TREND, QJsonObject{
        { QStringLiteral("days"), days }
    });
    if (seq < 0) {
        m_revenueTrendSeq = -1;
        m_pendingTrendDays = days;
        m_revenueTrendState = LoadState::Failed;
        m_revenueTrendError = QStringLiteral("请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_revenueTrendSeq = seq;
    m_pendingTrendDays = days;
#else
    Q_UNUSED(days);
#endif
}

void OverviewPage::requestPileStatus()
{
    m_pileStatusState = LoadState::Loading;
    m_pileStatusError.clear();
    updateStatusLabel();
    const int seq = m_net->send(ecp::CMD_STAT_PILE_STATUS);
    if (seq < 0) {
        m_pileStatusSeq = -1;
        m_pileStatusState = LoadState::Failed;
        m_pileStatusError = QStringLiteral("请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_pileStatusSeq = seq;
}

void OverviewPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                                  const QJsonObject &data)
{
    if (cmd == ecp::CMD_STAT_REVENUE) {
        if (seq != m_revenueSeq) return;
        m_revenueSeq = -1;
        handleRevenueResponse(code, msg, data);
        return;
    }
#ifdef HAVE_CHARTS
    if (cmd == ecp::CMD_STAT_REVENUE_TREND) {
        if (seq != m_revenueTrendSeq) return;
        m_revenueTrendSeq = -1;
        handleRevenueTrendResponse(code, msg, data);
        return;
    }
#endif
    if (cmd == ecp::CMD_STAT_PILE_STATUS) {
        if (seq != m_pileStatusSeq) return;
        m_pileStatusSeq = -1;
        handlePileStatusResponse(code, msg, data);
    }
}

void OverviewPage::handleRevenueResponse(int code, const QString &msg,
                                         const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_revenueState = LoadState::Failed;
        m_revenueError = msg;
        updateStatusLabel();
        return;
    }

    const qint64 todayRevenueFen = data.value(QStringLiteral("today")).toInteger();
    const qint64 monthRevenueFen = data.value(QStringLiteral("month")).toInteger();
    const qint64 totalRevenueFen = data.value(QStringLiteral("total")).toInteger();
    m_todayRevenue->setText(QStringLiteral("¥ %1").arg(ecp::fenToYuan(todayRevenueFen)));
    m_monthRevenue->setText(QStringLiteral("¥ %1").arg(ecp::fenToYuan(monthRevenueFen)));
    m_totalRevenue->setText(QStringLiteral("¥ %1").arg(ecp::fenToYuan(totalRevenueFen)));
    m_revenueState = LoadState::Success;
    m_revenueError.clear();
    updateStatusLabel();
}

void OverviewPage::handlePileStatusResponse(int code, const QString &msg,
                                            const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_pileStatusState = LoadState::Failed;
        m_pileStatusError = msg;
        updateStatusLabel();
        return;
    }

    const qint64 inUse = data.value(QStringLiteral("inUse")).toInteger();
    const qint64 idle = data.value(QStringLiteral("idle")).toInteger();
    const qint64 fault = data.value(QStringLiteral("fault")).toInteger();
    const qint64 total = data.value(QStringLiteral("total")).toInteger();
    if (inUse < 0 || idle < 0 || fault < 0 || total < 0
        || inUse + idle + fault != total) {
        m_pileStatusState = LoadState::Failed;
        m_pileStatusError = QStringLiteral("服务端返回的分类数量与总数不一致");
        updateStatusLabel();
        return;
    }

    updatePileStatusTable(inUse, idle, fault, total);
    m_pileStatusState = LoadState::Success;
    m_pileStatusError.clear();
    updateStatusLabel();
}

void OverviewPage::updateStatusLabel()
{
    QStringList errors;
    if (m_revenueState == LoadState::Failed)
        errors.append(QStringLiteral("营收概览加载失败：%1").arg(m_revenueError));
#ifdef HAVE_CHARTS
    if (m_revenueTrendState == LoadState::Failed)
        errors.append(QStringLiteral("营收趋势加载失败：%1").arg(m_revenueTrendError));
#endif
    if (m_pileStatusState == LoadState::Failed)
        errors.append(QStringLiteral("电桩状态加载失败：%1").arg(m_pileStatusError));

    const bool loading = m_revenueState == LoadState::Loading
        || m_pileStatusState == LoadState::Loading
#ifdef HAVE_CHARTS
        || m_revenueTrendState == LoadState::Loading
#endif
        ;
    if (loading) errors.prepend(QStringLiteral("正在加载数据总览…"));

    if (!errors.isEmpty()) {
        m_statusLabel->setText(errors.join(QStringLiteral("　")));
        return;
    }

    const bool revenueReady = m_revenueState == LoadState::Success;
    const bool pileStatusReady = m_pileStatusState == LoadState::Success;
#ifdef HAVE_CHARTS
    const bool trendReady = m_revenueTrendState == LoadState::Success;
#else
    const bool trendReady = true;
#endif
    m_statusLabel->setText(revenueReady && pileStatusReady && trendReady
        ? QStringLiteral("数据总览已更新")
        : QStringLiteral("准备加载数据总览"));
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
    valueLabel = new QLabel(QStringLiteral("—"), card);
    QFont valueFont = valueLabel->font();
    valueFont.setPointSize(18);
    valueFont.setBold(true);
    valueLabel->setFont(valueFont);

    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    return card;
}

void OverviewPage::updatePileStatusTable(qint64 inUse, qint64 idle, qint64 fault,
                                         qint64 total)
{
    m_pileTotal->setText(QStringLiteral("电桩总数：%1 台").arg(total));

    const QStringList statuses = {
        QStringLiteral("在用"), QStringLiteral("闲置"), QStringLiteral("故障")
    };
    const qint64 counts[] = { inUse, idle, fault };
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
void OverviewPage::handleRevenueTrendResponse(int code, const QString &msg,
                                              const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_revenueTrendState = LoadState::Failed;
        m_revenueTrendError = msg;
        updateStatusLabel();
        return;
    }

    QVector<RevenueTrendPoint> points;
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
    points.reserve(list.size());
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        const QDate date = QDate::fromString(
            item.value(QStringLiteral("date")).toString(), QStringLiteral("yyyy-MM-dd"));
        if (!date.isValid()) continue;
        points.append({date, item.value(QStringLiteral("amount")).toInteger()});
    }
    std::sort(points.begin(), points.end(), [](const RevenueTrendPoint &left,
                                               const RevenueTrendPoint &right) {
        return left.date < right.date;
    });

    renderRevenueTrend(points, m_pendingTrendDays);
    if (points.isEmpty()) {
        m_revenueTrendState = LoadState::Failed;
        m_revenueTrendError = QStringLiteral("响应中没有有效日期数据");
    } else {
        m_revenueTrendState = LoadState::Success;
        m_revenueTrendError.clear();
    }
    updateStatusLabel();
}

void OverviewPage::renderRevenueTrend(const QVector<RevenueTrendPoint> &points, int days)
{
    m_revenueSeries->clear();
    m_revenueChart->setTitle(days == 30
        ? QStringLiteral("近 30 日营收趋势")
        : QStringLiteral("近 7 日营收趋势"));
    if (points.isEmpty()) return;

    qint64 maxRevenueFen = 0;
    for (const RevenueTrendPoint &point : points) {
        const QDateTime pointTime(point.date, QTime(0, 0));
        // QLineSeries 只在展示层使用元坐标，业务缓存始终保留 qint64 分。
        m_revenueSeries->append(pointTime.toMSecsSinceEpoch(),
                                ecp::fenToYuan(point.amountFen).toDouble());
        if (point.amountFen > maxRevenueFen) maxRevenueFen = point.amountFen;
    }

    const QDateTime firstTime(points.first().date, QTime(0, 0));
    const QDateTime lastTime(points.last().date, QTime(23, 59, 59));
    m_revenueAxisX->setRange(firstTime, lastTime);
    m_revenueAxisX->setTickCount(days == 7 ? 7 : 6);

    const double maxRevenueYuan = ecp::fenToYuan(maxRevenueFen).toDouble();
    const double yMax = maxRevenueFen > 0 ? maxRevenueYuan * 1.1 : 1.0;
    m_revenueAxisY->setRange(0, yMax);
}
#endif

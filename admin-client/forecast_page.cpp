#include "forecast_page.h"
#include "net_client.h"
#include "protocol.h"
#include <QAbstractItemView>
#include <QButtonGroup>
#include <QColor>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QSet>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>

namespace {

constexpr qreal CONGESTION_WARNING_THRESHOLD = 0.8;

QTableWidgetItem *centeredItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}

} // namespace

ForecastPage::ForecastPage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();
    connect(m_net, &NetClient::response, this, &ForecastPage::handleResponse);
    requestStationList();
    requestForecast();
}

void ForecastPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("负荷预测与预警"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    pageLayout->addWidget(title);

    auto *filterLayout = new QHBoxLayout;
    filterLayout->addWidget(new QLabel(QStringLiteral("站点"), this));
    m_stationFilter = new QComboBox(this);
    m_stationFilter->setMinimumWidth(210);
    m_stationFilter->addItem(QStringLiteral("全部站点"), qint64(0));
    filterLayout->addWidget(m_stationFilter);

    filterLayout->addSpacing(16);
    filterLayout->addWidget(new QLabel(QStringLiteral("预测周期"), this));
    auto *horizonGroup = new QButtonGroup(this);
    const QList<int> horizons = {1, 6, 24};
    for (int horizon : horizons) {
        auto *button = new QPushButton(
            QStringLiteral("%1 小时").arg(horizon), this);
        button->setCheckable(true);
        horizonGroup->addButton(button, horizon);
        filterLayout->addWidget(button);
        if (horizon == 1) button->setChecked(true);
    }
    horizonGroup->setExclusive(true);
    connect(horizonGroup, &QButtonGroup::idClicked, this,
            [this](int horizon) { m_selectedHorizon = horizon; });

    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    filterLayout->addWidget(refreshButton);
    filterLayout->addStretch();
    pageLayout->addLayout(filterLayout);

    m_statusLabel = new QLabel(QStringLiteral("准备加载预测数据"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#667085"));
    m_statusLabel->setWordWrap(true);
    pageLayout->addWidget(m_statusLabel);

    auto *summaryLayout = new QHBoxLayout;
    summaryLayout->setSpacing(16);
    summaryLayout->addWidget(
        createMetricCard(QStringLiteral("预测站点数"), m_stationCount), 1);
    summaryLayout->addWidget(
        createMetricCard(QStringLiteral("拥堵预警数"), m_warningCount), 1);
    summaryLayout->addWidget(
        createMetricCard(QStringLiteral("高峰站点数"), m_peakCount), 1);
    summaryLayout->addWidget(
        createMetricCard(QStringLiteral("最大预测负荷"), m_maxLoad), 1);
    pageLayout->addLayout(summaryLayout);

    m_table = new QTableWidget(0, 9, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("站点"), QStringLiteral("预测周期"),
        QStringLiteral("预测时间"), QStringLiteral("预测负荷（kW）"),
        QStringLiteral("预测空闲桩"), QStringLiteral("拥堵度"),
        QStringLiteral("高峰时段"), QStringLiteral("预警状态"),
        QStringLiteral("模型版本")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    pageLayout->addWidget(m_table, 1);

    connect(refreshButton, &QPushButton::clicked,
            this, &ForecastPage::requestForecast);
}

void ForecastPage::requestStationList()
{
    m_stationListSeq = -1;
    m_stationListPage = 0;
    m_stationListTotal = 0;
    m_stationListSelectedId = m_stationFilter->currentData().toLongLong();
    m_pendingStationOptions.clear();
    m_pendingStationOptionIds.clear();
    m_stationFilter->setToolTip(QStringLiteral("正在加载站点筛选项…"));
    requestStationListPage(1);
}

void ForecastPage::requestStationListPage(int page)
{
    const int seq = m_net->send(ecp::CMD_STATION_LIST, QJsonObject{
        { QStringLiteral("page"), page },
        { QStringLiteral("size"), STATION_OPTION_PAGE_SIZE }
    });
    if (seq < 0) {
        abortStationListLoad(
            QStringLiteral("站点筛选项请求发送失败，请检查网络连接"));
        return;
    }
    m_stationListSeq = seq;
    m_stationListPage = page;
}

void ForecastPage::abortStationListLoad(const QString &message)
{
    m_stationListSeq = -1;
    m_stationListPage = 0;
    m_stationListTotal = 0;
    m_pendingStationOptions.clear();
    m_pendingStationOptionIds.clear();
    m_stationFilter->setToolTip(message);
}

void ForecastPage::commitStationOptions()
{
    const int acceptedCount = m_pendingStationOptions.size();
    const QSignalBlocker blocker(m_stationFilter);
    m_stationFilter->clear();
    m_stationFilter->addItem(QStringLiteral("全部站点"), qint64(0));
    for (const StationOption &option : m_pendingStationOptions)
        m_stationFilter->addItem(option.name, option.stationId);

    const int selectedIndex = m_stationFilter->findData(m_stationListSelectedId);
    m_stationFilter->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    m_stationFilter->setToolTip(
        QStringLiteral("已加载 %1 / %2 个站点筛选项")
            .arg(acceptedCount).arg(m_stationListTotal));

    m_stationListPage = 0;
    m_pendingStationOptions.clear();
    m_pendingStationOptionIds.clear();
}

void ForecastPage::requestForecast()
{
    m_statusLabel->setText(QStringLiteral("正在加载预测数据…"));
    const int seq = m_net->send(ecp::CMD_STAT_LOAD_FORECAST, QJsonObject{
        { QStringLiteral("stationId"), m_stationFilter->currentData().toLongLong() },
        { QStringLiteral("horizon"), m_selectedHorizon }
    });
    if (seq < 0) {
        m_forecastSeq = -1;
        m_statusLabel->setText(QStringLiteral("预测请求发送失败，请检查网络连接"));
        return;
    }
    m_forecastSeq = seq;
}

void ForecastPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                                  const QJsonObject &data)
{
    if (cmd == ecp::CMD_STATION_LIST) {
        if (seq != m_stationListSeq) return;
        m_stationListSeq = -1;
        handleStationListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_STAT_LOAD_FORECAST) {
        if (seq != m_forecastSeq) return;
        m_forecastSeq = -1;
        handleForecastResponse(code, msg, data);
    }
}

void ForecastPage::handleStationListResponse(int code, const QString &msg,
                                             const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        abortStationListLoad(
            QStringLiteral("站点筛选项加载失败：%1").arg(msg));
        return;
    }

    const QJsonValue totalValue = data.value(QStringLiteral("total"));
    const QJsonValue listValue = data.value(QStringLiteral("list"));
    const qint64 total = totalValue.toInteger(-1);
    if (!totalValue.isDouble() || total < 0 || !listValue.isArray()) {
        abortStationListLoad(
            QStringLiteral("站点筛选项加载失败：服务器响应格式异常"));
        return;
    }

    m_stationListTotal = total;
    const QJsonArray list = listValue.toArray();
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        const QJsonValue idValue = item.value(QStringLiteral("stationId"));
        const QJsonValue nameValue = item.value(QStringLiteral("name"));
        if (!idValue.isDouble() || !nameValue.isString()) continue;
        const qint64 stationId = idValue.toInteger();
        const QString name = nameValue.toString();
        if (stationId <= 0 || name.isEmpty()
            || m_pendingStationOptionIds.contains(stationId)) {
            continue;
        }
        m_pendingStationOptions.append({stationId, name});
        m_pendingStationOptionIds.insert(stationId);
    }

    if (qint64(m_stationListPage) * STATION_OPTION_PAGE_SIZE < total) {
        requestStationListPage(m_stationListPage + 1);
        return;
    }
    commitStationOptions();
}

void ForecastPage::handleForecastResponse(int code, const QString &msg,
                                          const QJsonObject &data)
{
    if (code == ecp::ERR_NOT_LOGIN || code == ecp::ERR_TOKEN_INVALID) return;
    if (code == ecp::ERR_CMD_UNKNOWN) {
        m_statusLabel->setText(
            QStringLiteral("服务端 2305 尚未接入，预测页面已就绪"));
        return;
    }
    if (code != ecp::ERR_OK) {
        m_statusLabel->setText(msg.isEmpty() ? ecp::errMsg(code) : msg);
        return;
    }

    const QJsonValue listValue = data.value(QStringLiteral("list"));
    if (!listValue.isArray()) {
        m_statusLabel->setText(QStringLiteral("预测数据格式异常：缺少 list 数组"));
        return;
    }

    QVector<ForecastData> forecasts;
    const QJsonArray list = listValue.toArray();
    forecasts.reserve(list.size());
    for (const QJsonValue &value : list) {
        if (!value.isObject()) {
            m_statusLabel->setText(QStringLiteral("预测数据格式异常：列表项不是对象"));
            return;
        }
        ForecastData forecast;
        if (!parseForecastItem(value.toObject(), forecast)) {
            m_statusLabel->setText(QStringLiteral("预测数据格式异常：字段类型或取值无效"));
            return;
        }
        forecasts.append(forecast);
    }

    m_forecasts = forecasts;
    refreshTable();
    updateSummary();
    m_statusLabel->setText(m_forecasts.isEmpty()
        ? QStringLiteral("暂无预测数据，请先运行负荷预测")
        : QStringLiteral("已加载 %1 条预测数据").arg(m_forecasts.size()));
}

void ForecastPage::refreshTable()
{
    m_table->setRowCount(0);
    const QColor warningBackground(QStringLiteral("#fff1f0"));
    const QColor warningText(QStringLiteral("#b42318"));

    for (const ForecastData &forecast : m_forecasts) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, new QTableWidgetItem(forecast.stationName));
        m_table->setItem(row, 1, centeredItem(
            QStringLiteral("%1 小时").arg(forecast.horizon)));
        m_table->setItem(row, 2, centeredItem(forecast.predictTime));
        m_table->setItem(row, 3, centeredItem(
            QString::number(forecast.loadKw, 'f', 1)));
        m_table->setItem(row, 4, centeredItem(QString::number(forecast.idlePile)));
        m_table->setItem(row, 5, centeredItem(congestionText(forecast.congestion)));
        m_table->setItem(row, 6, centeredItem(
            forecast.isPeak ? QStringLiteral("是") : QStringLiteral("否")));
        const bool warning = forecast.congestion >= CONGESTION_WARNING_THRESHOLD;
        auto *warningItem = centeredItem(
            warning ? QStringLiteral("拥堵预警") : QStringLiteral("正常"));
        m_table->setItem(row, 7, warningItem);
        m_table->setItem(row, 8, centeredItem(forecast.modelVersion));

        if (warning) {
            for (int column = 0; column < m_table->columnCount(); ++column)
                m_table->item(row, column)->setBackground(warningBackground);
            QFont font = warningItem->font();
            font.setBold(true);
            warningItem->setFont(font);
            warningItem->setForeground(warningText);
        }
    }
}

void ForecastPage::updateSummary()
{
    QSet<qint64> stations;
    QSet<qint64> warningStations;
    QSet<qint64> peakStations;
    qreal maxLoadKw = 0;
    bool hasLoad = false;

    for (const ForecastData &forecast : m_forecasts) {
        stations.insert(forecast.stationId);
        if (forecast.congestion >= CONGESTION_WARNING_THRESHOLD)
            warningStations.insert(forecast.stationId);
        if (forecast.isPeak) peakStations.insert(forecast.stationId);
        if (!hasLoad || forecast.loadKw > maxLoadKw) {
            maxLoadKw = forecast.loadKw;
            hasLoad = true;
        }
    }

    m_stationCount->setText(QString::number(stations.size()));
    m_warningCount->setText(QString::number(warningStations.size()));
    m_peakCount->setText(QString::number(peakStations.size()));
    m_maxLoad->setText(hasLoad
        ? QStringLiteral("%1 kW").arg(QString::number(maxLoadKw, 'f', 1))
        : QStringLiteral("—"));
}

QWidget *ForecastPage::createMetricCard(const QString &title, QLabel *&valueLabel)
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
    valueLabel = new QLabel(title == QStringLiteral("最大预测负荷")
        ? QStringLiteral("—") : QStringLiteral("0"), card);
    QFont valueFont = valueLabel->font();
    valueFont.setPointSize(18);
    valueFont.setBold(true);
    valueLabel->setFont(valueFont);

    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    return card;
}

QString ForecastPage::congestionText(qreal congestion)
{
    return QStringLiteral("%1%").arg(QString::number(congestion * 100.0, 'f', 0));
}

bool ForecastPage::parsePeakValue(const QJsonObject &item, bool &isPeak)
{
    const QJsonValue value = item.value(QStringLiteral("isPeak"));
    if (value.isBool()) {
        isPeak = value.toBool();
        return true;
    }
    if (!value.isDouble()) return false;
    const double peak = value.toDouble(-1.0);
    if (peak != 0.0 && peak != 1.0) return false;
    isPeak = peak == 1.0;
    return true;
}

bool ForecastPage::parseForecastItem(const QJsonObject &item,
                                     ForecastData &forecast)
{
    const QJsonValue stationId = item.value(QStringLiteral("stationId"));
    const QJsonValue stationName = item.value(QStringLiteral("stationName"));
    const QJsonValue horizon = item.value(QStringLiteral("horizon"));
    const QJsonValue predictTime = item.value(QStringLiteral("predictTime"));
    const QJsonValue loadKw = item.value(QStringLiteral("loadKw"));
    const QJsonValue idlePile = item.value(QStringLiteral("idlePile"));
    const QJsonValue congestion = item.value(QStringLiteral("congestion"));
    const QJsonValue modelVersion = item.value(QStringLiteral("modelVersion"));
    if (!stationId.isDouble() || !stationName.isString() || !horizon.isDouble()
        || !predictTime.isString() || !loadKw.isDouble() || !idlePile.isDouble()
        || !congestion.isDouble() || !modelVersion.isString()) {
        return false;
    }

    const qint64 parsedStationId = stationId.toInteger(-1);
    const int parsedHorizon = horizon.toInt(-1);
    const qint64 parsedIdlePile = idlePile.toInteger(-1);
    const qreal parsedCongestion = congestion.toDouble(-1);
    if (parsedStationId <= 0 || stationName.toString().isEmpty()
        || (parsedHorizon != 1 && parsedHorizon != 6 && parsedHorizon != 24)
        || parsedIdlePile < 0 || parsedCongestion < 0 || parsedCongestion > 1
        || !parsePeakValue(item, forecast.isPeak)) {
        return false;
    }

    forecast.stationId = parsedStationId;
    forecast.stationName = stationName.toString();
    forecast.horizon = parsedHorizon;
    forecast.predictTime = predictTime.toString();
    forecast.loadKw = loadKw.toDouble();
    forecast.idlePile = parsedIdlePile;
    forecast.congestion = parsedCongestion;
    forecast.modelVersion = modelVersion.toString();
    return true;
}

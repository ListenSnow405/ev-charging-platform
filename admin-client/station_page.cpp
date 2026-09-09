#include "station_page.h"
#include "add_station_dialog.h"
#include "net_client.h"
#include "loading_status.h"
#include "protocol.h"
#include <QAbstractItemView>
#include <QDialog>
#include <QFont>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int READ_RESPONSE_TIMEOUT_MS = 10000;
constexpr int WRITE_RESPONSE_TIMEOUT_MS = 10000;

QTableWidgetItem *centeredItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}

} // namespace

StationPage::StationPage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();
    m_stationListTimer = new QTimer(this);
    m_stationListTimer->setSingleShot(true);
    connect(m_stationListTimer, &QTimer::timeout, this, [this] {
        if (m_stationListSeq < 0) return;
        m_stationListSeq = -1;
        updateLoadingState();
        m_requestedPage = m_currentPage;
        m_statusLabel->setMessage(QStringLiteral("电站列表请求超时，请重试"), LoadingStatus::Tone::Error);
        updatePaginationControls();
    });

    m_stationAddTimer = new QTimer(this);
    m_stationAddTimer->setSingleShot(true);
    connect(m_stationAddTimer, &QTimer::timeout, this, [this] {
        if (m_stationAddSeq < 0) return;
        m_stationAddSeq = -1;
        updateLoadingState();
        m_stationAddOutcomeUnknown = true;
        m_addButton->setEnabled(true);
        m_statusLabel->setMessage(
            QStringLiteral("新增电站“%1”响应超时，操作结果未知；请先刷新并核对，避免重复新增")
                .arg(m_pendingAddStationName), LoadingStatus::Tone::Error);
    });

    m_stationDetailTimer = new QTimer(this);
    m_stationDetailTimer->setSingleShot(true);
    connect(m_stationDetailTimer, &QTimer::timeout, this, [this] {
        if (m_stationDetailSeq < 0) return;
        m_stationDetailSeq = -1;
        updateLoadingState();
        m_pendingDetailStationId = 0;
        m_pendingDetailStationName.clear();
        m_pendingDetailStationAddress.clear();
        m_statusLabel->setMessage(QStringLiteral("电站详情请求超时，请重试"), LoadingStatus::Tone::Error);
    });

    connect(m_net, &NetClient::response, this, &StationPage::handleResponse);
    requestStationList(1);
}

void StationPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(12);

    auto *toolbar = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("充电站管理"), this);
    title->setObjectName(QStringLiteral("PageTitle"));
    toolbar->addWidget(title);
    toolbar->addStretch();

    m_detailsButton = new QPushButton(QStringLiteral("查看详情"), this);
    m_detailsButton->setEnabled(false);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    refreshButton->setObjectName(QStringLiteral("Secondary"));
    m_addButton = new QPushButton(QStringLiteral("新增电站"), this);
    m_addButton->setObjectName(QStringLiteral("Primary"));
    toolbar->addWidget(m_detailsButton);
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(m_addButton);
    pageLayout->addLayout(toolbar);

    m_statusLabel = new LoadingStatus(QStringLiteral("准备加载电站列表"), this);
    pageLayout->addWidget(m_statusLabel);

    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("电站 ID"), QStringLiteral("站名"), QStringLiteral("详细地址"),
        QStringLiteral("经度"), QStringLiteral("纬度"), QStringLiteral("总电桩数"),
        QStringLiteral("在线率")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setMouseTracking(true);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->setDefaultSectionSize(40);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    pageLayout->addWidget(m_table, 1);

    auto *pagination = new QHBoxLayout;
    m_previousPageButton = new QPushButton(QStringLiteral("上一页"), this);
    m_previousPageButton->setObjectName(QStringLiteral("Secondary"));
    m_pageLabel = new QLabel(this);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_nextPageButton = new QPushButton(QStringLiteral("下一页"), this);
    m_nextPageButton->setObjectName(QStringLiteral("Secondary"));
    pagination->addStretch();
    pagination->addWidget(m_previousPageButton);
    pagination->addWidget(m_pageLabel);
    pagination->addWidget(m_nextPageButton);
    pagination->addStretch();
    pageLayout->addLayout(pagination);
    updatePaginationControls();

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        m_detailsButton->setEnabled(m_table->currentRow() >= 0);
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { requestStationDetail(row); });
    connect(m_detailsButton, &QPushButton::clicked,
            this, &StationPage::showSelectedStationDetails);
    connect(refreshButton, &QPushButton::clicked, this, [this] {
        requestStationList(m_currentPage);
    });
    connect(m_previousPageButton, &QPushButton::clicked, this, [this] {
        requestStationList(m_currentPage - 1);
    });
    connect(m_nextPageButton, &QPushButton::clicked, this, [this] {
        requestStationList(m_currentPage + 1);
    });
    connect(m_addButton, &QPushButton::clicked, this, &StationPage::addStation);
}

void StationPage::requestStationList(int page)
{
    if (page < 1) return;

    m_statusLabel->setMessage(QStringLiteral("正在加载电站列表…"), LoadingStatus::Tone::Loading);
    const int seq = m_net->send(ecp::CMD_STATION_LIST, QJsonObject{
        { QStringLiteral("page"), page },
        { QStringLiteral("size"), PAGE_SIZE }
    });
    if (seq < 0) {
        m_stationListSeq = -1;
        updateLoadingState();
        m_requestedPage = m_currentPage;
        m_statusLabel->setMessage(QStringLiteral("电站列表请求发送失败，请检查网络连接"), LoadingStatus::Tone::Error);
        updatePaginationControls();
        return;
    }
    m_stationListSeq = seq;
    updateLoadingState();
    m_requestedPage = page;
    m_stationListTimer->start(READ_RESPONSE_TIMEOUT_MS);
    updatePaginationControls();
}

void StationPage::requestStationDetail(int row)
{
    if (row < 0 || row >= m_stations.size()) {
        QMessageBox::information(this, QStringLiteral("查看详情"),
                                 QStringLiteral("请先选择一个充电站"));
        return;
    }

    const StationData &station = m_stations.at(row);
    const int seq = m_net->send(ecp::CMD_STATION_DETAIL, QJsonObject{
        { QStringLiteral("stationId"), station.stationId }
    });
    if (seq < 0) {
        QMessageBox::warning(this, QStringLiteral("查看详情失败"),
                             QStringLiteral("请求发送失败，请检查网络连接"));
        return;
    }

    m_stationDetailSeq = seq;
    updateLoadingState();
    m_pendingDetailStationId = station.stationId;
    m_pendingDetailStationName = station.name;
    m_pendingDetailStationAddress = station.address;
    m_stationDetailTimer->start(READ_RESPONSE_TIMEOUT_MS);
    m_statusLabel->setMessage(QStringLiteral("正在加载“%1”的电桩详情…").arg(station.name), LoadingStatus::Tone::Loading);
}

void StationPage::addStation()
{
    if (m_stationAddOutcomeUnknown) {
        const auto answer = QMessageBox::warning(
            this, QStringLiteral("上一次新增结果待确认"),
            QStringLiteral("上一次新增电站请求响应超时，服务端可能已经完成创建。\n"
                           "重复提交可能产生重复电站。\n\n"
                           "请先刷新并核对电站列表或数据库。\n"
                           "仅在已经确认仍需要再次新增时继续。"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
    }

    AddStationDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    const StationFormData &input = dialog.stationData();
    const int seq = m_net->send(ecp::CMD_STATION_ADD, QJsonObject{
        { QStringLiteral("name"), input.name },
        { QStringLiteral("address"), input.address },
        { QStringLiteral("lng"), input.longitude },
        { QStringLiteral("lat"), input.latitude },
        { QStringLiteral("price"), input.priceFen },
        { QStringLiteral("pileCount"), input.pileCount }
    });
    if (seq < 0) {
        m_stationAddTimer->stop();
        m_stationAddSeq = -1;
        updateLoadingState();
        QMessageBox::warning(this, QStringLiteral("新增电站失败"),
                             QStringLiteral("请求发送失败，请检查网络连接"));
        return;
    }

    m_stationAddSeq = seq;
    updateLoadingState();
    m_pendingAddStationName = input.name;
    m_stationAddOutcomeUnknown = false;
    m_stationAddTimer->start(WRITE_RESPONSE_TIMEOUT_MS);
    m_addButton->setEnabled(false);
    m_statusLabel->setMessage(QStringLiteral("正在新增电站…"), LoadingStatus::Tone::Loading);
}

void StationPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                                 const QJsonObject &data)
{
    if (cmd == ecp::CMD_STATION_LIST) {
        if (seq != m_stationListSeq) return;
        m_stationListTimer->stop();
        m_stationListSeq = -1;
        updateLoadingState();
        handleStationListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_STATION_ADD) {
        if (seq != m_stationAddSeq) return;
        m_stationAddTimer->stop();
        m_stationAddSeq = -1;
        updateLoadingState();
        m_stationAddOutcomeUnknown = false;
        m_addButton->setEnabled(true);
        handleStationAddResponse(code, msg, data);
        m_pendingAddStationName.clear();
        return;
    }
    if (cmd == ecp::CMD_STATION_DETAIL) {
        if (seq != m_stationDetailSeq) return;
        m_stationDetailTimer->stop();
        m_stationDetailSeq = -1;
        updateLoadingState();
        handleStationDetailResponse(code, msg, data);
    }
}

void StationPage::handleStationListResponse(int code, const QString &msg,
                                            const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_requestedPage = m_currentPage;
        m_statusLabel->setMessage(QStringLiteral("电站列表加载失败：%1").arg(msg), LoadingStatus::Tone::Error);
        updatePaginationControls();
        return;
    }

    const QJsonValue totalValue = data.value(QStringLiteral("total"));
    const QJsonValue listValue = data.value(QStringLiteral("list"));
    const qint64 total = totalValue.toInteger(-1);
    if (!totalValue.isDouble() || total < 0 || !listValue.isArray()) {
        m_requestedPage = m_currentPage;
        m_statusLabel->setMessage(QStringLiteral("电站列表加载失败：服务器响应格式异常"), LoadingStatus::Tone::Error);
        updatePaginationControls();
        return;
    }

    QVector<StationData> stations;
    const QJsonArray list = listValue.toArray();
    stations.reserve(list.size());
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        stations.append({
            item.value(QStringLiteral("stationId")).toInteger(),
            item.value(QStringLiteral("name")).toString(),
            item.value(QStringLiteral("address")).toString(),
            item.value(QStringLiteral("lng")).toDouble(),
            item.value(QStringLiteral("lat")).toDouble(),
            item.value(QStringLiteral("pileTotal")).toInteger(),
            item.value(QStringLiteral("onlineRate")).toDouble()
        });
    }

    m_stations = stations;
    m_currentPage = total == 0 ? 1 : m_requestedPage;
    m_requestedPage = m_currentPage;
    m_total = total;
    m_table->setRowCount(0);
    for (const StationData &station : m_stations) appendStationRow(station);
    m_table->clearSelection();
    m_detailsButton->setEnabled(false);

    m_statusLabel->setMessage(QStringLiteral("已加载 %1 个电站，共 %2 个")
                               .arg(m_stations.size()).arg(total), total == 0 ? LoadingStatus::Tone::Neutral : LoadingStatus::Tone::Success);
    updatePaginationControls();
}

void StationPage::handleStationAddResponse(int code, const QString &msg,
                                           const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_statusLabel->setMessage(QStringLiteral("新增电站失败：%1").arg(msg), LoadingStatus::Tone::Error);
        QMessageBox::warning(this, QStringLiteral("新增电站失败"), msg);
        return;
    }

    const qint64 stationId = data.value(QStringLiteral("stationId")).toInteger();
    if (stationId <= 0) {
        m_statusLabel->setMessage(QStringLiteral("新增电站响应异常：缺少有效电站 ID"), LoadingStatus::Tone::Error);
        QMessageBox::warning(this, QStringLiteral("新增电站失败"),
                             QStringLiteral("服务器响应缺少有效电站 ID"));
        requestStationList(m_currentPage);
        return;
    }

    QMessageBox::information(this, QStringLiteral("新增电站成功"),
                             QStringLiteral("新增电站成功，电站 ID：%1").arg(stationId));
    requestStationList(m_currentPage);
}

void StationPage::handleStationDetailResponse(int code, const QString &msg,
                                              const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_statusLabel->setMessage(QStringLiteral("电桩详情加载失败：%1").arg(msg), LoadingStatus::Tone::Error);
        QMessageBox::warning(this, QStringLiteral("查看详情失败"), msg);
        return;
    }

    QVector<PileData> piles;
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
    piles.reserve(list.size());
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        piles.append({
            item.value(QStringLiteral("code")).toString(),
            item.value(QStringLiteral("type")).toInt(),
            item.value(QStringLiteral("status")).toInt(),
            item.value(QStringLiteral("power")).toDouble()
        });
    }

    m_statusLabel->setMessage(QStringLiteral("已加载“%1”的 %2 个电桩")
                               .arg(m_pendingDetailStationName).arg(piles.size()), LoadingStatus::Tone::Success);
    showStationDetailDialog(piles);
}

void StationPage::appendStationRow(const StationData &station)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);
    m_table->setItem(row, 0, centeredItem(QString::number(station.stationId)));
    m_table->setItem(row, 1, new QTableWidgetItem(station.name));
    m_table->setItem(row, 2, new QTableWidgetItem(station.address));
    m_table->setItem(row, 3, centeredItem(QString::number(station.longitude, 'f', 4)));
    m_table->setItem(row, 4, centeredItem(QString::number(station.latitude, 'f', 4)));
    m_table->setItem(row, 5, centeredItem(QString::number(station.pileTotal)));
    m_table->setItem(row, 6, centeredItem(
        QStringLiteral("%1%").arg(QString::number(station.onlineRatePercent, 'f', 1))));
}

void StationPage::updatePaginationControls()
{
    const qint64 totalPages = m_total > 0
        ? (m_total + PAGE_SIZE - 1) / PAGE_SIZE
        : 1;
    m_pageLabel->setText(QStringLiteral("第 %1 / %2 页，共 %3 个电站")
                             .arg(m_currentPage).arg(totalPages).arg(m_total));

    const bool requestPending = m_stationListSeq >= 0;
    m_previousPageButton->setEnabled(!requestPending && m_currentPage > 1);
    m_nextPageButton->setEnabled(
        !requestPending && qint64(m_currentPage) * PAGE_SIZE < m_total);
}

void StationPage::showSelectedStationDetails()
{
    requestStationDetail(m_table->currentRow());
}

void StationPage::showStationDetailDialog(const QVector<PileData> &piles)
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("%1 · 电桩实时状态明细")
                              .arg(m_pendingDetailStationName));
    dialog.resize(680, 420);

    auto *layout = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(
        QStringLiteral("电站 ID：%1　地址：%2")
            .arg(m_pendingDetailStationId).arg(m_pendingDetailStationAddress),
        &dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *table = new QTableWidget(piles.size(), 4, &dialog);
    table->setHorizontalHeaderLabels({
        QStringLiteral("电桩编号"), QStringLiteral("类型"),
        QStringLiteral("功率（kW）"), QStringLiteral("状态")
    });
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setMouseTracking(true);
    table->setShowGrid(false);
    table->verticalHeader()->setDefaultSectionSize(40);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int row = 0; row < piles.size(); ++row) {
        const PileData &pile = piles.at(row);
        table->setItem(row, 0, centeredItem(pile.code));
        table->setItem(row, 1, centeredItem(typeText(pile.type)));
        table->setItem(row, 2, centeredItem(QString::number(pile.powerKw, 'f', 1)));
        table->setItem(row, 3, centeredItem(statusText(pile.status)));
    }
    layout->addWidget(table, 1);

    auto *closeButton = new QPushButton(QStringLiteral("关闭"), &dialog);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    auto *buttonLayout = new QHBoxLayout;
    buttonLayout->addStretch();
    buttonLayout->addWidget(closeButton);
    layout->addLayout(buttonLayout);
    dialog.exec();
}

QString StationPage::typeText(int type)
{
    switch (type) {
    case ecp::PILE_FAST: return QStringLiteral("快充");
    case ecp::PILE_SLOW: return QStringLiteral("慢充");
    default:             return QStringLiteral("未知");
    }
}

QString StationPage::statusText(int status)
{
    switch (status) {
    case ecp::PILE_IN_USE: return QStringLiteral("在用");
    case ecp::PILE_IDLE:   return QStringLiteral("闲置");
    case ecp::PILE_FAULT:  return QStringLiteral("故障");
    default:               return QStringLiteral("未知");
    }
}

void StationPage::updateLoadingState()
{
    m_statusLabel->setLoading(m_stationListSeq >= 0 || m_stationAddSeq >= 0 || m_stationDetailSeq >= 0);
}

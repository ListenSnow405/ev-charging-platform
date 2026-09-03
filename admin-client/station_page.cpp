#include "station_page.h"
#include "add_station_dialog.h"
#include "net_client.h"
#include "protocol.h"
#include <QAbstractItemView>
#include <QDialog>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

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
    connect(m_net, &NetClient::response, this, &StationPage::handleResponse);
    requestStationList();
}

void StationPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(12);

    auto *toolbar = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("充电站管理"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    toolbar->addWidget(title);
    toolbar->addStretch();

    m_detailsButton = new QPushButton(QStringLiteral("查看详情"), this);
    m_detailsButton->setEnabled(false);
    auto *refreshButton = new QPushButton(QStringLiteral("刷新"), this);
    m_addButton = new QPushButton(QStringLiteral("新增电站"), this);
    toolbar->addWidget(m_detailsButton);
    toolbar->addWidget(refreshButton);
    toolbar->addWidget(m_addButton);
    pageLayout->addLayout(toolbar);

    m_statusLabel = new QLabel(QStringLiteral("准备加载电站列表"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#667085"));
    pageLayout->addWidget(m_statusLabel);

    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("电站 ID"), QStringLiteral("站名"), QStringLiteral("详细地址"),
        QStringLiteral("经度"), QStringLiteral("纬度"), QStringLiteral("总电桩数"),
        QStringLiteral("在线率")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    pageLayout->addWidget(m_table, 1);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        m_detailsButton->setEnabled(m_table->currentRow() >= 0);
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { requestStationDetail(row); });
    connect(m_detailsButton, &QPushButton::clicked,
            this, &StationPage::showSelectedStationDetails);
    connect(refreshButton, &QPushButton::clicked,
            this, &StationPage::requestStationList);
    connect(m_addButton, &QPushButton::clicked, this, &StationPage::addStation);
}

void StationPage::requestStationList()
{
    m_statusLabel->setText(QStringLiteral("正在加载电站列表…"));
    const int seq = m_net->send(ecp::CMD_STATION_LIST, QJsonObject{
        { QStringLiteral("page"), 1 },
        { QStringLiteral("size"), 100 }
    });
    if (seq < 0) {
        m_stationListSeq = -1;
        m_statusLabel->setText(QStringLiteral("电站列表请求发送失败，请检查网络连接"));
        return;
    }
    m_stationListSeq = seq;
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
    m_pendingDetailStationId = station.stationId;
    m_pendingDetailStationName = station.name;
    m_pendingDetailStationAddress = station.address;
    m_statusLabel->setText(QStringLiteral("正在加载“%1”的电桩详情…").arg(station.name));
}

void StationPage::addStation()
{
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
        QMessageBox::warning(this, QStringLiteral("新增电站失败"),
                             QStringLiteral("请求发送失败，请检查网络连接"));
        return;
    }

    m_stationAddSeq = seq;
    m_addButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("正在新增电站…"));
}

void StationPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                                 const QJsonObject &data)
{
    if (cmd == ecp::CMD_STATION_LIST) {
        if (seq != m_stationListSeq) return;
        m_stationListSeq = -1;
        handleStationListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_STATION_ADD) {
        if (seq != m_stationAddSeq) return;
        m_stationAddSeq = -1;
        m_addButton->setEnabled(true);
        handleStationAddResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_STATION_DETAIL) {
        if (seq != m_stationDetailSeq) return;
        m_stationDetailSeq = -1;
        handleStationDetailResponse(code, msg, data);
    }
}

void StationPage::handleStationListResponse(int code, const QString &msg,
                                            const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_statusLabel->setText(QStringLiteral("电站列表加载失败：%1").arg(msg));
        return;
    }

    QVector<StationData> stations;
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
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
    m_table->setRowCount(0);
    for (const StationData &station : m_stations) appendStationRow(station);
    m_table->clearSelection();
    m_detailsButton->setEnabled(false);

    const qint64 total = data.value(QStringLiteral("total")).toInteger(m_stations.size());
    m_statusLabel->setText(QStringLiteral("已加载 %1 个电站，共 %2 个")
                               .arg(m_stations.size()).arg(total));
}

void StationPage::handleStationAddResponse(int code, const QString &msg,
                                           const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_statusLabel->setText(QStringLiteral("新增电站失败：%1").arg(msg));
        QMessageBox::warning(this, QStringLiteral("新增电站失败"), msg);
        return;
    }

    const qint64 stationId = data.value(QStringLiteral("stationId")).toInteger();
    if (stationId <= 0) {
        m_statusLabel->setText(QStringLiteral("新增电站响应异常：缺少有效电站 ID"));
        QMessageBox::warning(this, QStringLiteral("新增电站失败"),
                             QStringLiteral("服务器响应缺少有效电站 ID"));
        requestStationList();
        return;
    }

    QMessageBox::information(this, QStringLiteral("新增电站成功"),
                             QStringLiteral("新增电站成功，电站 ID：%1").arg(stationId));
    requestStationList();
}

void StationPage::handleStationDetailResponse(int code, const QString &msg,
                                              const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_statusLabel->setText(QStringLiteral("电桩详情加载失败：%1").arg(msg));
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

    m_statusLabel->setText(QStringLiteral("已加载“%1”的 %2 个电桩")
                               .arg(m_pendingDetailStationName).arg(piles.size()));
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

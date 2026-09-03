#include "station_page.h"
#include "add_station_dialog.h"
#include "time_util.h"
#include <QAbstractItemView>
#include <QDialog>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace {

QString onlineRateText(int onlineCount, int total)
{
    if (total <= 0) return QStringLiteral("0.0%");
    const int tenths = (onlineCount * 1000 + total / 2) / total;
    return QStringLiteral("%1.%2%").arg(tenths / 10).arg(tenths % 10);
}

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
    loadMockStations();
}

void StationPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(16);

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
    auto *addButton = new QPushButton(QStringLiteral("新增电站"), this);
    toolbar->addWidget(m_detailsButton);
    toolbar->addWidget(addButton);
    pageLayout->addLayout(toolbar);

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
            [this](int row, int) { showStationDetails(row); });
    connect(m_detailsButton, &QPushButton::clicked,
            this, &StationPage::showSelectedStationDetails);
    connect(addButton, &QPushButton::clicked, this, &StationPage::addStation);
}

void StationPage::loadMockStations()
{
    // Mock 数据集中于此，待 2101/2103 接入后替换为服务端响应。
    const auto pilesForStation = [](int stationId, int pileCount) {
        QVector<PileMock> piles;
        piles.reserve(pileCount);
        for (int i = 1; i <= pileCount; ++i) {
            const QString code = QStringLiteral("SZ%1-%2")
                .arg(stationId, 3, 10, QLatin1Char('0'))
                .arg(i, 2, 10, QLatin1Char('0'));
            const QString type = i <= 2 ? QStringLiteral("快充") : QStringLiteral("慢充");
            const qreal powerKw = i <= 2 ? 120 : 7;
            QString status = QStringLiteral("闲置");
            if ((stationId + i) % 7 == 0) status = QStringLiteral("故障");
            else if ((stationId + i) % 3 == 0) status = QStringLiteral("在用");
            piles.append({ code, type, powerKw, status });
        }
        return piles;
    };

    m_stations = {
        { 1, QStringLiteral("福田CBD充电站"), QStringLiteral("深圳市福田区福华三路 88 号"),
          114.0579, 22.5410, 152, 4, 4, pilesForStation(1, 4) },
        { 2, QStringLiteral("南山科技园充电站"), QStringLiteral("深圳市南山区科苑南路 3009 号"),
          113.9455, 22.5390, 148, 4, 3, pilesForStation(2, 4) },
        { 3, QStringLiteral("深圳市民中心充电站"), QStringLiteral("深圳市福田区福中三路 1 号"),
          114.0650, 22.5470, 160, 4, 4, pilesForStation(3, 4) },
        { 4, QStringLiteral("深圳湾公园充电站"), QStringLiteral("深圳市南山区望海路 99 号"),
          113.9720, 22.5130, 145, 4, 3, pilesForStation(4, 4) },
        { 5, QStringLiteral("宝安中心充电站"), QStringLiteral("深圳市宝安区新湖路 99 号"),
          113.8830, 22.5550, 140, 4, 4, pilesForStation(5, 4) },
        { 6, QStringLiteral("龙岗大运充电站"), QStringLiteral("深圳市龙岗区龙翔大道 8 号"),
          114.2180, 22.6900, 138, 4, 2, pilesForStation(6, 4) }
    };

    m_table->setRowCount(0);
    for (const StationMock &station : m_stations) appendStationRow(station);
}

void StationPage::appendStationRow(const StationMock &station)
{
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto *idItem = centeredItem(QString::number(station.stationId));
    idItem->setData(Qt::DisplayRole, station.stationId);
    m_table->setItem(row, 0, idItem);
    m_table->setItem(row, 1, new QTableWidgetItem(station.name));
    m_table->setItem(row, 2, new QTableWidgetItem(station.address));
    m_table->setItem(row, 3, centeredItem(QString::number(station.longitude, 'f', 4)));
    m_table->setItem(row, 4, centeredItem(QString::number(station.latitude, 'f', 4)));
    m_table->setItem(row, 5, centeredItem(QString::number(station.pileTotal)));
    m_table->setItem(row, 6, centeredItem(onlineRateText(station.onlineCount,
                                                        station.pileTotal)));
}

void StationPage::showSelectedStationDetails()
{
    showStationDetails(m_table->currentRow());
}

void StationPage::showStationDetails(int row)
{
    if (row < 0 || row >= m_stations.size()) {
        QMessageBox::information(this, QStringLiteral("查看详情"),
                                 QStringLiteral("请先选择一个充电站"));
        return;
    }

    const StationMock &station = m_stations.at(row);
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("%1 · 电桩实时状态明细").arg(station.name));
    dialog.resize(680, 420);

    auto *layout = new QVBoxLayout(&dialog);
    auto *summary = new QLabel(
        QStringLiteral("电站 ID：%1　地址：%2").arg(station.stationId).arg(station.address),
        &dialog);
    summary->setWordWrap(true);
    layout->addWidget(summary);

    auto *table = new QTableWidget(station.piles.size(), 4, &dialog);
    table->setHorizontalHeaderLabels({
        QStringLiteral("电桩编号"), QStringLiteral("类型"),
        QStringLiteral("功率（kW）"), QStringLiteral("状态")
    });
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    for (int pileRow = 0; pileRow < station.piles.size(); ++pileRow) {
        const PileMock &pile = station.piles.at(pileRow);
        table->setItem(pileRow, 0, centeredItem(pile.code));
        table->setItem(pileRow, 1, centeredItem(pile.type));
        table->setItem(pileRow, 2, centeredItem(QString::number(pile.powerKw, 'f', 1)));
        table->setItem(pileRow, 3, centeredItem(pile.status));
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

void StationPage::addStation()
{
    AddStationDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted) return;

    const StationFormData &input = dialog.stationData();
    const int stationId = nextStationId();
    StationMock station;
    station.stationId = stationId;
    station.name = input.name;
    station.address = input.address;
    station.longitude = input.longitude;
    station.latitude = input.latitude;
    station.priceFen = input.priceFen;
    station.pileTotal = input.pileCount;
    station.onlineCount = 0;
    for (int i = 1; i <= input.pileCount; ++i) {
        const QString code = QStringLiteral("SZ%1-%2")
            .arg(stationId, 3, 10, QLatin1Char('0'))
            .arg(i, 2, 10, QLatin1Char('0'));
        const bool fast = i % 2 == 1;
        station.piles.append({ code,
                               fast ? QStringLiteral("快充") : QStringLiteral("慢充"),
                               fast ? qreal(120) : qreal(7),
                               QStringLiteral("闲置") });
    }

    // TODO(L3)：服务端实现后替换为 2102 请求；当前只追加到本地 Mock 数据。
    m_stations.append(station);
    appendStationRow(station);
    m_table->selectRow(m_table->rowCount() - 1);
    QMessageBox::information(
        this, QStringLiteral("新增成功"),
        QStringLiteral("已临时新增“%1”\n充电价格：%2 元/度\n本次数据仅在当前运行期间有效。")
            .arg(station.name, ecp::fenToYuan(station.priceFen)));
}

int StationPage::nextStationId() const
{
    int nextId = 1;
    for (const StationMock &station : m_stations) {
        if (station.stationId >= nextId) nextId = station.stationId + 1;
    }
    return nextId;
}

#include "pile_page.h"
#include "protocol.h"
#include <QAbstractItemView>
#include <QComboBox>
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

QTableWidgetItem *centeredItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}

} // namespace

PilePage::PilePage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();
    loadMockData();
}

void PilePage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("充电桩管理"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    pageLayout->addWidget(title);

    auto *filterLayout = new QHBoxLayout;
    filterLayout->addWidget(new QLabel(QStringLiteral("所属电站"), this));
    m_stationFilter = new QComboBox(this);
    m_stationFilter->setMinimumWidth(190);
    m_stationFilter->addItem(QStringLiteral("全部"), -1);
    filterLayout->addWidget(m_stationFilter);

    filterLayout->addSpacing(16);
    filterLayout->addWidget(new QLabel(QStringLiteral("状态"), this));
    m_statusFilter = new QComboBox(this);
    m_statusFilter->addItem(QStringLiteral("全部"), -1);
    m_statusFilter->addItem(QStringLiteral("在用"), ecp::PILE_IN_USE);
    m_statusFilter->addItem(QStringLiteral("闲置"), ecp::PILE_IDLE);
    m_statusFilter->addItem(QStringLiteral("故障"), ecp::PILE_FAULT);
    filterLayout->addWidget(m_statusFilter);
    filterLayout->addStretch();

    m_rebootButton = new QPushButton(QStringLiteral("远程重启"), this);
    m_rebootButton->setEnabled(false);
    filterLayout->addWidget(m_rebootButton);
    pageLayout->addLayout(filterLayout);

    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("电桩编号"), QStringLiteral("所属电站"), QStringLiteral("类型"),
        QStringLiteral("功率（kW）"), QStringLiteral("当前状态"),
        QStringLiteral("累计充电次数"), QStringLiteral("累计充电时长")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    pageLayout->addWidget(m_table, 1);

    connect(m_stationFilter, &QComboBox::currentIndexChanged,
            this, [this](int) { refreshTable(); });
    connect(m_statusFilter, &QComboBox::currentIndexChanged,
            this, [this](int) { refreshTable(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        m_rebootButton->setEnabled(selectedPile() != nullptr);
    });
    connect(m_rebootButton, &QPushButton::clicked,
            this, &PilePage::handleRemoteReboot);
}

void PilePage::loadMockData()
{
    // Mock 数据集中于此，待 2111 接入后替换为服务端响应。
    struct StationSeed { int id; QString name; };
    const QVector<StationSeed> stations = {
        { 1, QStringLiteral("福田CBD充电站") },
        { 2, QStringLiteral("南山科技园充电站") },
        { 3, QStringLiteral("深圳市民中心充电站") },
        { 4, QStringLiteral("深圳湾公园充电站") },
        { 5, QStringLiteral("宝安中心充电站") },
        { 6, QStringLiteral("龙岗大运充电站") }
    };

    m_piles.clear();
    int pileId = 1;
    for (const StationSeed &station : stations) {
        m_stationFilter->addItem(station.name, station.id);
        for (int number = 1; number <= 4; ++number) {
            const int type = number <= 2 ? ecp::PILE_FAST : ecp::PILE_SLOW;
            int status = ecp::PILE_IDLE;
            if ((station.id + number) % 7 == 0) status = ecp::PILE_FAULT;
            else if ((station.id + number) % 3 == 0) status = ecp::PILE_IN_USE;

            const QString code = QStringLiteral("SZ%1-%2")
                .arg(station.id, 3, 10, QLatin1Char('0'))
                .arg(number, 2, 10, QLatin1Char('0'));
            const qreal powerKw = type == ecp::PILE_FAST ? qreal(120) : qreal(7);
            const int chargeCount = station.id * 37 + number * 11;
            const qint64 durationSeconds =
                qint64(station.id * 10 + number * 3) * 3600 + qint64(number * 17) * 60;
            m_piles.append({ pileId++, code, station.id, station.name, type, powerKw,
                             status, chargeCount, durationSeconds });
        }
    }

    refreshTable();
}

void PilePage::refreshTable()
{
    const int stationId = m_stationFilter->currentData().toInt();
    const int status = m_statusFilter->currentData().toInt();

    // TODO(L3)：服务端实现后，将本地过滤替换为 2111 请求：
    // {page, size, stationId, status}。当前不得发送真实网络请求。
    m_table->setRowCount(0);
    for (const PileMock &pile : m_piles) {
        if (stationId >= 0 && pile.stationId != stationId) continue;
        if (status >= 0 && pile.status != status) continue;

        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *codeItem = centeredItem(pile.code);
        codeItem->setData(Qt::UserRole, pile.pileId);
        m_table->setItem(row, 0, codeItem);
        m_table->setItem(row, 1, new QTableWidgetItem(pile.stationName));
        m_table->setItem(row, 2, centeredItem(typeText(pile.type)));
        m_table->setItem(row, 3, centeredItem(QString::number(pile.powerKw, 'f', 1)));
        m_table->setItem(row, 4, centeredItem(statusText(pile.status)));
        m_table->setItem(row, 5, centeredItem(QString::number(pile.chargeCount)));
        m_table->setItem(row, 6, centeredItem(durationText(pile.chargeDurationSeconds)));
    }

    m_table->clearSelection();
    m_rebootButton->setEnabled(false);
}

void PilePage::handleRemoteReboot()
{
    const PileMock *pile = selectedPile();
    if (!pile) {
        QMessageBox::information(this, QStringLiteral("远程重启"),
                                 QStringLiteral("请先选择一个电桩"));
        return;
    }

    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认远程重启"),
        QStringLiteral("确定要远程重启电桩 %1 吗？").arg(pile->code),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    // TODO(L3)：服务端实现后替换为 2112 请求：{pileId}。
    // 当前仅模拟操作，不发送真实网络请求。
    QMessageBox::information(this, QStringLiteral("操作成功"),
                             QStringLiteral("重启指令已模拟发送"));
}

const PilePage::PileMock *PilePage::selectedPile() const
{
    const int row = m_table->currentRow();
    const QTableWidgetItem *codeItem = row >= 0 ? m_table->item(row, 0) : nullptr;
    if (!codeItem) return nullptr;

    const int pileId = codeItem->data(Qt::UserRole).toInt();
    for (const PileMock &pile : m_piles) {
        if (pile.pileId == pileId) return &pile;
    }
    return nullptr;
}

QString PilePage::typeText(int type)
{
    switch (type) {
    case ecp::PILE_FAST: return QStringLiteral("快充");
    case ecp::PILE_SLOW: return QStringLiteral("慢充");
    default:             return QStringLiteral("未知");
    }
}

QString PilePage::statusText(int status)
{
    switch (status) {
    case ecp::PILE_IN_USE: return QStringLiteral("在用");
    case ecp::PILE_IDLE:   return QStringLiteral("闲置");
    case ecp::PILE_FAULT:  return QStringLiteral("故障");
    default:               return QStringLiteral("未知");
    }
}

QString PilePage::durationText(qint64 seconds)
{
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (hours > 0) {
        return QStringLiteral("%1小时%2分钟").arg(hours).arg(minutes);
    }
    return QStringLiteral("%1分钟").arg(minutes);
}

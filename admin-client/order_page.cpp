#include "order_page.h"
#include "protocol.h"
#include "time_util.h"
#include <QAbstractItemView>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTime>
#include <QVBoxLayout>

namespace {

QTableWidgetItem *centeredItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}

} // namespace

OrderPage::OrderPage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();
    loadMockData();
}

void OrderPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("订单管理"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    pageLayout->addWidget(title);

    auto *filterLayout = new QHBoxLayout;
    filterLayout->addWidget(new QLabel(QStringLiteral("状态"), this));
    m_statusFilter = new QComboBox(this);
    m_statusFilter->addItem(QStringLiteral("全部"), -1);
    m_statusFilter->addItem(QStringLiteral("已预约"), ecp::ORDER_RESERVED);
    m_statusFilter->addItem(QStringLiteral("充电中"), ecp::ORDER_CHARGING);
    m_statusFilter->addItem(QStringLiteral("待结算"), ecp::ORDER_TO_SETTLE);
    m_statusFilter->addItem(QStringLiteral("已结算"), ecp::ORDER_SETTLED);
    m_statusFilter->addItem(QStringLiteral("已取消"), ecp::ORDER_CANCELLED);
    filterLayout->addWidget(m_statusFilter);

    filterLayout->addSpacing(12);
    filterLayout->addWidget(new QLabel(QStringLiteral("起始日期"), this));
    m_dateFrom = new QDateEdit(this);
    m_dateFrom->setCalendarPopup(true);
    m_dateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    filterLayout->addWidget(m_dateFrom);

    filterLayout->addWidget(new QLabel(QStringLiteral("结束日期"), this));
    m_dateTo = new QDateEdit(this);
    m_dateTo->setCalendarPopup(true);
    m_dateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    filterLayout->addWidget(m_dateTo);

    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    auto *resetButton = new QPushButton(QStringLiteral("重置"), this);
    filterLayout->addWidget(searchButton);
    filterLayout->addWidget(resetButton);
    filterLayout->addStretch();
    pageLayout->addLayout(filterLayout);

    m_table = new QTableWidget(0, 12, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("订单 ID"), QStringLiteral("订单号"), QStringLiteral("用户 ID"),
        QStringLiteral("电站 ID"), QStringLiteral("电桩 ID"), QStringLiteral("状态"),
        QStringLiteral("充电量（kWh）"), QStringLiteral("订单金额（元）"),
        QStringLiteral("预约时间"), QStringLiteral("开始时间"),
        QStringLiteral("结束时间"), QStringLiteral("结算时间")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    pageLayout->addWidget(m_table, 1);

    connect(searchButton, &QPushButton::clicked, this, &OrderPage::refreshTable);
    connect(resetButton, &QPushButton::clicked, this, &OrderPage::resetFilters);
}

void OrderPage::loadMockData()
{
    // Mock 数据集中于此，仅对照 t_order；待 2304 接入后由服务端响应替换。
    m_orders.clear();
    const QDateTime firstReserve(QDate(2026, 8, 5), QTime(8, 0));

    for (int index = 0; index < 25; ++index) {
        const int orderId = 1001 + index;
        const int status = index % 5;
        const int stationId = index % 6 + 1;
        const int pileId = (stationId - 1) * 4 + index % 4 + 1;
        const QDateTime reserve = firstReserve.addDays(index).addSecs((index % 6) * 1800);
        const QString orderNo = QStringLiteral("ORD%1%2")
            .arg(reserve.toString(QStringLiteral("yyyyMMddHHmmss")))
            .arg(orderId, 4, 10, QLatin1Char('0'));

        QString startTime;
        QString endTime;
        QString settleTime;
        int kwhX100 = 0;
        qint64 amountFen = 0;
        const qint64 priceFen = 138 + stationId * 2;

        if (status == ecp::ORDER_CHARGING || status == ecp::ORDER_TO_SETTLE
            || status == ecp::ORDER_SETTLED) {
            const QDateTime start = reserve.addSecs(10 * 60);
            startTime = ecp::toStr(start);
            kwhX100 = 1250 + index * 137;
            amountFen = priceFen * kwhX100 / 100;

            if (status == ecp::ORDER_TO_SETTLE || status == ecp::ORDER_SETTLED) {
                const QDateTime end = start.addSecs(45 * 60 + (index % 4) * 20 * 60);
                endTime = ecp::toStr(end);
                if (status == ecp::ORDER_SETTLED) {
                    settleTime = ecp::toStr(end.addSecs(5 * 60));
                }
            }
        }

        m_orders.append({ orderId, orderNo, index % 14 + 1, pileId, stationId,
                          status, priceFen, kwhX100, amountFen, ecp::toStr(reserve),
                          startTime, endTime, settleTime });
    }

    resetFilters();
}

void OrderPage::refreshTable()
{
    const QDate dateFrom = m_dateFrom->date();
    const QDate dateTo = m_dateTo->date();
    if (dateFrom > dateTo) {
        QMessageBox::warning(this, QStringLiteral("筛选条件有误"),
                             QStringLiteral("起始日期不能晚于结束日期"));
        return;
    }
    const int status = m_statusFilter->currentData().toInt();

    // TODO(L3)：服务端实现后，将本地筛选替换为 2304 请求：
    // {page, size, status, dateFrom, dateTo}。当前不得发送真实网络请求。
    // 本轮 Mock 日期范围按 reserveTime（预约时间）判断。
    m_table->setRowCount(0);
    for (const OrderMock &order : m_orders) {
        if (!matchesFilters(order, status, dateFrom, dateTo)) continue;

        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, centeredItem(QString::number(order.orderId)));
        m_table->setItem(row, 1, centeredItem(order.orderNo));
        m_table->setItem(row, 2, centeredItem(QString::number(order.userId)));
        m_table->setItem(row, 3, centeredItem(QString::number(order.stationId)));
        m_table->setItem(row, 4, centeredItem(QString::number(order.pileId)));
        m_table->setItem(row, 5, centeredItem(statusText(order.status)));
        m_table->setItem(row, 6, centeredItem(kwhText(order.kwhX100)));
        m_table->setItem(row, 7, centeredItem(ecp::fenToYuan(order.amountFen)));
        m_table->setItem(row, 8, centeredItem(timeText(order.reserveTime)));
        m_table->setItem(row, 9, centeredItem(timeText(order.startTime)));
        m_table->setItem(row, 10, centeredItem(timeText(order.endTime)));
        m_table->setItem(row, 11, centeredItem(timeText(order.settleTime)));
    }
}

void OrderPage::resetFilters()
{
    m_statusFilter->setCurrentIndex(0);
    if (m_orders.isEmpty()) {
        m_dateFrom->setDate(QDate::currentDate());
        m_dateTo->setDate(QDate::currentDate());
    } else {
        m_dateFrom->setDate(ecp::fromStr(m_orders.first().reserveTime).date());
        m_dateTo->setDate(ecp::fromStr(m_orders.last().reserveTime).date());
    }
    refreshTable();
}

bool OrderPage::matchesFilters(const OrderMock &order, int status,
                               const QDate &dateFrom, const QDate &dateTo) const
{
    if (status >= 0 && order.status != status) return false;
    const QDate reserveDate = ecp::fromStr(order.reserveTime).date();
    return reserveDate >= dateFrom && reserveDate <= dateTo;
}

QString OrderPage::statusText(int status)
{
    switch (status) {
    case ecp::ORDER_RESERVED:   return QStringLiteral("已预约");
    case ecp::ORDER_CHARGING:   return QStringLiteral("充电中");
    case ecp::ORDER_TO_SETTLE:  return QStringLiteral("待结算");
    case ecp::ORDER_SETTLED:    return QStringLiteral("已结算");
    case ecp::ORDER_CANCELLED:  return QStringLiteral("已取消");
    default:                    return QStringLiteral("未知");
    }
}

QString OrderPage::kwhText(int kwhX100)
{
    return QStringLiteral("%1.%2 kWh")
        .arg(kwhX100 / 100)
        .arg(qAbs(kwhX100 % 100), 2, 10, QLatin1Char('0'));
}

QString OrderPage::timeText(const QString &time)
{
    return time.isEmpty() ? QStringLiteral("—") : time;
}

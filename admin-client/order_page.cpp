#include "order_page.h"
#include "net_client.h"
#include "protocol.h"
#include "time_util.h"
#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QDateEdit>
#include <QDateTime>
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
    connect(m_net, &NetClient::response, this, &OrderPage::handleResponse);
    requestOrderList();
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
    m_dateFilterEnabled = new QCheckBox(QStringLiteral("按日期筛选"), this);
    filterLayout->addWidget(m_dateFilterEnabled);
    filterLayout->addWidget(new QLabel(QStringLiteral("起始日期"), this));
    m_dateFrom = new QDateEdit(this);
    m_dateFrom->setCalendarPopup(true);
    m_dateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateFrom->setDate(QDate::currentDate().addMonths(-1));
    m_dateFrom->setEnabled(false);
    filterLayout->addWidget(m_dateFrom);

    filterLayout->addWidget(new QLabel(QStringLiteral("结束日期"), this));
    m_dateTo = new QDateEdit(this);
    m_dateTo->setCalendarPopup(true);
    m_dateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateTo->setDate(QDate::currentDate());
    m_dateTo->setEnabled(false);
    filterLayout->addWidget(m_dateTo);

    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    auto *resetButton = new QPushButton(QStringLiteral("重置"), this);
    filterLayout->addWidget(searchButton);
    filterLayout->addWidget(resetButton);
    filterLayout->addStretch();
    pageLayout->addLayout(filterLayout);

    m_statusLabel = new QLabel(QStringLiteral("准备加载订单列表"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#667085"));
    pageLayout->addWidget(m_statusLabel);

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

    connect(m_dateFilterEnabled, &QCheckBox::toggled, m_dateFrom, &QWidget::setEnabled);
    connect(m_dateFilterEnabled, &QCheckBox::toggled, m_dateTo, &QWidget::setEnabled);
    connect(searchButton, &QPushButton::clicked, this, &OrderPage::requestOrderList);
    connect(resetButton, &QPushButton::clicked, this, &OrderPage::resetFilters);
}

void OrderPage::requestOrderList()
{
    QString dateFrom;
    QString dateTo;
    if (m_dateFilterEnabled->isChecked()) {
        if (m_dateFrom->date() > m_dateTo->date()) {
            QMessageBox::warning(this, QStringLiteral("筛选条件有误"),
                                 QStringLiteral("起始日期不能晚于结束日期"));
            return;
        }
        dateFrom = ecp::toStr(QDateTime(m_dateFrom->date(), QTime(0, 0, 0)));
        dateTo = ecp::toStr(QDateTime(m_dateTo->date(), QTime(23, 59, 59)));
    }

    m_statusLabel->setText(QStringLiteral("正在加载订单…"));
    const int seq = m_net->send(ecp::CMD_ADMIN_ORDER_LIST, QJsonObject{
        { QStringLiteral("page"), 1 },
        { QStringLiteral("size"), 100 },
        { QStringLiteral("status"), m_statusFilter->currentData().toInt() },
        { QStringLiteral("dateFrom"), dateFrom },
        { QStringLiteral("dateTo"), dateTo }
    });
    if (seq < 0) {
        m_orderListSeq = -1;
        m_statusLabel->setText(QStringLiteral("订单列表请求发送失败，请检查网络连接"));
        return;
    }
    m_orderListSeq = seq;
}

void OrderPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                               const QJsonObject &data)
{
    if (cmd != ecp::CMD_ADMIN_ORDER_LIST || seq != m_orderListSeq) return;
    m_orderListSeq = -1;
    handleOrderListResponse(code, msg, data);
}

void OrderPage::handleOrderListResponse(int code, const QString &msg,
                                        const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_statusLabel->setText(QStringLiteral("订单加载失败：%1").arg(msg));
        return;
    }

    QVector<OrderData> orders;
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
    orders.reserve(list.size());
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        orders.append({
            item.value(QStringLiteral("orderId")).toInteger(),
            item.value(QStringLiteral("orderNo")).toString(),
            item.value(QStringLiteral("userId")).toInteger(),
            item.value(QStringLiteral("stationId")).toInteger(),
            item.value(QStringLiteral("pileId")).toInteger(),
            item.value(QStringLiteral("status")).toInt(),
            item.value(QStringLiteral("kwh")).toDouble(),
            item.value(QStringLiteral("amount")).toInteger(),
            item.value(QStringLiteral("reserveTime")).toString(),
            item.value(QStringLiteral("startTime")).toString(),
            item.value(QStringLiteral("endTime")).toString(),
            item.value(QStringLiteral("settleTime")).toString()
        });
    }

    m_orders = orders;
    refreshTable();
    const qint64 total = data.value(QStringLiteral("total")).toInteger(m_orders.size());
    m_statusLabel->setText(QStringLiteral("已加载 %1 条订单，共 %2 条")
                               .arg(m_orders.size()).arg(total));
}

void OrderPage::refreshTable()
{
    m_table->setRowCount(0);
    for (const OrderData &order : m_orders) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        m_table->setItem(row, 0, centeredItem(QString::number(order.orderId)));
        m_table->setItem(row, 1, centeredItem(order.orderNo));
        m_table->setItem(row, 2, centeredItem(QString::number(order.userId)));
        m_table->setItem(row, 3, centeredItem(QString::number(order.stationId)));
        m_table->setItem(row, 4, centeredItem(QString::number(order.pileId)));
        m_table->setItem(row, 5, centeredItem(statusText(order.status)));
        m_table->setItem(row, 6, centeredItem(kwhText(order.kwh)));
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
    m_dateFilterEnabled->setChecked(false);
    m_dateFrom->setDate(QDate::currentDate().addMonths(-1));
    m_dateTo->setDate(QDate::currentDate());
    requestOrderList();
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

QString OrderPage::kwhText(qreal kwh)
{
    return QStringLiteral("%1 kWh").arg(QString::number(kwh, 'f', 2));
}

QString OrderPage::timeText(const QString &time)
{
    return time.isEmpty() ? QStringLiteral("—") : time;
}

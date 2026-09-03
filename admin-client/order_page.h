#pragma once
// -----------------------------------------------------------------------------
//  admin-client/order_page.h  —  PC 管理端订单管理页
//  归属 L3。Mock 字段仅对照 t_order，不代表最终 2304 list[] 契约。
// -----------------------------------------------------------------------------
#include <QDate>
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QComboBox;
class QDateEdit;
class QTableWidget;

class OrderPage : public QWidget
{
public:
    explicit OrderPage(NetClient *net, QWidget *parent = nullptr);

private:
    struct OrderMock
    {
        int orderId = 0;
        QString orderNo;
        int userId = 0;
        int pileId = 0;
        int stationId = 0;
        int status = 0;
        qint64 priceFen = 0;
        int kwhX100 = 0;
        qint64 amountFen = 0;
        QString reserveTime;
        QString startTime;
        QString endTime;
        QString settleTime;
    };

    void setupUi();
    void loadMockData();
    void refreshTable();
    void resetFilters();
    bool matchesFilters(const OrderMock &order, int status,
                        const QDate &dateFrom, const QDate &dateTo) const;

    static QString statusText(int status);
    static QString kwhText(int kwhX100);
    static QString timeText(const QString &time);

    NetClient    *m_net = nullptr;
    QComboBox    *m_statusFilter = nullptr;
    QDateEdit    *m_dateFrom = nullptr;
    QDateEdit    *m_dateTo = nullptr;
    QTableWidget *m_table = nullptr;
    QVector<OrderMock> m_orders;
};

#pragma once
// -----------------------------------------------------------------------------
//  admin-client/order_page.h  —  PC 管理端订单管理页
//  归属 L3。通过现有 NetClient 查询 2304 管理端订单列表。
// -----------------------------------------------------------------------------
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QCheckBox;
class QComboBox;
class QDateEdit;
class QJsonObject;
class QLabel;
class QTableWidget;

class OrderPage : public QWidget
{
public:
    explicit OrderPage(NetClient *net, QWidget *parent = nullptr);

private:
    struct OrderData
    {
        qint64 orderId = 0;
        QString orderNo;
        qint64 userId = 0;
        qint64 stationId = 0;
        qint64 pileId = 0;
        int status = 0;
        qreal kwh = 0;
        qint64 amountFen = 0;
        QString reserveTime;
        QString startTime;
        QString endTime;
        QString settleTime;
    };

    void setupUi();
    void requestOrderList();
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleOrderListResponse(int code, const QString &msg,
                                 const QJsonObject &data);
    void refreshTable();
    void resetFilters();

    static QString statusText(int status);
    static QString kwhText(qreal kwh);
    static QString timeText(const QString &time);

    NetClient    *m_net = nullptr;
    QComboBox    *m_statusFilter = nullptr;
    QCheckBox    *m_dateFilterEnabled = nullptr;
    QDateEdit    *m_dateFrom = nullptr;
    QDateEdit    *m_dateTo = nullptr;
    QTableWidget *m_table = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QVector<OrderData> m_orders;
    int m_orderListSeq = -1;
};

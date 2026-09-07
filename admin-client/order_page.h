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
class QPushButton;
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
    void requestOrderList(int page, int status, const QString &dateFrom,
                          const QString &dateTo);
    void searchOrders();
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleOrderListResponse(int code, const QString &msg,
                                 const QJsonObject &data);
    void refreshTable();
    void resetFilters();
    void updatePaginationControls();

    static QString statusText(int status);
    static QString kwhText(qreal kwh);
    static QString timeText(const QString &time);

    NetClient    *m_net = nullptr;
    QComboBox    *m_statusFilter = nullptr;
    QCheckBox    *m_dateFilterEnabled = nullptr;
    QDateEdit    *m_dateFrom = nullptr;
    QDateEdit    *m_dateTo = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton  *m_previousPageButton = nullptr;
    QPushButton  *m_nextPageButton = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QLabel       *m_pageLabel = nullptr;
    QVector<OrderData> m_orders;

    static constexpr int PAGE_SIZE = 20;
    int m_currentPage = 1;
    qint64 m_total = 0;
    int m_requestedPage = 1;
    int m_currentStatus = -1;
    QString m_currentDateFrom;
    QString m_currentDateTo;
    int m_requestedStatus = -1;
    QString m_requestedDateFrom;
    QString m_requestedDateTo;
    int m_orderListSeq = -1;
};

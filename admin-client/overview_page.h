#pragma once
// -----------------------------------------------------------------------------
//  admin-client/overview_page.h  —  PC 管理端数据总览页
//  归属 L3。 [说明书] 1.4 销售业绩 + 电桩状态
// -----------------------------------------------------------------------------
#include <QDate>
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QJsonObject;
class QLabel;
class QTableWidget;

#ifdef HAVE_CHARTS
class QChart;
class QDateTimeAxis;
class QLineSeries;
class QValueAxis;
#endif

class OverviewPage : public QWidget
{
public:
    explicit OverviewPage(NetClient *net, QWidget *parent = nullptr);

private:
    enum class LoadState { Idle, Loading, Success, Failed };

#ifdef HAVE_CHARTS
    struct RevenueTrendPoint
    {
        QDate date;
        qint64 amountFen = 0;
    };
#endif

    void setupUi();
    void refreshOverview();
    void requestRevenue();
    void requestRevenueTrend(int days);
    void requestPileStatus();
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleRevenueResponse(int code, const QString &msg,
                               const QJsonObject &data);
    void handlePileStatusResponse(int code, const QString &msg,
                                  const QJsonObject &data);
    void updateStatusLabel();
    QWidget *createMetricCard(const QString &title, QLabel *&valueLabel);
    void updatePileStatusTable(qint64 inUse, qint64 idle, qint64 fault,
                               qint64 total);

#ifdef HAVE_CHARTS
    void handleRevenueTrendResponse(int code, const QString &msg,
                                    const QJsonObject &data);
    void renderRevenueTrend(const QVector<RevenueTrendPoint> &points, int days);

    QChart        *m_revenueChart = nullptr;
    QLineSeries   *m_revenueSeries = nullptr;
    QDateTimeAxis *m_revenueAxisX = nullptr;
    QValueAxis    *m_revenueAxisY = nullptr;
#endif

    NetClient    *m_net = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QLabel       *m_todayRevenue = nullptr;
    QLabel       *m_monthRevenue = nullptr;
    QLabel       *m_totalRevenue = nullptr;
    QLabel       *m_pileTotal = nullptr;
    QTableWidget *m_pileStatusTable = nullptr;

    LoadState m_revenueState = LoadState::Idle;
    LoadState m_revenueTrendState = LoadState::Idle;
    LoadState m_pileStatusState = LoadState::Idle;
    QString m_revenueError;
    QString m_revenueTrendError;
    QString m_pileStatusError;

    int m_revenueSeq = -1;
    int m_revenueTrendSeq = -1;
    int m_pileStatusSeq = -1;
#ifdef HAVE_CHARTS
    int m_currentTrendDays = 7;
#endif
};

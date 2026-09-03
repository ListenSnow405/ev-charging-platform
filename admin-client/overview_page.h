#pragma once
// -----------------------------------------------------------------------------
//  admin-client/overview_page.h  —  PC 管理端数据总览页
//  归属 L3。 [说明书] 1.4 销售业绩 + 电桩状态
// -----------------------------------------------------------------------------
#include <QWidget>

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
    explicit OverviewPage(QWidget *parent = nullptr);

private:
    void setupUi();
    void loadMockData();
    QWidget *createMetricCard(const QString &title, QLabel *&valueLabel);
    void updatePileStatusTable(int inUse, int idle, int fault);

#ifdef HAVE_CHARTS
    void updateRevenueTrend(int days);

    QChart        *m_revenueChart = nullptr;
    QLineSeries   *m_revenueSeries = nullptr;
    QDateTimeAxis *m_revenueAxisX = nullptr;
    QValueAxis    *m_revenueAxisY = nullptr;
#endif

    QLabel       *m_todayRevenue = nullptr;
    QLabel       *m_monthRevenue = nullptr;
    QLabel       *m_totalRevenue = nullptr;
    QLabel       *m_pileTotal = nullptr;
    QTableWidget *m_pileStatusTable = nullptr;
};

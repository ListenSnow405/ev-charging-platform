#pragma once
// -----------------------------------------------------------------------------
//  admin-client/station_page.h  —  PC 管理端充电站管理页
//  归属 L3。 [说明书] 1.4 电站列表、站内详情与新增电站
// -----------------------------------------------------------------------------
#include <QVector>
#include <QString>
#include <QWidget>

class NetClient;
class QPushButton;
class QTableWidget;

class StationPage : public QWidget
{
public:
    explicit StationPage(NetClient *net, QWidget *parent = nullptr);

private:
    struct PileMock
    {
        QString code;
        QString type;
        qreal powerKw = 0;
        QString status;
    };

    struct StationMock
    {
        int stationId = 0;
        QString name;
        QString address;
        qreal longitude = 0;
        qreal latitude = 0;
        qint64 priceFen = 0;
        int pileTotal = 0;
        int onlineCount = 0;
        QVector<PileMock> piles;
    };

    void setupUi();
    void loadMockStations();
    void appendStationRow(const StationMock &station);
    void showSelectedStationDetails();
    void showStationDetails(int row);
    void addStation();
    int nextStationId() const;

    NetClient    *m_net = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton  *m_detailsButton = nullptr;
    QVector<StationMock> m_stations;
};

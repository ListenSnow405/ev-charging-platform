#pragma once
// -----------------------------------------------------------------------------
//  admin-client/station_page.h  —  PC 管理端充电站管理页
//  归属 L3。 [说明书] 1.4 电站列表、站内详情与新增电站
// -----------------------------------------------------------------------------
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QJsonObject;
class QLabel;
class QPushButton;
class QTableWidget;

class StationPage : public QWidget
{
public:
    explicit StationPage(NetClient *net, QWidget *parent = nullptr);

private:
    struct StationData
    {
        qint64 stationId = 0;
        QString name;
        QString address;
        qreal longitude = 0;
        qreal latitude = 0;
        qint64 pileTotal = 0;
        qreal onlineRatePercent = 0;
    };

    struct PileData
    {
        QString code;
        int type = 0;
        int status = 0;
        qreal powerKw = 0;
    };

    void setupUi();
    void requestStationList();
    void requestStationDetail(int row);
    void addStation();
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleStationListResponse(int code, const QString &msg,
                                   const QJsonObject &data);
    void handleStationAddResponse(int code, const QString &msg,
                                  const QJsonObject &data);
    void handleStationDetailResponse(int code, const QString &msg,
                                     const QJsonObject &data);
    void appendStationRow(const StationData &station);
    void showSelectedStationDetails();
    void showStationDetailDialog(const QVector<PileData> &piles);

    static QString typeText(int type);
    static QString statusText(int status);

    NetClient    *m_net = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton  *m_detailsButton = nullptr;
    QPushButton  *m_addButton = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QVector<StationData> m_stations;

    int m_stationListSeq = -1;
    int m_stationAddSeq = -1;
    int m_stationDetailSeq = -1;
    qint64 m_pendingDetailStationId = 0;
    QString m_pendingDetailStationName;
    QString m_pendingDetailStationAddress;
};

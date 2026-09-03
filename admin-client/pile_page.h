#pragma once
// -----------------------------------------------------------------------------
//  admin-client/pile_page.h  —  PC 管理端充电桩管理页
//  归属 L3。 [说明书] 1.4 电桩列表、筛选与远程重启
// -----------------------------------------------------------------------------
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QComboBox;
class QJsonObject;
class QLabel;
class QPushButton;
class QTableWidget;

class PilePage : public QWidget
{
public:
    explicit PilePage(NetClient *net, QWidget *parent = nullptr);

private:
    struct PileData
    {
        qint64 pileId = 0;
        QString code;
        QString stationName;
        int type = 0;
        qreal powerKw = 0;
        int status = 0;
        qint64 chargeCount = 0;
        qint64 chargeDurationSeconds = 0;
    };

    void setupUi();
    void requestStationOptions();
    void requestPileList();
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleStationOptionsResponse(int code, const QString &msg,
                                      const QJsonObject &data);
    void handlePileListResponse(int code, const QString &msg,
                                const QJsonObject &data);
    void refreshTable();
    void resetFilters();

    static QString typeText(int type);
    static QString statusText(int status);
    static QString durationText(qint64 seconds);

    NetClient    *m_net = nullptr;
    QComboBox    *m_stationFilter = nullptr;
    QComboBox    *m_statusFilter = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton  *m_rebootButton = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QVector<PileData> m_piles;

    int m_stationOptionsSeq = -1;
    int m_pileListSeq = -1;
};

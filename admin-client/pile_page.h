#pragma once
// -----------------------------------------------------------------------------
//  admin-client/pile_page.h  —  PC 管理端充电桩管理页
//  归属 L3。 [说明书] 1.4 电桩列表、筛选与远程重启
// -----------------------------------------------------------------------------
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class LoadingStatus;
class QComboBox;
class QJsonObject;
class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

class PilePage : public QWidget
{
public:
    explicit PilePage(NetClient *net, QWidget *parent = nullptr);

private:
    struct StationOption
    {
        qint64 stationId = 0;
        QString name;
    };

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
    void updateLoadingState();
    void requestStationOptions();
    void requestStationOptionsPage(int page);
    void abortStationOptionsLoad(const QString &message);
    void commitStationOptions();
    void requestPileList(int page, qint64 stationId, int status);
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleStationOptionsResponse(int code, const QString &msg,
                                      const QJsonObject &data);
    void handlePileListResponse(int code, const QString &msg,
                                const QJsonObject &data);
    // [说明书] 1.4 远程重启：2112 → 服务端查在线设备 → 9003 下发设备
    void requestPileReboot();
    void handlePileRebootResponse(int code, const QString &msg);
    const PileData *selectedPile() const;
    void updateRebootButton();
    void refreshTable();
    void resetFilters();
    void updatePaginationControls();

    static QString typeText(int type);
    static QString statusText(int status);
    static QString durationText(qint64 seconds);

    NetClient    *m_net = nullptr;
    QComboBox    *m_stationFilter = nullptr;
    QComboBox    *m_statusFilter = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton  *m_rebootButton = nullptr;
    QPushButton  *m_previousPageButton = nullptr;
    QPushButton  *m_nextPageButton = nullptr;
    LoadingStatus *m_statusLabel = nullptr;
    QLabel       *m_pageLabel = nullptr;
    QTimer       *m_stationOptionsTimer = nullptr;
    QTimer       *m_pileListTimer = nullptr;
    QTimer       *m_pileRebootTimer = nullptr;
    QVector<PileData> m_piles;

    static constexpr int PAGE_SIZE = 20;
    static constexpr int STATION_OPTION_PAGE_SIZE = 100;
    int m_currentPage = 1;
    qint64 m_total = 0;
    int m_requestedPage = 1;
    qint64 m_currentStationId = 0;
    int m_currentStatus = -1;
    qint64 m_requestedStationId = 0;
    int m_requestedStatus = -1;
    int m_stationOptionsPage = 0;
    qint64 m_stationOptionsTotal = 0;
    qint64 m_stationOptionsSelectedId = 0;
    QVector<StationOption> m_pendingStationOptions;
    QSet<qint64> m_pendingStationOptionIds;
    int m_stationOptionsSeq = -1;
    int m_pileListSeq = -1;
    int m_pileRebootSeq = -1;
    // 重启请求发出后表格可能被刷新，响应回来时选中行未必还是当初那台桩，
    // 因此把编号在发起时就留下来，用于成功/失败提示。
    QString m_pendingRebootCode;
};

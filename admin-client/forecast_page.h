#pragma once
// -----------------------------------------------------------------------------
//  admin-client/forecast_page.h  —  PC 管理端负荷预测与预警页
//  归属 L3。 [说明书] 1.4 负荷预测、空闲桩预测与高峰预警
// -----------------------------------------------------------------------------
#include <QSet>
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QComboBox;
class QJsonObject;
class QLabel;
class QTableWidget;

class ForecastPage : public QWidget
{
public:
    explicit ForecastPage(NetClient *net, QWidget *parent = nullptr);

private:
    struct StationOption
    {
        qint64 stationId = 0;
        QString name;
    };

    struct ForecastData
    {
        qint64 stationId = 0;
        QString stationName;
        int horizon = 1;
        QString predictTime;
        qreal loadKw = 0;
        qint64 idlePile = 0;
        bool isPeak = false;
        qreal congestion = 0;
        QString modelVersion;
    };

    void setupUi();
    void requestStationList();
    void requestStationListPage(int page);
    void abortStationListLoad(const QString &message);
    void commitStationOptions();
    void requestForecast();
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleStationListResponse(int code, const QString &msg,
                                   const QJsonObject &data);
    void handleForecastResponse(int code, const QString &msg,
                                const QJsonObject &data);
    void refreshTable();
    void updateSummary();
    QWidget *createMetricCard(const QString &title, QLabel *&valueLabel);

    static QString congestionText(qreal congestion);
    static bool parsePeakValue(const QJsonObject &item, bool &isPeak);
    static bool parseForecastItem(const QJsonObject &item, ForecastData &forecast);

    NetClient    *m_net = nullptr;
    QComboBox    *m_stationFilter = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QLabel       *m_stationCount = nullptr;
    QLabel       *m_warningCount = nullptr;
    QLabel       *m_peakCount = nullptr;
    QLabel       *m_maxLoad = nullptr;
    QTableWidget *m_table = nullptr;
    QVector<ForecastData> m_forecasts;

    static constexpr int STATION_OPTION_PAGE_SIZE = 100;
    int m_selectedHorizon = 1;
    int m_stationListPage = 0;
    qint64 m_stationListTotal = 0;
    qint64 m_stationListSelectedId = 0;
    QVector<StationOption> m_pendingStationOptions;
    QSet<qint64> m_pendingStationOptionIds;
    int m_stationListSeq = -1;
    int m_forecastSeq = -1;
};

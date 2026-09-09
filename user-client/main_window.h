#pragma once
// -----------------------------------------------------------------------------
//  user-client/main_window.h  鈥? 鍏呯數鐢ㄦ埛绔富绐楀彛
//  褰掑睘 L4銆俒璇存槑涔 1.4 妯℃嫙鎵嬫満绔氦浜掍綋楠?
// -----------------------------------------------------------------------------
#include <QWidget>
#include <QTabWidget>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QPushButton>
#include <QTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QPair>
#include "net_client.h"

class QNetworkAccessManager;
class QNetworkReply;

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(NetClient *net, QWidget *parent = nullptr);

signals:
    void logoutRequested();

private slots:
    void onNetResponse(int cmd, int seq, int code, const QString &msg, const QJsonObject &data);
    void onNetDisconnected();
    void refreshProfile();
    void editNickname();
    void editAvatar();
    void recharge();
    void logout();

private:
    QWidget *makePlaceholder(const QString &title, const QString &todo);
    QWidget *makeNearbyPage();
    QWidget *makeNavPage();          // [说明书] 1.4 一键导航（QWebEngineView）
    QWidget *makeChargePage();
    QWidget *makeMinePage();

    void requestNearbyStations();
    void sendNearbyStationsRequest(const QString &keyword);
    void requestStationPiles(qint64 stationId);
    void requestChargeUnfinishedOrder();
    void requestChargeOrders();
    void sendChargeReserveRequest(qint64 pileId);
    void reserveChargePile(qint64 pileId);
    void startChargeOrder();
    void stopChargeOrder();
    void settleChargeOrder();
    void renderNearbyStations();
    void renderStationPiles();
    void renderChargeStations();
    void renderChargePiles();
    void renderChargeOrders();
    void requestProfile();
    void applyProfile(const QJsonObject &data);
    void setStatus(const QString &text, bool isError = false);
    void refreshAvatarBadge();
    void updateMineTexts();
    void updateChargeSummary();
    void setChargeOrder(const QJsonObject &order);
    void clearChargeOrder();
    QString chargeOrderStatusText(int status) const;
    QString chargeStageText() const;
    QString profileName() const;
    QString profilePhone() const;
    QString balanceText() const;

    NetClient   *m_net = nullptr;
    QNetworkAccessManager *m_mapNetwork = nullptr;
    QNetworkReply *m_pendingGeocodeReply = nullptr;
    QTabWidget  *m_tabs = nullptr;
    QLabel      *m_status = nullptr;
    QLabel      *m_avatar = nullptr;
    QLabel      *m_name = nullptr;
    QLabel      *m_phone = nullptr;
    QLabel      *m_balance = nullptr;
    QLineEdit   *m_navDestEdit = nullptr;
    QLineEdit   *m_nearbySearch = nullptr;
    QComboBox   *m_nearbySort = nullptr;
    QLabel      *m_nearbySummary = nullptr;
    QLabel      *m_nearbyStatus = nullptr;
    QLabel      *m_nearbyDetailTitle = nullptr;
    QLabel      *m_nearbyDetailMeta = nullptr;
    QLabel      *m_nearbyPileStatus = nullptr;
    QScrollArea *m_nearbyScroll = nullptr;
    QWidget     *m_nearbyCardsHost = nullptr;
    QVBoxLayout *m_nearbyCardsLay = nullptr;
    QTableWidget *m_nearbyPileTable = nullptr;
    QComboBox   *m_chargeStationCombo = nullptr;
    QLabel      *m_chargeStatus = nullptr;
    QLabel      *m_chargeStage = nullptr;
    QLabel      *m_chargeOrderMeta = nullptr;
    QLabel      *m_chargeOrderMoney = nullptr;
    QLabel      *m_chargeOrderTime = nullptr;
    QLabel      *m_chargeOrderPrice = nullptr;
    QLabel      *m_chargeHint = nullptr;
    QTableWidget *m_chargePileTable = nullptr;
    QTableWidget *m_chargeOrderTable = nullptr;
    QPushButton *m_chargeRefreshBtn = nullptr;
    QPushButton *m_chargeReserveBtn = nullptr;
    QPushButton *m_chargeStartBtn = nullptr;
    QPushButton *m_chargeStopBtn = nullptr;
    QPushButton *m_chargeSettleBtn = nullptr;
    QPushButton *m_chargeCancelBtn = nullptr;
    QPushButton *m_nearbyRefreshBtn = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_rechargeBtn = nullptr;
    QPushButton *m_nicknameBtn = nullptr;
    QPushButton *m_avatarBtn = nullptr;
    QPushButton *m_logoutBtn = nullptr;
    QTimer       *m_chargeSummaryTimer = nullptr;
    QJsonObject  m_profile;
    QJsonArray   m_nearbyStations;
    QJsonArray   m_nearbyPiles;
    qint64       m_selectedNearbyStationId = -1;
    qint64       m_pendingNearbyStationsSeq = -1;
    qint64       m_pendingNearbyPilesSeq = -1;
    qint64       m_pendingChargeUnfinishedSeq = -1;
    qint64       m_pendingChargeOrdersSeq = -1;
    qint64       m_pendingChargeReserveSeq = -1;
    qint64       m_pendingChargeStartSeq = -1;
    qint64       m_pendingChargeStopSeq = -1;
    qint64       m_pendingChargeSettleSeq = -1;
    qint64       m_pendingChargeCancelSeq = -1;
    qint64       m_selectedChargePileId = -1;
    qint64       m_pendingChargeReservePileId = -1;
    qint64       m_chargeOrderId = -1;
    bool         m_suppressChargeUnfinishedPrompt = false;
    QString      m_selectedNearbyStationName;
    QString      m_nearbyLocationText;
    QString      m_mapKey;
    QHash<QString, QPair<double, double>> m_nearbyGeocodeCache;
    double       m_mapDefaultLat = 22.5470;
    double       m_mapDefaultLng = 114.0650;
    QJsonObject  m_chargeOrder;
    QJsonArray   m_chargeOrders;
};

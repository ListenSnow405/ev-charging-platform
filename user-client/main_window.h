#pragma once
// -----------------------------------------------------------------------------
//  user-client/main_window.h  —  充电用户端主窗口
//  归属 L4。[说明书] 1.4 模拟手机端交互体验
// -----------------------------------------------------------------------------
#include <QWidget>
#include <QTabWidget>
#include <QLabel>
#include <QPushButton>
#include <QJsonObject>
#include "net_client.h"

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

    void requestProfile();
    void applyProfile(const QJsonObject &data);
    void setStatus(const QString &text, bool isError = false);
    void refreshAvatarBadge();
    void updateMineTexts();
    QString profileName() const;
    QString profilePhone() const;
    QString balanceText() const;

    NetClient   *m_net = nullptr;
    QTabWidget  *m_tabs = nullptr;
    QLabel      *m_status = nullptr;
    QLabel      *m_avatar = nullptr;
    QLabel      *m_name = nullptr;
    QLabel      *m_phone = nullptr;
    QLabel      *m_balance = nullptr;
    QPushButton *m_refreshBtn = nullptr;
    QPushButton *m_rechargeBtn = nullptr;
    QPushButton *m_nicknameBtn = nullptr;
    QPushButton *m_avatarBtn = nullptr;
    QPushButton *m_logoutBtn = nullptr;
    QJsonObject  m_profile;
};

#pragma once
// -----------------------------------------------------------------------------
//  admin-client/login_window.h  —  管理员登录
//  归属 L3。 [说明书] 1.4 默认初始账号 admin / 123456
// -----------------------------------------------------------------------------
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QString>
#include <QtGlobal>
#include "net_client.h"

class QTimer;

class LoginWindow : public QWidget
{
    Q_OBJECT
public:
    explicit LoginWindow(QWidget *parent = nullptr);

private slots:
    void onLogin();
    void handleReloginRequested(const QString &reason);

private:
    void beginReconnect(const QString &context);
    void attemptReconnect();

    NetClient   *m_net = nullptr;
    QLineEdit   *m_account = nullptr;
    QLineEdit   *m_password = nullptr;
    QPushButton *m_btn = nullptr;
    QLabel      *m_status = nullptr;
    QTimer      *m_reconnectTimer = nullptr;
    QString      m_host = QStringLiteral("127.0.0.1");
    QString      m_reloginReason;
    QString      m_reconnectContext;
    quint16      m_port = 9527;
    int          m_loginSeq = -1;
    int          m_reconnectAttempts = 0;
    bool         m_reconnectActive = false;
    bool         m_reconnectExhausted = false;
};

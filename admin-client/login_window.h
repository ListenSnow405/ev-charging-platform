#pragma once
// -----------------------------------------------------------------------------
//  admin-client/login_window.h  —  管理员登录（骨架）
//  归属 L3。 [说明书] 1.4 默认初始账号 admin / 123456
// -----------------------------------------------------------------------------
#include <QWidget>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include "net_client.h"

class LoginWindow : public QWidget
{
    Q_OBJECT
public:
    explicit LoginWindow(QWidget *parent = nullptr);

private slots:
    void onLogin();

private:
    NetClient   *m_net = nullptr;
    QLineEdit   *m_account = nullptr;
    QLineEdit   *m_password = nullptr;
    QPushButton *m_btn = nullptr;
    QLabel      *m_status = nullptr;
};

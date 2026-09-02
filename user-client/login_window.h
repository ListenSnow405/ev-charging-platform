#pragma once
// -----------------------------------------------------------------------------
//  user-client/login_window.h  —  手机号免密登录（骨架）
//  归属 L4。 [说明书] 1.4 输入 11 位手机号；存在即登录，不存在自动注册
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
    QLineEdit   *m_phone = nullptr;
    QPushButton *m_btn = nullptr;
    QLabel      *m_status = nullptr;
};

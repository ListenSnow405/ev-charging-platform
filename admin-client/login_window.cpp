#include "login_window.h"
#include "main_window.h"
#include "protocol.h"
#include "app_path.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSettings>
#include <QFileInfo>

LoginWindow::LoginWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("充电桩运营管理后台 · 登录"));
    resize(380, 240);

    m_account  = new QLineEdit(QStringLiteral("admin"), this);
    m_password = new QLineEdit(QStringLiteral("123456"), this);
    m_password->setEchoMode(QLineEdit::Password);
    m_btn      = new QPushButton(QStringLiteral("登录"), this);
    m_status   = new QLabel(QStringLiteral("正在连接服务器…"), this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color:#888"));

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("账号"), m_account);
    form->addRow(QStringLiteral("密码"), m_password);
    auto *lay = new QVBoxLayout(this);
    lay->addLayout(form);
    lay->addWidget(m_btn);
    lay->addWidget(m_status);
    lay->addStretch();

    m_net = new NetClient(this);

    QString host = QStringLiteral("127.0.0.1");
    quint16 port = 9527;
    const QString cfgPath = ecp::resPath(QStringLiteral("config/app.ini"));
    if (QFileInfo::exists(cfgPath)) {
        QSettings cfg(cfgPath, QSettings::IniFormat);
        host = cfg.value(QStringLiteral("server/host"), host).toString();
        port = static_cast<quint16>(cfg.value(QStringLiteral("server/port"), port).toUInt());
    }

    connect(m_net, &NetClient::connected, this, [this] {
        m_status->setText(QStringLiteral("已连接服务器"));
        m_net->send(0);                       // 探针，确认链路
    });
    connect(m_net, &NetClient::errorText, this, [this](const QString &s) { m_status->setText(s); });
    connect(m_net, &NetClient::response, this,
            [this](int cmd, int, int code, const QString &msg, const QJsonObject &) {
        if (cmd == 0 && code == ecp::ERR_OK) { m_status->setText(QStringLiteral("链路正常，可以登录")); return; }
        if (cmd == ecp::CMD_ADMIN_LOGIN) {
            if (code != ecp::ERR_OK) { m_status->setText(msg); return; }
            // TODO(L3)：保存 token，切换到主窗口
            auto *w = new MainWindow(m_net);
            w->setAttribute(Qt::WA_DeleteOnClose);
            w->show();
            close();
        }
    });
    connect(m_btn, &QPushButton::clicked, this, &LoginWindow::onLogin);

    m_net->connectToServer(host, port);
}

void LoginWindow::onLogin()
{
    // TODO(L3)：接入命令字 2001。服务端 handler 由 L2 实现后即可打通。
    // 当前 handler 未注册，服务端会返回 ERR_CMD_UNKNOWN(1005)，属预期行为。
    m_status->setText(QStringLiteral("发起登录…（业务 handler 待 L2 实现）"));
    m_net->send(ecp::CMD_ADMIN_LOGIN, QJsonObject{
        {"account",  m_account->text()},
        {"password", m_password->text()}   // TODO(L3)：改为 SHA-256 摘要后再发送
    });
}

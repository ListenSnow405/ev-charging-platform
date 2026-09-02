#include "login_window.h"
#include "main_window.h"
#include "protocol.h"
#include "app_path.h"
#include <QVBoxLayout>
#include <QFont>
#include <QSettings>
#include <QFileInfo>

LoginWindow::LoginWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("充电用户端"));
    resize(430, 620);

    auto *title = new QLabel(QStringLiteral("充电用户端"), this);
    QFont f = title->font(); f.setPointSize(20); f.setBold(true); title->setFont(f);
    title->setAlignment(Qt::AlignCenter);
    auto *sub = new QLabel(QStringLiteral("手机号免密登录 · 首次登录自动注册"), this);
    sub->setAlignment(Qt::AlignCenter);
    sub->setStyleSheet(QStringLiteral("color:#888"));

    m_phone = new QLineEdit(this);
    m_phone->setPlaceholderText(QStringLiteral("请输入 11 位手机号"));
    m_phone->setMaxLength(11);
    m_btn    = new QPushButton(QStringLiteral("登录"), this);
    m_status = new QLabel(QStringLiteral("正在连接服务器…"), this);
    m_status->setWordWrap(true);
    m_status->setStyleSheet(QStringLiteral("color:#888"));

    auto *demo = new QLabel(QStringLiteral(
        "演示账号：13800138001(有余额) / 13800138002\n13800138004(低余额) / 13800138006(已冻结)"), this);
    demo->setAlignment(Qt::AlignCenter);
    demo->setStyleSheet(QStringLiteral("color:#aaa;font-size:11px"));

    auto *lay = new QVBoxLayout(this);
    lay->addStretch();
    lay->addWidget(title); lay->addWidget(sub);
    lay->addSpacing(24);
    lay->addWidget(m_phone); lay->addWidget(m_btn);
    lay->addWidget(m_status); lay->addWidget(demo);
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
        m_status->setText(QStringLiteral("已连接服务器")); m_net->send(0); });
    connect(m_net, &NetClient::errorText, this, [this](const QString &s) { m_status->setText(s); });
    connect(m_net, &NetClient::response, this,
            [this](int cmd, int, int code, const QString &msg, const QJsonObject &) {
        if (cmd == 0 && code == ecp::ERR_OK) { m_status->setText(QStringLiteral("链路正常，可以登录")); return; }
        if (cmd == ecp::CMD_USER_LOGIN) {
            if (code != ecp::ERR_OK) { m_status->setText(msg); return; }
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
    const QString phone = m_phone->text().trimmed();
    // [说明书] 1.4 11 位手机号 —— 本地先校验，避免无谓的往返
    if (phone.size() != 11) {
        m_status->setText(ecp::errMsg(ecp::ERR_PHONE_FORMAT));
        return;
    }
    // TODO(L4)：命令字 1001，服务端 handler 由 L2 实现后即可打通
    m_status->setText(QStringLiteral("发起登录…（业务 handler 待 L2 实现）"));
    m_net->send(ecp::CMD_USER_LOGIN, QJsonObject{{"phone", phone}});
}

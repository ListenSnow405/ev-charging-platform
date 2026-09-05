#include "login_window.h"

#include "main_window.h"
#include "protocol.h"
#include "app_path.h"

#include <QFileInfo>
#include <QFont>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QVBoxLayout>

LoginWindow::LoginWindow(QWidget *parent) : QWidget(parent)
{
    setWindowTitle(QStringLiteral("充电用户端"));
    resize(430, 620);
    setMinimumSize(390, 560);
    setStyleSheet(QStringLiteral(
        "QWidget { background: #f5f7fb; color: #111827; }"
        "QFrame#Card { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 16px; }"
        "QLabel#Title { font-size: 28px; font-weight: 700; color: #111827; }"
        "QLabel#Subtitle { color: #6b7280; font-size: 13px; }"
        "QLabel#Status { color: #4b5563; }"
        "QLabel#Hint { color: #6b7280; font-size: 12px; }"
        "QLineEdit { background: #ffffff; border: 1px solid #d1d5db; border-radius: 12px; padding: 12px 14px; font-size: 15px; }"
        "QLineEdit:focus { border-color: #2563eb; }"
        "QPushButton#Primary { background: #2563eb; color: #ffffff; border: none; border-radius: 12px; padding: 12px 16px; font-size: 15px; font-weight: 600; }"
        "QPushButton#Primary:hover { background: #1d4ed8; }"
        "QPushButton#Primary:disabled { background: #93c5fd; }"));

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(22, 20, 22, 20);
    outer->setSpacing(16);

    auto *title = new QLabel(QStringLiteral("充电用户端"), this);
    title->setObjectName(QStringLiteral("Title"));
    title->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    auto *subtitle = new QLabel(QStringLiteral("手机号免密登录，首次登录自动注册"), this);
    subtitle->setObjectName(QStringLiteral("Subtitle"));

    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("Card"));
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(18, 18, 18, 18);
    cardLay->setSpacing(12);

    auto *cardTitle = new QLabel(QStringLiteral("登录"), card);
    QFont cardTitleFont = cardTitle->font();
    cardTitleFont.setPointSize(17);
    cardTitleFont.setBold(true);
    cardTitle->setFont(cardTitleFont);

    auto *cardTip = new QLabel(QStringLiteral("输入 11 位手机号即可进入系统。未注册手机号会自动创建用户。"), card);
    cardTip->setWordWrap(true);
    cardTip->setObjectName(QStringLiteral("Hint"));

    m_phone = new QLineEdit(card);
    m_phone->setPlaceholderText(QStringLiteral("请输入 11 位手机号"));
    m_phone->setMaxLength(11);
    m_phone->setValidator(new QRegularExpressionValidator(
        QRegularExpression(QStringLiteral("^[0-9]{0,11}$")), m_phone));

    m_btn = new QPushButton(QStringLiteral("登录"), card);
    m_btn->setObjectName(QStringLiteral("Primary"));

    m_status = new QLabel(QStringLiteral("正在连接服务器..."), card);
    m_status->setObjectName(QStringLiteral("Status"));
    m_status->setWordWrap(true);

    auto *hint = new QLabel(QStringLiteral("示例账号：13800138001 / 13800138002 / 13800138004"), card);
    hint->setObjectName(QStringLiteral("Hint"));
    hint->setWordWrap(true);

    cardLay->addWidget(cardTitle);
    cardLay->addWidget(cardTip);
    cardLay->addSpacing(4);
    cardLay->addWidget(m_phone);
    cardLay->addWidget(m_btn);
    cardLay->addWidget(m_status);
    cardLay->addWidget(hint);

    outer->addWidget(title);
    outer->addWidget(subtitle);
    outer->addSpacing(6);
    outer->addWidget(card);
    outer->addStretch();

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
        m_status->setText(QStringLiteral("已连接服务器，可以登录"));
        m_btn->setEnabled(true);
    });
    connect(m_net, &NetClient::disconnected, this, [this] {
        m_status->setText(QStringLiteral("与服务器断开连接"));
        m_btn->setEnabled(true);
    });
    connect(m_net, &NetClient::errorText, this, [this](const QString &s) {
        m_status->setText(s);
        m_btn->setEnabled(true);
    });
    connect(m_net, &NetClient::response, this,
            [this](int cmd, int, int code, const QString &msg, const QJsonObject &data) {
        if (cmd == 0) {
            if (code == ecp::ERR_OK) {
                m_status->setText(QStringLiteral("链路正常，可以登录"));
            }
            return;
        }

        if (cmd != ecp::CMD_USER_LOGIN) {
            return;
        }

        m_btn->setEnabled(true);
        if (code != ecp::ERR_OK) {
            m_status->setText(msg);
            return;
        }

        const QString token = data.value(QStringLiteral("token")).toString();
        if (token.isEmpty()) {
            m_status->setText(QStringLiteral("登录成功，但未拿到 token"));
            return;
        }

        m_net->setToken(token);
        m_status->setText(QStringLiteral("登录成功，正在进入首页"));

        auto *w = new MainWindow(m_net);
        w->setAttribute(Qt::WA_DeleteOnClose);
        connect(w, &MainWindow::logoutRequested, this, [this] {
            m_net->setToken(QString());
            m_phone->clear();
            m_status->setText(QStringLiteral("登录已失效，请重新登录"));
            show();
            raise();
            activateWindow();
            m_phone->setFocus();
        });
        w->show();
        close();
    });

    connect(m_btn, &QPushButton::clicked, this, &LoginWindow::onLogin);
    connect(m_phone, &QLineEdit::returnPressed, this, &LoginWindow::onLogin);

    m_btn->setEnabled(false);
    m_net->connectToServer(host, port);
}

void LoginWindow::onLogin()
{
    const QString phone = m_phone->text().trimmed();
    const QRegularExpression re(QStringLiteral("^[0-9]{11}$"));
    if (!re.match(phone).hasMatch()) {
        m_status->setText(ecp::errMsg(ecp::ERR_PHONE_FORMAT));
        return;
    }

    m_btn->setEnabled(false);
    m_status->setText(QStringLiteral("正在登录..."));
    m_net->send(ecp::CMD_USER_LOGIN, QJsonObject{{QStringLiteral("phone"), phone}});
}

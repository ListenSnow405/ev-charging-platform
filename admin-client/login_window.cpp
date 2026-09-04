#include "login_window.h"
#include "main_window.h"
#include "protocol.h"
#include "app_path.h"
#include <QVBoxLayout>
#include <QFormLayout>
#include <QSettings>
#include <QFileInfo>
#include <QTimer>

namespace {

constexpr int RECONNECT_DELAY_MS = 2000;
constexpr int MAX_RECONNECT_ATTEMPTS = 30;

} // namespace

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
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout,
            this, &LoginWindow::attemptReconnect);

    const QString cfgPath = ecp::resPath(QStringLiteral("config/app.ini"));
    if (QFileInfo::exists(cfgPath)) {
        QSettings cfg(cfgPath, QSettings::IniFormat);
        m_host = cfg.value(QStringLiteral("server/host"), m_host).toString();
        m_port = static_cast<quint16>(
            cfg.value(QStringLiteral("server/port"), m_port).toUInt());
    }

    connect(m_net, &NetClient::connected, this, [this] {
        m_reconnectTimer->stop();
        m_reconnectActive = false;
        m_reconnectExhausted = false;
        m_reconnectAttempts = 0;
        m_btn->setEnabled(m_loginSeq < 0);
        if (isVisible()) {
            m_status->setText(m_reloginReason.isEmpty()
                ? QStringLiteral("已连接服务器")
                : QStringLiteral("%1\n已重新连接服务器，可以重新登录")
                      .arg(m_reloginReason));
        }
        m_net->send(0);                       // 探针，确认链路
    });
    connect(m_net, &NetClient::disconnected, this, [this] {
        m_loginSeq = -1;
        if (!isVisible()) return;
        if (m_reconnectExhausted) {
            m_btn->setEnabled(true);
            return;
        }
        if (!m_reconnectActive)
            beginReconnect(QStringLiteral("服务器连接已断开"));
    });
    connect(m_net, &NetClient::errorText, this, [this](const QString &s) {
        if (!isVisible()) return;
        if (m_net->isConnected() || !m_reconnectActive) {
            if (m_loginSeq < 0 && !m_net->isConnected()) m_btn->setEnabled(true);
            m_status->setText(s);
            return;
        }

        if (m_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
            m_reconnectActive = false;
            m_reconnectExhausted = true;
            m_btn->setEnabled(true);
            m_status->setText(QStringLiteral(
                "%1\n自动重连未成功，请确认服务端已启动后点击登录重试")
                                  .arg(m_reconnectContext));
            return;
        }

        m_status->setText(QStringLiteral("%1\n连接失败，2 秒后自动重试")
                              .arg(m_reconnectContext));
        m_reconnectTimer->start(RECONNECT_DELAY_MS);
    });
    connect(m_net, &NetClient::response, this,
            [this](int cmd, int seq, int code, const QString &msg,
                   const QJsonObject &data) {
        if (cmd == 0 && code == ecp::ERR_OK) {
            if (isVisible()) {
                m_status->setText(m_reloginReason.isEmpty()
                    ? QStringLiteral("链路正常，可以登录")
                    : QStringLiteral("%1\n链路正常，可以重新登录")
                          .arg(m_reloginReason));
            }
            return;
        }
        if (cmd != ecp::CMD_ADMIN_LOGIN || seq != m_loginSeq) return;

        m_loginSeq = -1;
        m_btn->setEnabled(true);
        if (code != ecp::ERR_OK) {
            m_status->setText(msg);
            return;
        }

        const QString token = data.value(QStringLiteral("token")).toString();
        if (token.isEmpty()) {
            m_status->setText(QStringLiteral("登录响应异常：服务端未返回 token，请稍后重试"));
            return;
        }

        m_reloginReason.clear();
        m_net->setToken(token);
        auto *w = new MainWindow(m_net);
        w->setAttribute(Qt::WA_DeleteOnClose);
        connect(w, &MainWindow::reloginRequested,
                this, &LoginWindow::handleReloginRequested);
        w->show();
        hide();
    });
    connect(m_btn, &QPushButton::clicked, this, &LoginWindow::onLogin);

    beginReconnect(QStringLiteral("正在连接服务器…"));
}

void LoginWindow::onLogin()
{
    if (m_loginSeq >= 0) return;

    if (!m_net->isConnected()) {
        beginReconnect(QStringLiteral("正在重新连接服务器，请连接成功后重试登录"));
        return;
    }

    m_reloginReason.clear();
    m_btn->setEnabled(false);
    m_status->setText(QStringLiteral("发起登录…"));
    const int seq = m_net->send(ecp::CMD_ADMIN_LOGIN, QJsonObject{
        {"account",  m_account->text()},
        // 协议约定发送明文密码，由服务端执行 SHA-256 后与数据库摘要比对。
        {"password", m_password->text()}
    });
    if (seq < 0) {
        m_loginSeq = -1;
        m_btn->setEnabled(true);
        m_status->setText(QStringLiteral("登录请求发送失败，请检查服务器连接"));
        return;
    }
    m_loginSeq = seq;
}

void LoginWindow::handleReloginRequested(const QString &reason)
{
    m_loginSeq = -1;
    m_btn->setEnabled(true);
    m_net->setToken(QString());
    m_reloginReason = reason.trimmed().isEmpty()
        ? QStringLiteral("登录已失效，请重新登录")
        : reason;

    show();
    raise();
    activateWindow();

    if (!m_net->isConnected()) {
        beginReconnect(QStringLiteral("%1\n正在重新连接服务器…")
                           .arg(m_reloginReason));
        return;
    }
    m_status->setText(m_reloginReason);
}

void LoginWindow::beginReconnect(const QString &context)
{
    m_reconnectTimer->stop();
    m_reconnectContext = context;
    m_reconnectAttempts = 0;
    m_reconnectActive = true;
    m_reconnectExhausted = false;
    m_btn->setEnabled(false);
    m_status->setText(context);
    m_reconnectTimer->start(0);
}

void LoginWindow::attemptReconnect()
{
    if (!m_reconnectActive || m_net->isConnected()) return;
    if (m_reconnectAttempts >= MAX_RECONNECT_ATTEMPTS) {
        m_reconnectActive = false;
        m_reconnectExhausted = true;
        m_btn->setEnabled(true);
        m_status->setText(QStringLiteral(
            "%1\n自动重连未成功，请确认服务端已启动后点击登录重试")
                              .arg(m_reconnectContext));
        return;
    }

    ++m_reconnectAttempts;
    m_status->setText(QStringLiteral("%1\n正在自动重连（%2/%3）…")
                          .arg(m_reconnectContext)
                          .arg(m_reconnectAttempts)
                          .arg(MAX_RECONNECT_ATTEMPTS));
    m_net->connectToServer(m_host, m_port);
}

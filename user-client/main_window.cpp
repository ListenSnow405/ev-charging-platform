#include "main_window.h"

#include "protocol.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSizePolicy>
#include <QFont>
#include <QVBoxLayout>

#ifdef HAVE_WEBENGINE
#include <QWebEngineView>
#endif

namespace {
static QString textOrEmpty(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toString();
}
}

MainWindow::MainWindow(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setWindowTitle(QStringLiteral("充电用户端"));
    resize(430, 780);
    setMinimumSize(390, 680);
    setStyleSheet(QStringLiteral(
        "QWidget { background: #f5f7fb; color: #111827; }"
        "QFrame#Card { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 16px; }"
        "QFrame#Hero { background: #ffffff; border: 1px solid #e5e7eb; border-radius: 18px; }"
        "QLabel#HeroTitle { font-size: 18px; font-weight: 700; }"
        "QLabel#Muted { color: #6b7280; }"
        "QLabel#Badge { background: #e8efff; color: #1d4ed8; border-radius: 12px; padding: 4px 10px; }"
        "QPushButton { background: #ffffff; color: #111827; border: 1px solid #d1d5db; border-radius: 10px; padding: 10px 14px; }"
        "QPushButton:hover { border-color: #94a3b8; }"
        "QPushButton#Primary { background: #2563eb; color: #ffffff; border: none; }"
        "QPushButton#Primary:hover { background: #1d4ed8; }"
        "QPushButton#Danger { background: #fee2e2; color: #b91c1c; border: none; }"
        "QPushButton#Danger:hover { background: #fecaca; }"
        "QTabWidget::pane { border: none; }"
        "QTabBar::tab { background: #ffffff; border: 1px solid #d1d5db; padding: 10px 12px; min-width: 80px; }"
        "QTabBar::tab:selected { background: #2563eb; color: #ffffff; border-color: #2563eb; }"));

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(makeNearbyPage(), QStringLiteral("找桩"));
    m_tabs->addTab(makeNavPage(), QStringLiteral("导航"));
    m_tabs->addTab(makeChargePage(), QStringLiteral("充电"));
    m_tabs->addTab(makeMinePage(), QStringLiteral("我的"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_tabs);

    connect(m_net, &NetClient::response, this, &MainWindow::onNetResponse);
    connect(m_net, &NetClient::disconnected, this, &MainWindow::onNetDisconnected);

    requestProfile();
}

QWidget *MainWindow::makePlaceholder(const QString &title, const QString &todo)
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 18, 18, 18);
    auto *card = new QFrame(w);
    card->setObjectName(QStringLiteral("Card"));
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(18, 18, 18, 18);
    auto *t = new QLabel(title, card);
    QFont f = t->font();
    f.setPointSize(16);
    f.setBold(true);
    t->setFont(f);
    auto *d = new QLabel(todo, card);
    d->setWordWrap(true);
    d->setObjectName(QStringLiteral("Muted"));
    cardLay->addWidget(t);
    cardLay->addWidget(d);
    cardLay->addStretch();
    lay->addWidget(card);
    lay->addStretch();
    return w;
}

QWidget *MainWindow::makeNearbyPage()
{
    return makePlaceholder(QStringLiteral("附近充电站"),
                           QStringLiteral("后续接入区域定位、距离排序和充电站卡片。当前先保留页面骨架，方便联调主流程。"));
}

QWidget *MainWindow::makeNavPage()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 18, 18, 18);
#ifdef HAVE_WEBENGINE
    auto *view = new QWebEngineView(w);
    view->setHtml(QStringLiteral(
        "<html><body style='font-family:sans-serif;padding:20px;color:#111827'>"
        "<h3>导航页</h3>"
        "<p>后续这里会加载腾讯地图路线规划页。</p>"
        "<p style='color:#6b7280'>地图 Key 从 config/app.ini 读取。</p>"
        "</body></html>"));
    lay->addWidget(view, 1);
#else
    lay->addWidget(makePlaceholder(QStringLiteral("一键导航"),
                                   QStringLiteral("QtWebEngineWidgets 未安装时显示占位页。后续接入腾讯地图路线规划。")), 1);
#endif
    return w;
}

QWidget *MainWindow::makeChargePage()
{
    return makePlaceholder(QStringLiteral("充电"),
                           QStringLiteral("进入前会先查未结算订单。后续接入预约、开始、计费、结算流程。"));
}

QWidget *MainWindow::makeMinePage()
{
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 18, 18, 18);
    lay->setSpacing(14);

    auto *hero = new QFrame(w);
    hero->setObjectName(QStringLiteral("Hero"));
    auto *heroLay = new QVBoxLayout(hero);
    heroLay->setContentsMargins(18, 18, 18, 18);
    heroLay->setSpacing(12);

    auto *topRow = new QHBoxLayout;
    m_avatar = new QLabel(QStringLiteral("用"), hero);
    m_avatar->setFixedSize(56, 56);
    m_avatar->setAlignment(Qt::AlignCenter);
    m_avatar->setStyleSheet(QStringLiteral(
        "background:#e8efff;color:#1d4ed8;border-radius:28px;font-size:22px;font-weight:700;"));

    auto *nameBlock = new QVBoxLayout;
    m_name = new QLabel(QStringLiteral("未登录"), hero);
    m_name->setObjectName(QStringLiteral("HeroTitle"));
    QFont nameFont = m_name->font();
    nameFont.setPointSize(18);
    nameFont.setBold(true);
    m_name->setFont(nameFont);
    m_phone = new QLabel(QStringLiteral("手机号：--"), hero);
    m_phone->setObjectName(QStringLiteral("Muted"));
    nameBlock->addWidget(m_name);
    nameBlock->addWidget(m_phone);

    topRow->addWidget(m_avatar, 0, Qt::AlignTop);
    topRow->addSpacing(12);
    topRow->addLayout(nameBlock);
    topRow->addStretch();

    m_balance = new QLabel(QStringLiteral("余额：¥ 0.00"), hero);
    QFont balanceFont = m_balance->font();
    balanceFont.setPointSize(20);
    balanceFont.setBold(true);
    m_balance->setFont(balanceFont);

    auto *badge = new QLabel(QStringLiteral("账户中心"), hero);
    badge->setObjectName(QStringLiteral("Badge"));
    badge->setAlignment(Qt::AlignCenter);
    badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    heroLay->addLayout(topRow);
    heroLay->addWidget(m_balance);
    heroLay->addWidget(badge, 0, Qt::AlignLeft);

    auto *buttonCard = new QFrame(w);
    buttonCard->setObjectName(QStringLiteral("Card"));
    auto *buttonLay = new QGridLayout(buttonCard);
    buttonLay->setContentsMargins(16, 16, 16, 16);
    buttonLay->setHorizontalSpacing(10);
    buttonLay->setVerticalSpacing(10);

    m_refreshBtn = new QPushButton(QStringLiteral("刷新资料"), buttonCard);
    m_nicknameBtn = new QPushButton(QStringLiteral("修改昵称"), buttonCard);
    m_avatarBtn = new QPushButton(QStringLiteral("更换头像"), buttonCard);
    m_rechargeBtn = new QPushButton(QStringLiteral("余额充值"), buttonCard);
    m_logoutBtn = new QPushButton(QStringLiteral("退出登录"), buttonCard);
    m_logoutBtn->setObjectName(QStringLiteral("Danger"));

    buttonLay->addWidget(m_refreshBtn, 0, 0);
    buttonLay->addWidget(m_nicknameBtn, 0, 1);
    buttonLay->addWidget(m_avatarBtn, 1, 0);
    buttonLay->addWidget(m_rechargeBtn, 1, 1);
    buttonLay->addWidget(m_logoutBtn, 2, 0, 1, 2);

    m_status = new QLabel(QStringLiteral("正在等待用户资料..."), w);
    m_status->setObjectName(QStringLiteral("Muted"));
    m_status->setWordWrap(true);

    lay->addWidget(hero);
    lay->addWidget(buttonCard);
    lay->addWidget(m_status);
    lay->addStretch();

    connect(m_refreshBtn, &QPushButton::clicked, this, &MainWindow::refreshProfile);
    connect(m_nicknameBtn, &QPushButton::clicked, this, &MainWindow::editNickname);
    connect(m_avatarBtn, &QPushButton::clicked, this, &MainWindow::editAvatar);
    connect(m_rechargeBtn, &QPushButton::clicked, this, &MainWindow::recharge);
    connect(m_logoutBtn, &QPushButton::clicked, this, &MainWindow::logout);

    updateMineTexts();
    return w;
}

void MainWindow::requestProfile()
{
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    setStatus(QStringLiteral("正在加载用户资料..."));
    m_net->send(ecp::CMD_USER_INFO);
}

void MainWindow::applyProfile(const QJsonObject &data)
{
    m_profile = data;
    updateMineTexts();
    refreshAvatarBadge();
}

void MainWindow::setStatus(const QString &text, bool isError)
{
    if (!m_status) return;
    m_status->setText(text);
    m_status->setStyleSheet(isError ? QStringLiteral("color:#b91c1c;")
                                    : QStringLiteral("color:#6b7280;"));
}

void MainWindow::refreshAvatarBadge()
{
    if (!m_avatar) return;
    const QString avatarPath = profileName().left(1).toUpper();
    m_avatar->setText(avatarPath.isEmpty() ? QStringLiteral("用") : avatarPath);
}

QString MainWindow::profileName() const
{
    const QString nickname = textOrEmpty(m_profile, "nickname").trimmed();
    if (!nickname.isEmpty()) return nickname;
    return QStringLiteral("未登录");
}

QString MainWindow::profilePhone() const
{
    const QString phone = textOrEmpty(m_profile, "phone").trimmed();
    if (phone.isEmpty()) return QStringLiteral("--");
    return phone;
}

QString MainWindow::balanceText() const
{
    const qint64 balanceFen = m_profile.value(QStringLiteral("balance")).toVariant().toLongLong();
    return QStringLiteral("余额：¥ %1").arg(ecp::fenToYuan(balanceFen));
}

void MainWindow::updateMineTexts()
{
    if (m_name) m_name->setText(profileName());
    if (m_phone) m_phone->setText(QStringLiteral("手机号：%1").arg(profilePhone()));
    if (m_balance) m_balance->setText(balanceText());
}

void MainWindow::onNetResponse(int cmd, int, int code, const QString &msg, const QJsonObject &data)
{
    if (code == ecp::ERR_TOKEN_INVALID || code == ecp::ERR_NOT_LOGIN) {
        setStatus(msg, true);
        if (m_net) m_net->setToken(QString());
        QMessageBox::information(this, QStringLiteral("提示"), msg);
        emit logoutRequested();
        close();
        return;
    }

    if (cmd == ecp::CMD_USER_INFO) {
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        applyProfile(data);
        setStatus(QStringLiteral("资料已更新"));
        return;
    }

    if (cmd == ecp::CMD_USER_SET_NICKNAME) {
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        requestProfile();
        return;
    }

    if (cmd == ecp::CMD_USER_SET_AVATAR) {
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        requestProfile();
        return;
    }

    if (cmd == ecp::CMD_USER_RECHARGE) {
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        if (data.contains(QStringLiteral("balance"))) {
            m_profile[QStringLiteral("balance")] = data.value(QStringLiteral("balance"));
            updateMineTexts();
        }
        setStatus(QStringLiteral("充值成功"));
        return;
    }

    if (cmd == ecp::CMD_ORDER_UNFINISHED) {
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        const bool hasUnfinished = data.value(QStringLiteral("hasUnfinished")).toBool();
        if (hasUnfinished) {
            QMessageBox::information(this, QStringLiteral("提示"),
                                     QStringLiteral("您有未完成的充电订单，请先结算。"));
        }
        return;
    }
}

void MainWindow::onNetDisconnected()
{
    setStatus(QStringLiteral("与服务器断开连接"), true);
}

void MainWindow::refreshProfile()
{
    requestProfile();
}

void MainWindow::editNickname()
{
    const QString current = profileName();
    bool ok = false;
    const QString nickname = QInputDialog::getText(
        this, QStringLiteral("修改昵称"), QStringLiteral("请输入新昵称"),
        QLineEdit::Normal, current, &ok).trimmed();
    if (!ok || nickname.isEmpty() || nickname == current) return;
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    setStatus(QStringLiteral("正在修改昵称..."));
    m_net->send(ecp::CMD_USER_SET_NICKNAME, QJsonObject{{QStringLiteral("nickname"), nickname}});
}

void MainWindow::editAvatar()
{
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    const QString avatarPath = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择头像"),
        QString(),
        QStringLiteral("图片文件 (*.png *.jpg *.jpeg *.bmp *.gif);;所有文件 (*.*)"));
    if (avatarPath.isEmpty()) return;
    setStatus(QStringLiteral("正在更换头像..."));
    m_net->send(ecp::CMD_USER_SET_AVATAR, QJsonObject{{QStringLiteral("avatarPath"), avatarPath}});
}

void MainWindow::recharge()
{
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    bool ok = false;
    const QString amountText = QInputDialog::getText(
        this, QStringLiteral("余额充值"), QStringLiteral("请输入充值金额（元）"),
        QLineEdit::Normal, QStringLiteral("10.00"), &ok).trimmed();
    if (!ok || amountText.isEmpty()) return;
    const qint64 amountFen = ecp::yuanToFen(amountText);
    if (amountFen <= 0) {
        setStatus(QStringLiteral("充值金额格式不正确"), true);
        return;
    }
    setStatus(QStringLiteral("正在提交充值..."));
    m_net->send(ecp::CMD_USER_RECHARGE, QJsonObject{{QStringLiteral("amount"), amountFen}});
}

void MainWindow::logout()
{
    emit logoutRequested();
    close();
}

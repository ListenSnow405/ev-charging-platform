#include "main_window.h"

#include "app_path.h"
#include "protocol.h"
#include "time_util.h"

#include <QFrame>
#include <QButtonGroup>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QSettings>
#include <QPushButton>
#include <QSizePolicy>
#include <QFont>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#ifdef HAVE_WEBENGINE
#include <QWebEngineView>
#include <QWebEngineSettings>
#endif

namespace {
static QString textOrEmpty(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toString();
}

static QString buildNavPreviewHtml(const QString &key, double lat, double lng)
{
    const QString safeKey = key.toHtmlEscaped();
    return QStringLiteral(R"HTML(
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    html, body, #map { width: 100%%; height: 100%%; margin: 0; overflow: hidden; }
    body { background: #f5f7fb; font-family: sans-serif; }
    #tip {
      position: fixed; left: 16px; top: 16px; z-index: 2;
      max-width: calc(100%% - 32px);
      background: rgba(17, 24, 39, 0.88); color: #fff;
      border-radius: 12px; padding: 10px 12px; line-height: 1.5;
      box-shadow: 0 10px 24px rgba(15, 23, 42, 0.18);
      font-size: 13px;
    }
    #tip b { display: block; font-size: 14px; margin-bottom: 2px; }
  </style>
  <script src="https://map.qq.com/api/gljs?v=1.exp&key=%1"></script>
</head>
<body>
  <div id="tip">
    <b>一键导航</b>
    这里是腾讯地图预览页。输入终点后点击“开始导航”，会跳转到路线规划。
  </div>
  <div id="map"></div>
  <script>
    function initMap() {
      const center = new TMap.LatLng(%2, %3);
      new TMap.Map(document.getElementById('map'), {
        center: center,
        zoom: 13,
        pitch: 0,
        rotation: 0
      });
    }
    window.onload = initMap;
  </script>
</body>
</html>
)HTML").arg(safeKey).arg(lat, 0, 'f', 6).arg(lng, 0, 'f', 6);
}

static QUrl buildRoutePlanUrl(const QString &mode, double lat, double lng, const QString &destName)
{
    QUrl url(QStringLiteral("https://apis.map.qq.com/uri/v1/routeplan"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("type"), mode);
    query.addQueryItem(QStringLiteral("from"), QStringLiteral("当前位置"));
    query.addQueryItem(QStringLiteral("fromcoord"), QStringLiteral("%1,%2").arg(lat, 0, 'f', 6).arg(lng, 0, 'f', 6));
    query.addQueryItem(QStringLiteral("to"), destName.trimmed());
    query.addQueryItem(QStringLiteral("referer"), QStringLiteral("ecp-user"));
    url.setQuery(query);
    return url;
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
    lay->setSpacing(12);

    auto *panel = new QFrame(w);
    panel->setObjectName(QStringLiteral("Card"));
    auto *panelLay = new QVBoxLayout(panel);
    panelLay->setContentsMargins(16, 16, 16, 16);
    panelLay->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("一键导航"), panel);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);

    auto *subtitle = new QLabel(QStringLiteral("读取本地 config/app.ini 中的腾讯地图 Key，加载路线规划页。"), panel);
    subtitle->setObjectName(QStringLiteral("Muted"));
    subtitle->setWordWrap(true);

    auto *destEdit = new QLineEdit(panel);
    destEdit->setPlaceholderText(QStringLiteral("输入目的地，例如：深圳市民中心"));
    destEdit->setText(QStringLiteral("深圳市民中心"));

    auto *modeRow = new QHBoxLayout;
    auto *driveBtn = new QPushButton(QStringLiteral("驾车"), panel);
    auto *walkBtn = new QPushButton(QStringLiteral("步行"), panel);
    driveBtn->setCheckable(true);
    walkBtn->setCheckable(true);
    driveBtn->setChecked(true);
    auto *modeGroup = new QButtonGroup(panel);
    modeGroup->setExclusive(true);
    modeGroup->addButton(driveBtn);
    modeGroup->addButton(walkBtn);
    modeRow->addWidget(driveBtn);
    modeRow->addWidget(walkBtn);
    modeRow->addStretch();

    auto *actionRow = new QHBoxLayout;
    auto *previewBtn = new QPushButton(QStringLiteral("地图预览"), panel);
    auto *routeBtn = new QPushButton(QStringLiteral("开始导航"), panel);
    auto *resetBtn = new QPushButton(QStringLiteral("回到预览"), panel);
    actionRow->addWidget(previewBtn);
    actionRow->addWidget(routeBtn);
    actionRow->addWidget(resetBtn);
    actionRow->addStretch();

    auto *status = new QLabel(panel);
    status->setObjectName(QStringLiteral("Muted"));
    status->setWordWrap(true);

    panelLay->addWidget(title);
    panelLay->addWidget(subtitle);
    panelLay->addWidget(destEdit);
    panelLay->addLayout(modeRow);
    panelLay->addLayout(actionRow);
    panelLay->addWidget(status);

#ifdef HAVE_WEBENGINE
    auto *view = new QWebEngineView(w);
    view->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);

    const QString cfgPath = ecp::resPath(QStringLiteral("config/app.ini"));
    QSettings cfg(cfgPath, QSettings::IniFormat);
    const QString key = cfg.value(QStringLiteral("map/key")).toString().trimmed();
    const double defaultLat = cfg.value(QStringLiteral("map/default_lat"), 22.5470).toDouble();
    const double defaultLng = cfg.value(QStringLiteral("map/default_lng"), 114.0650).toDouble();

    auto loadPreview = [view, status, key, defaultLat, defaultLng]() {
        if (key.isEmpty()) {
            view->setHtml(QStringLiteral(
                "<html><body style='font-family:sans-serif;padding:20px;color:#111827'>"
                "<h3>地图未配置</h3>"
                "<p>请先在 config/app.ini 填写 <code>[map] key</code>。</p>"
                "<p style='color:#6b7280'>导航页会在这里加载腾讯地图。</p>"
                "</body></html>"));
            status->setText(QStringLiteral("未检测到地图 Key，请先配置 config/app.ini 的 [map] key。"));
            return;
        }
        view->setHtml(buildNavPreviewHtml(key, defaultLat, defaultLng),
                      QUrl(QStringLiteral("https://map.qq.com/")));
        status->setText(QStringLiteral("地图预览已加载。"));
    };

    auto loadRoute = [view, status, key, defaultLat, defaultLng, driveBtn, destEdit]() {
        const QString destination = destEdit->text().trimmed();
        if (destination.isEmpty()) {
            status->setText(QStringLiteral("请先输入目的地。"));
            return;
        }
        const QString mode = driveBtn->isChecked() ? QStringLiteral("drive")
                                                    : QStringLiteral("walk");
        if (key.isEmpty()) {
            status->setText(QStringLiteral("未检测到地图 Key，请先配置 config/app.ini 的 [map] key。"));
            return;
        }
        view->load(buildRoutePlanUrl(mode, defaultLat, defaultLng, destination));
        status->setText(QStringLiteral("已打开路线规划。"));
    };

    connect(previewBtn, &QPushButton::clicked, this, loadPreview);
    connect(routeBtn, &QPushButton::clicked, this, loadRoute);
    connect(resetBtn, &QPushButton::clicked, this, loadPreview);
    connect(driveBtn, &QPushButton::clicked, this, [status] { status->setText(QStringLiteral("当前模式：驾车")); });
    connect(walkBtn, &QPushButton::clicked, this, [status] { status->setText(QStringLiteral("当前模式：步行")); });

    lay->addWidget(panel, 0);
    lay->addWidget(view, 1);
    loadPreview();
#else
    panelLay->addWidget(new QLabel(QStringLiteral("QtWebEngineWidgets 未安装，导航页仅显示占位说明。"), panel));
    lay->addWidget(panel, 0);
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

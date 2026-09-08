#include "main_window.h"

#include "app_path.h"
#include "protocol.h"
#include "time_util.h"

#include <algorithm>
#include <QAbstractItemView>
#include <QComboBox>
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
#include <QHeaderView>
#include <QLayoutItem>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVector>
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

static QString formatDistance(qint64 metres)
{
    if (metres < 0) return QStringLiteral("未知距离");
    if (metres < 1000) return QStringLiteral("%1 m").arg(metres);
    return QStringLiteral("%1 km").arg(QString::number(metres / 1000.0, 'f', metres < 10000 ? 1 : 0));
}

static QString formatPrice(qint64 fen)
{
    return QStringLiteral("%1 元/度").arg(ecp::fenToYuan(fen));
}

static QString stationLoadLabel(const QJsonObject &item)
{
    const qint64 total = item.value(QStringLiteral("pileTotal")).toVariant().toLongLong();
    const qint64 idle = item.value(QStringLiteral("pileIdle")).toVariant().toLongLong();
    if (total <= 0) return QStringLiteral("暂无电桩");
    if (idle <= 0) return QStringLiteral("当前繁忙");
    if (idle * 2 >= total) return QStringLiteral("空闲较多");
    return QStringLiteral("状态正常");
}

static int stationSortCompare(const QJsonObject &left, const QJsonObject &right, int sortIndex)
{
    const qint64 leftDistance = left.value(QStringLiteral("distance")).toVariant().toLongLong();
    const qint64 rightDistance = right.value(QStringLiteral("distance")).toVariant().toLongLong();
    const qint64 leftIdle = left.value(QStringLiteral("pileIdle")).toVariant().toLongLong();
    const qint64 rightIdle = right.value(QStringLiteral("pileIdle")).toVariant().toLongLong();
    const qint64 leftPrice = left.value(QStringLiteral("price")).toVariant().toLongLong();
    const qint64 rightPrice = right.value(QStringLiteral("price")).toVariant().toLongLong();
    if (sortIndex == 1) {
        if (leftIdle != rightIdle) return leftIdle > rightIdle ? -1 : 1;
        if (leftDistance != rightDistance) return leftDistance < rightDistance ? -1 : 1;
    } else if (sortIndex == 2) {
        if (leftPrice != rightPrice) return leftPrice < rightPrice ? -1 : 1;
        if (leftDistance != rightDistance) return leftDistance < rightDistance ? -1 : 1;
    } else {
        if (leftDistance != rightDistance) return leftDistance < rightDistance ? -1 : 1;
        if (leftIdle != rightIdle) return leftIdle > rightIdle ? -1 : 1;
    }
    const qint64 leftId = left.value(QStringLiteral("stationId")).toVariant().toLongLong();
    const qint64 rightId = right.value(QStringLiteral("stationId")).toVariant().toLongLong();
    if (leftId == rightId) return 0;
    return leftId < rightId ? -1 : 1;
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
    m_tabs->addTab(makeNearbyPage(), QStringLiteral("附近电桩"));
    m_tabs->addTab(makeNavPage(), QStringLiteral("导航"));
    m_tabs->addTab(makeChargePage(), QStringLiteral("充电"));
    m_tabs->addTab(makeMinePage(), QStringLiteral("我的"));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(m_tabs);

    connect(m_net, &NetClient::response, this, &MainWindow::onNetResponse);
    connect(m_net, &NetClient::disconnected, this, &MainWindow::onNetDisconnected);

    requestProfile();
    requestNearbyStations();
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
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 18, 18, 18);
    lay->setSpacing(12);

    auto *toolbar = new QFrame(w);
    toolbar->setObjectName(QStringLiteral("Card"));
    auto *toolbarLay = new QVBoxLayout(toolbar);
    toolbarLay->setContentsMargins(16, 16, 16, 16);
    toolbarLay->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("附近电桩"), toolbar);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);

    m_nearbySummary = new QLabel(QStringLiteral("正在等待加载附近站点"), toolbar);
    m_nearbySummary->setObjectName(QStringLiteral("Muted"));
    m_nearbySummary->setWordWrap(true);

    auto *searchRow = new QHBoxLayout;
    m_nearbySearch = new QLineEdit(toolbar);
    m_nearbySearch->setPlaceholderText(QStringLiteral("输入站点名或地址"));
    m_nearbySort = new QComboBox(toolbar);
    m_nearbySort->addItem(QStringLiteral("距离最近"));
    m_nearbySort->addItem(QStringLiteral("空闲优先"));
    m_nearbySort->addItem(QStringLiteral("价格优先"));
    m_nearbyRefreshBtn = new QPushButton(QStringLiteral("刷新站点"), toolbar);
    searchRow->addWidget(m_nearbySearch, 1);
    searchRow->addWidget(m_nearbySort, 0);
    searchRow->addWidget(m_nearbyRefreshBtn, 0);

    m_nearbyStatus = new QLabel(QStringLiteral("请刷新以加载附近站点"), toolbar);
    m_nearbyStatus->setObjectName(QStringLiteral("Muted"));
    m_nearbyStatus->setWordWrap(true);

    toolbarLay->addWidget(title);
    toolbarLay->addWidget(m_nearbySummary);
    toolbarLay->addLayout(searchRow);
    toolbarLay->addWidget(m_nearbyStatus);

    auto *listCard = new QFrame(w);
    listCard->setObjectName(QStringLiteral("Card"));
    auto *listLay = new QVBoxLayout(listCard);
    listLay->setContentsMargins(16, 16, 16, 16);
    listLay->setSpacing(10);

    auto *listTitle = new QLabel(QStringLiteral("站点列表"), listCard);
    QFont listFont = listTitle->font();
    listFont.setPointSize(14);
    listFont.setBold(true);
    listTitle->setFont(listFont);

    m_nearbyScroll = new QScrollArea(listCard);
    m_nearbyScroll->setWidgetResizable(true);
    m_nearbyScroll->setFrameShape(QFrame::NoFrame);
    m_nearbyCardsHost = new QWidget(m_nearbyScroll);
    m_nearbyCardsLay = new QVBoxLayout(m_nearbyCardsHost);
    m_nearbyCardsLay->setContentsMargins(0, 0, 0, 0);
    m_nearbyCardsLay->setSpacing(10);
    m_nearbyCardsLay->addStretch();
    m_nearbyScroll->setWidget(m_nearbyCardsHost);

    listLay->addWidget(listTitle);
    listLay->addWidget(m_nearbyScroll, 1);

    auto *detailCard = new QFrame(w);
    detailCard->setObjectName(QStringLiteral("Card"));
    auto *detailLay = new QVBoxLayout(detailCard);
    detailLay->setContentsMargins(16, 16, 16, 16);
    detailLay->setSpacing(8);

    m_nearbyDetailTitle = new QLabel(QStringLiteral("请选择一个站点查看电桩"), detailCard);
    QFont detailTitleFont = m_nearbyDetailTitle->font();
    detailTitleFont.setPointSize(14);
    detailTitleFont.setBold(true);
    m_nearbyDetailTitle->setFont(detailTitleFont);

    m_nearbyDetailMeta = new QLabel(QStringLiteral("站内电桩详情会显示在这里"), detailCard);
    m_nearbyDetailMeta->setObjectName(QStringLiteral("Muted"));
    m_nearbyDetailMeta->setWordWrap(true);

    m_nearbyPileStatus = new QLabel(QStringLiteral("尚未选择站点"), detailCard);
    m_nearbyPileStatus->setObjectName(QStringLiteral("Muted"));
    m_nearbyPileStatus->setWordWrap(true);

    m_nearbyPileTable = new QTableWidget(0, 4, detailCard);
    m_nearbyPileTable->setHorizontalHeaderLabels({
        QStringLiteral("编号"), QStringLiteral("类型"), QStringLiteral("状态"), QStringLiteral("功率")
    });
    m_nearbyPileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_nearbyPileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_nearbyPileTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_nearbyPileTable->setAlternatingRowColors(true);
    m_nearbyPileTable->horizontalHeader()->setStretchLastSection(true);
    m_nearbyPileTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_nearbyPileTable->verticalHeader()->setVisible(false);

    detailLay->addWidget(m_nearbyDetailTitle);
    detailLay->addWidget(m_nearbyDetailMeta);
    detailLay->addWidget(m_nearbyPileStatus);
    detailLay->addWidget(m_nearbyPileTable, 1);

    lay->addWidget(toolbar, 0);
    lay->addWidget(listCard, 2);
    lay->addWidget(detailCard, 1);

    connect(m_nearbyRefreshBtn, &QPushButton::clicked, this, &MainWindow::requestNearbyStations);
    connect(m_nearbySearch, &QLineEdit::returnPressed, this, &MainWindow::requestNearbyStations);
    connect(m_nearbySort, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &MainWindow::renderNearbyStations);

    return w;
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

    m_navDestEdit = new QLineEdit(panel);
    auto *destEdit = m_navDestEdit;
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

void MainWindow::requestNearbyStations()
{
    if (!m_nearbyStatus) return;
    if (!m_net || !m_net->isConnected()) {
        m_nearbyStatus->setText(QStringLiteral("未连接到服务器，请先启动服务端并重新登录"));
        m_nearbyStatus->setStyleSheet(QStringLiteral("color:#b91c1c;"));
        return;
    }

    const QString cfgPath = ecp::resPath(QStringLiteral("config/app.ini"));
    QSettings cfg(cfgPath, QSettings::IniFormat);
    m_mapKey = cfg.value(QStringLiteral("map/key")).toString().trimmed();
    m_mapDefaultLat = cfg.value(QStringLiteral("map/default_lat"), 22.5470).toDouble();
    m_mapDefaultLng = cfg.value(QStringLiteral("map/default_lng"), 114.0650).toDouble();

    const QString keyword = m_nearbySearch ? m_nearbySearch->text().trimmed() : QString();
    const int sortBy = m_nearbySort && m_nearbySort->currentIndex() == 1 ? 1 : 0;
    QJsonObject req{
        {QStringLiteral("lng"), m_mapDefaultLng},
        {QStringLiteral("lat"), m_mapDefaultLat},
        {QStringLiteral("keyword"), keyword},
        {QStringLiteral("sortBy"), sortBy}
    };

    m_pendingNearbyStationsSeq = m_net->send(ecp::CMD_STATION_NEARBY, req);
    if (m_pendingNearbyStationsSeq >= 0) {
        m_nearbyStatus->setText(QStringLiteral("正在加载附近站点..."));
        m_nearbyStatus->setStyleSheet(QStringLiteral("color:#6b7280;"));
    }
}

void MainWindow::requestStationPiles(qint64 stationId)
{
    if (!m_net || !m_net->isConnected()) {
        if (m_nearbyPileStatus) {
            m_nearbyPileStatus->setText(QStringLiteral("未连接到服务器，无法加载站内电桩"));
            m_nearbyPileStatus->setStyleSheet(QStringLiteral("color:#b91c1c;"));
        }
        return;
    }

    m_selectedNearbyStationId = stationId;
    m_pendingNearbyPilesSeq = m_net->send(ecp::CMD_STATION_PILES,
                                          QJsonObject{{QStringLiteral("stationId"), stationId}});
    if (m_pendingNearbyPilesSeq >= 0 && m_nearbyPileStatus) {
        m_nearbyPileStatus->setText(QStringLiteral("正在加载站内电桩..."));
        m_nearbyPileStatus->setStyleSheet(QStringLiteral("color:#6b7280;"));
    }
}

void MainWindow::renderNearbyStations()
{
    if (!m_nearbyCardsLay || !m_nearbySummary || !m_nearbyStatus) return;

    while (m_nearbyCardsLay->count() > 0) {
        QLayoutItem *item = m_nearbyCardsLay->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }

    QVector<QJsonObject> stations;
    stations.reserve(m_nearbyStations.size());
    for (const QJsonValue &value : m_nearbyStations) {
        if (value.isObject()) stations.append(value.toObject());
    }
    const int sortIndex = m_nearbySort ? m_nearbySort->currentIndex() : 0;
    std::sort(stations.begin(), stations.end(), [sortIndex](const QJsonObject &left,
                                                            const QJsonObject &right) {
        return stationSortCompare(left, right, sortIndex) < 0;
    });

    const qint64 prevSelectedId = m_selectedNearbyStationId;
    QJsonObject selectedStation;
    bool selectedFound = false;
    for (const QJsonObject &station : stations) {
        if (station.value(QStringLiteral("stationId")).toVariant().toLongLong() == prevSelectedId) {
            selectedStation = station;
            selectedFound = true;
            break;
        }
    }
    if (!selectedFound && !stations.isEmpty()) {
        selectedStation = stations.first();
        m_selectedNearbyStationId = selectedStation.value(QStringLiteral("stationId")).toVariant().toLongLong();
        m_selectedNearbyStationName = selectedStation.value(QStringLiteral("name")).toString();
        selectedFound = true;
        requestStationPiles(m_selectedNearbyStationId);
    }
    if (selectedFound) {
        const QString selectedName = selectedStation.value(QStringLiteral("name")).toString();
        const qint64 selectedPrice = selectedStation.value(QStringLiteral("price")).toVariant().toLongLong();
        const qint64 selectedTotal = selectedStation.value(QStringLiteral("pileTotal")).toVariant().toLongLong();
        const qint64 selectedIdle = selectedStation.value(QStringLiteral("pileIdle")).toVariant().toLongLong();
        const qint64 selectedDistance = selectedStation.value(QStringLiteral("distance")).toVariant().toLongLong();
        if (m_nearbyDetailTitle) {
            m_nearbyDetailTitle->setText(selectedName.isEmpty() ? QStringLiteral("未命名站点") : selectedName);
        }
        if (m_nearbyDetailMeta) {
            m_nearbyDetailMeta->setText(QStringLiteral("%1 · %2 · 空闲 %3/%4 · %5")
                                        .arg(formatPrice(selectedPrice))
                                        .arg(stationLoadLabel(selectedStation))
                                        .arg(selectedIdle)
                                        .arg(selectedTotal)
                                        .arg(formatDistance(selectedDistance)));
        }
        m_selectedNearbyStationName = selectedName;
    }

    m_nearbySummary->setText(QStringLiteral("当前位置：%1, %2 · 已找到 %3 个站点")
                             .arg(m_mapDefaultLat, 0, 'f', 4)
                             .arg(m_mapDefaultLng, 0, 'f', 4)
                             .arg(stations.size()));

    if (stations.isEmpty()) {
        auto *empty = new QLabel(QStringLiteral("附近暂无可用站点，可以更换关键词后重试。"), m_nearbyCardsHost);
        empty->setObjectName(QStringLiteral("Muted"));
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignCenter);
        empty->setMinimumHeight(120);
        m_nearbyCardsLay->addWidget(empty);
        m_nearbyCardsLay->addStretch();
        if (m_nearbyPileTable) m_nearbyPileTable->setRowCount(0);
        if (m_nearbyDetailTitle) m_nearbyDetailTitle->setText(QStringLiteral("请选择一个站点查看电桩"));
        if (m_nearbyDetailMeta) m_nearbyDetailMeta->setText(QStringLiteral("站内电桩详情会显示在这里"));
        if (m_nearbyPileStatus) m_nearbyPileStatus->setText(QStringLiteral("尚未选择站点"));
        m_nearbyStatus->setText(QStringLiteral("未查到符合条件的附近电桩"));
        return;
    }

    for (const QJsonObject &station : stations) {
        const qint64 stationId = station.value(QStringLiteral("stationId")).toVariant().toLongLong();
        const QString name = station.value(QStringLiteral("name")).toString();
        const QString address = station.value(QStringLiteral("address")).toString();
        const qint64 price = station.value(QStringLiteral("price")).toVariant().toLongLong();
        const qint64 total = station.value(QStringLiteral("pileTotal")).toVariant().toLongLong();
        const qint64 idle = station.value(QStringLiteral("pileIdle")).toVariant().toLongLong();
        const qint64 distance = station.value(QStringLiteral("distance")).toVariant().toLongLong();

        auto *card = new QFrame(m_nearbyCardsHost);
        card->setObjectName(QStringLiteral("Card"));
        auto *cardLay = new QVBoxLayout(card);
        cardLay->setContentsMargins(14, 12, 14, 12);
        cardLay->setSpacing(8);

        auto *topRow = new QHBoxLayout;
        auto *nameLabel = new QLabel(name.isEmpty() ? QStringLiteral("未命名站点") : name, card);
        QFont nameFont = nameLabel->font();
        nameFont.setPointSize(13);
        nameFont.setBold(true);
        nameLabel->setFont(nameFont);
        auto *distanceLabel = new QLabel(formatDistance(distance), card);
        distanceLabel->setObjectName(QStringLiteral("Badge"));
        topRow->addWidget(nameLabel, 1);
        topRow->addWidget(distanceLabel, 0);

        auto *meta = new QLabel(QStringLiteral("%1 · %2 · 空闲 %3/%4")
                                .arg(formatPrice(price))
                                .arg(stationLoadLabel(station))
                                .arg(idle)
                                .arg(total), card);
        meta->setObjectName(QStringLiteral("Muted"));
        meta->setWordWrap(true);

        auto *addr = new QLabel(address.isEmpty() ? QStringLiteral("暂无地址") : address, card);
        addr->setObjectName(QStringLiteral("Muted"));
        addr->setWordWrap(true);

        auto *actionRow = new QHBoxLayout;
        auto *detailBtn = new QPushButton(QStringLiteral("查看电桩"), card);
        detailBtn->setObjectName(QStringLiteral("Primary"));
        auto *navBtn = new QPushButton(QStringLiteral("去导航"), card);
        actionRow->addWidget(detailBtn);
        actionRow->addWidget(navBtn);
        actionRow->addStretch();

        connect(detailBtn, &QPushButton::clicked, this, [this, stationId, name, metaText = meta->text()] {
            m_selectedNearbyStationName = name;
            if (m_nearbyDetailTitle) m_nearbyDetailTitle->setText(name);
            if (m_nearbyDetailMeta) m_nearbyDetailMeta->setText(metaText);
            requestStationPiles(stationId);
        });
        connect(navBtn, &QPushButton::clicked, this, [this, name] {
            if (m_navDestEdit) m_navDestEdit->setText(name);
            if (m_tabs) m_tabs->setCurrentIndex(1);
        });

        cardLay->addLayout(topRow);
        cardLay->addWidget(meta);
        cardLay->addWidget(addr);
        cardLay->addLayout(actionRow);
        m_nearbyCardsLay->addWidget(card);
    }
    m_nearbyCardsLay->addStretch();
    m_nearbyStatus->setText(QStringLiteral("站点列表已更新"));
    m_nearbyStatus->setStyleSheet(QStringLiteral("color:#6b7280;"));
}

void MainWindow::renderStationPiles()
{
    if (!m_nearbyPileTable || !m_nearbyPileStatus) return;
    m_nearbyPileTable->setRowCount(0);

    if (m_nearbyPiles.isEmpty()) {
        m_nearbyPileStatus->setText(QStringLiteral("该站点暂无电桩数据"));
        return;
    }

    m_nearbyPileTable->setRowCount(m_nearbyPiles.size());
    int row = 0;
    for (const QJsonValue &value : m_nearbyPiles) {
        const QJsonObject pile = value.toObject();
        const QString code = pile.value(QStringLiteral("code")).toString();
        const QString type = pile.value(QStringLiteral("type")).toInt() == 0
                                 ? QStringLiteral("快充")
                                 : QStringLiteral("慢充");
        QString status = QStringLiteral("未知");
        switch (pile.value(QStringLiteral("status")).toInt()) {
        case 0: status = QStringLiteral("空闲"); break;
        case 1: status = QStringLiteral("在用"); break;
        case 2: status = QStringLiteral("故障"); break;
        default: break;
        }
        const QString power = QStringLiteral("%1 kW")
                                  .arg(pile.value(QStringLiteral("power")).toVariant().toLongLong());

        m_nearbyPileTable->setItem(row, 0, new QTableWidgetItem(code));
        m_nearbyPileTable->setItem(row, 1, new QTableWidgetItem(type));
        m_nearbyPileTable->setItem(row, 2, new QTableWidgetItem(status));
        m_nearbyPileTable->setItem(row, 3, new QTableWidgetItem(power));
        ++row;
    }
    m_nearbyPileStatus->setText(QStringLiteral("已加载 %1 个电桩").arg(m_nearbyPiles.size()));
    m_nearbyPileStatus->setStyleSheet(QStringLiteral("color:#6b7280;"));
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

void MainWindow::onNetResponse(int cmd, int seq, int code, const QString &msg, const QJsonObject &data)
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

    if (cmd == ecp::CMD_STATION_NEARBY) {
        if (m_pendingNearbyStationsSeq >= 0 && seq != m_pendingNearbyStationsSeq) {
            return;
        }
        m_pendingNearbyStationsSeq = -1;
        if (code != ecp::ERR_OK) {
            if (m_nearbyStatus) {
                m_nearbyStatus->setText(msg);
                m_nearbyStatus->setStyleSheet(QStringLiteral("color:#b91c1c;"));
            }
            return;
        }
        m_nearbyStations = data.value(QStringLiteral("list")).toArray();
        renderNearbyStations();
        return;
    }

    if (cmd == ecp::CMD_STATION_PILES) {
        if (m_pendingNearbyPilesSeq >= 0 && seq != m_pendingNearbyPilesSeq) {
            return;
        }
        m_pendingNearbyPilesSeq = -1;
        if (code != ecp::ERR_OK) {
            if (m_nearbyPileStatus) {
                m_nearbyPileStatus->setText(msg);
                m_nearbyPileStatus->setStyleSheet(QStringLiteral("color:#b91c1c;"));
            }
            return;
        }
        m_nearbyPiles = data.value(QStringLiteral("list")).toArray();
        renderStationPiles();
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

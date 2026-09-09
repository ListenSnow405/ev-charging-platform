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
#include <QIcon>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
#include <QTimer>

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

static QString formatMoney(qint64 fen)
{
    return QStringLiteral("%1 元").arg(ecp::fenToYuan(fen));
}

static QString formatElapsedTime(const QString &startTime, const QString &endTime = QString())
{
    if (startTime.isEmpty()) return QStringLiteral("-");
    const QString end = endTime.isEmpty() ? ecp::nowStr() : endTime;
    const qint64 seconds = ecp::secondsBetween(startTime, end);
    if (seconds < 0) return QStringLiteral("-");
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    const qint64 secs = seconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(secs, 2, 10, QLatin1Char('0'));
}

static qint64 estimateChargeAmountFen(const QJsonObject &order)
{
    const int status = order.value(QStringLiteral("status")).toInt(-1);
    const qint64 amountFen = order.value(QStringLiteral("amount")).toVariant().toLongLong();
    if (status != ecp::ORDER_CHARGING) return amountFen;

    const qint64 priceFen = order.value(QStringLiteral("price")).toVariant().toLongLong();
    const qreal powerKw = order.value(QStringLiteral("power")).toDouble(-1.0);
    const QString startTime = order.value(QStringLiteral("startTime")).toString();
    if (priceFen <= 0 || powerKw <= 0.0 || startTime.isEmpty()) return amountFen;

    qint64 seconds = ecp::secondsBetween(startTime, ecp::nowStr());
    if (seconds <= 0) seconds = 1;
    qint64 kwhX100 = static_cast<qint64>(powerKw * seconds * 100.0 / 3600.0 + 0.5);
    if (kwhX100 <= 0) kwhX100 = 1;
    return (priceFen * kwhX100 + 50) / 100;
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

static QString pileTypeText(int type)
{
    return type == 0 ? QStringLiteral("快充") : QStringLiteral("慢充");
}

static QString pileStatusText(int status)
{
    switch (status) {
    case 0: return QStringLiteral("在用");
    case 1: return QStringLiteral("闲置");
    case 2: return QStringLiteral("故障");
    default: return QStringLiteral("未知");
    }
}

static QString orderStatusText(int status)
{
    switch (status) {
    case ecp::ORDER_RESERVED:   return QStringLiteral("已预约");
    case ecp::ORDER_CHARGING:   return QStringLiteral("充电中");
    case ecp::ORDER_TO_SETTLE:  return QStringLiteral("待结算");
    case ecp::ORDER_SETTLED:    return QStringLiteral("已结算");
    case ecp::ORDER_CANCELLED:  return QStringLiteral("已取消");
    default: return QStringLiteral("未知");
    }
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
        "QTabWidget::pane { border: none; }"
        "QTabBar { background: transparent; expanding: true; }"
        "QTabBar::tab {"
        " background: transparent;"
        " border: none;"
        " color: #6b7280;"
        " font-size: 13px;"
        " min-width: 88px;"
        " min-height: 58px;"
        " padding: 5px 8px;"
        "}"
        "QTabBar::tab:hover { color: #2563eb; }"
        "QTabBar::tab:selected {"
        " background: transparent;"
        " color: #2563eb;"
        " border: none;"
        "}"));

    m_tabs = new QTabWidget(this);
    m_tabs->setTabPosition(QTabWidget::South);
    m_tabs->addTab(makeNearbyPage(), QStringLiteral("附近电桩"));
    m_tabs->addTab(makeNavPage(), QStringLiteral("导航"));
    m_tabs->addTab(makeChargePage(), QStringLiteral("充电"));
    m_tabs->addTab(makeMinePage(), QStringLiteral("我的"));
    m_tabs->setIconSize(QSize(20, 20));
    m_tabs->setTabIcon(0, QIcon(QStringLiteral(":/icons/nearby.svg")));
    m_tabs->setTabIcon(1, QIcon(QStringLiteral(":/icons/navigation.svg")));
    m_tabs->setTabIcon(2, QIcon(QStringLiteral(":/icons/charge.svg")));
    m_tabs->setTabIcon(3, QIcon(QStringLiteral(":/icons/profile.svg")));

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 18);
    lay->addWidget(m_tabs);

    connect(m_net, &NetClient::response, this, &MainWindow::onNetResponse);
    connect(m_net, &NetClient::disconnected, this, &MainWindow::onNetDisconnected);
    connect(m_tabs, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2) {
            renderChargeStations();
            renderChargePiles();
            requestChargeUnfinishedOrder();
            requestChargeOrders();
        }
    });

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
    auto *w = new QWidget;
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(18, 18, 18, 18);
    lay->setSpacing(12);

    auto *summary = new QFrame(w);
    summary->setObjectName(QStringLiteral("Hero"));
    auto *summaryLay = new QVBoxLayout(summary);
    summaryLay->setContentsMargins(18, 18, 18, 18);
    summaryLay->setSpacing(10);

    auto *title = new QLabel(QStringLiteral("充电流程"), summary);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);

    m_chargeStage = new QLabel(QStringLiteral("正在检查未完成订单"), summary);
    m_chargeStage->setObjectName(QStringLiteral("Muted"));
    m_chargeStage->setWordWrap(true);

    m_chargeStatus = new QLabel(QStringLiteral("页面准备中"), summary);
    m_chargeStatus->setObjectName(QStringLiteral("Muted"));
    m_chargeStatus->setWordWrap(true);

    auto *orderRow = new QHBoxLayout;
    m_chargeOrderMeta = new QLabel(QStringLiteral("当前订单：无"), summary);
    m_chargeOrderMeta->setWordWrap(true);
    m_chargeOrderMoney = new QLabel(QStringLiteral("费用：-"), summary);
    m_chargeOrderMoney->setWordWrap(true);
    m_chargeOrderTime = new QLabel(QStringLiteral("已充电时长：-"), summary);
    m_chargeOrderTime->setWordWrap(true);
    m_chargeOrderPrice = new QLabel(QStringLiteral("单价：-"), summary);
    m_chargeOrderPrice->setWordWrap(true);
    orderRow->addWidget(m_chargeOrderMeta, 1);
    orderRow->addWidget(m_chargeOrderMoney, 0);
    orderRow->addWidget(m_chargeOrderTime, 0);

    summaryLay->addWidget(title);
    summaryLay->addWidget(m_chargeStage);
    summaryLay->addWidget(m_chargeStatus);
    summaryLay->addLayout(orderRow);
    summaryLay->addWidget(m_chargeOrderPrice);

    auto *actionCard = new QFrame(w);
    actionCard->setObjectName(QStringLiteral("Card"));
    auto *actionLay = new QGridLayout(actionCard);
    actionLay->setContentsMargins(16, 16, 16, 16);
    actionLay->setHorizontalSpacing(10);
    actionLay->setVerticalSpacing(10);

    m_chargeStationCombo = new QComboBox(actionCard);
    m_chargeStationCombo->setMinimumWidth(160);
    m_chargeStationCombo->addItem(QStringLiteral("请选择充电站"), QVariant::fromValue<qint64>(-1));
    m_chargeReserveBtn = new QPushButton(QStringLiteral("预约电桩"), actionCard);
    m_chargeStartBtn = new QPushButton(QStringLiteral("开始充电"), actionCard);
    m_chargeStopBtn = new QPushButton(QStringLiteral("结束计费"), actionCard);
    m_chargeSettleBtn = new QPushButton(QStringLiteral("立即结算"), actionCard);
    m_chargeCancelBtn = new QPushButton(QStringLiteral("取消预约"), actionCard);
    m_chargeRefreshBtn = new QPushButton(QStringLiteral("刷新"), actionCard);
    m_chargeReserveBtn->setObjectName(QStringLiteral("Primary"));
    m_chargeStartBtn->setObjectName(QStringLiteral("Primary"));
    m_chargeStopBtn->setObjectName(QStringLiteral("Primary"));
    m_chargeSettleBtn->setObjectName(QStringLiteral("Primary"));
    m_chargeCancelBtn->setObjectName(QStringLiteral("Danger"));

    actionLay->addWidget(new QLabel(QStringLiteral("充电站"), actionCard), 0, 0);
    actionLay->addWidget(m_chargeStationCombo, 0, 1);
    actionLay->addWidget(m_chargeRefreshBtn, 0, 2);
    actionLay->addWidget(m_chargeReserveBtn, 1, 0);
    actionLay->addWidget(m_chargeStartBtn, 1, 1);
    actionLay->addWidget(m_chargeStopBtn, 1, 2);
    actionLay->addWidget(m_chargeSettleBtn, 2, 0);
    actionLay->addWidget(m_chargeCancelBtn, 2, 1);

    auto *pileCard = new QFrame(w);
    pileCard->setObjectName(QStringLiteral("Card"));
    auto *pileLay = new QVBoxLayout(pileCard);
    pileLay->setContentsMargins(16, 16, 16, 16);
    pileLay->setSpacing(8);
    auto *pileTitle = new QLabel(QStringLiteral("可用电桩"), pileCard);
    QFont pileTitleFont = pileTitle->font();
    pileTitleFont.setPointSize(14);
    pileTitleFont.setBold(true);
    pileTitle->setFont(pileTitleFont);
    m_chargeHint = new QLabel(QStringLiteral("先选中一个充电站，再从表格里挑电桩预约。"), pileCard);
    m_chargeHint->setObjectName(QStringLiteral("Muted"));
    m_chargeHint->setWordWrap(true);

    m_chargePileTable = new QTableWidget(0, 5, pileCard);
    m_chargePileTable->setHorizontalHeaderLabels({
        QStringLiteral("编号"), QStringLiteral("类型"), QStringLiteral("功率"),
        QStringLiteral("状态"), QStringLiteral("操作")
    });
    m_chargePileTable->horizontalHeader()->setStretchLastSection(true);
    m_chargePileTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_chargePileTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Stretch);
    m_chargePileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_chargePileTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_chargePileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_chargePileTable->setAlternatingRowColors(true);
    m_chargePileTable->verticalHeader()->setVisible(false);
    connect(m_chargePileTable, &QTableWidget::cellClicked, this, [this](int row, int) {
        if (row < 0 || row >= m_nearbyPiles.size()) return;
        const QJsonObject pile = m_nearbyPiles.at(row).toObject();
        m_selectedChargePileId = pile.value(QStringLiteral("pileId")).toVariant().toLongLong();
        updateChargeSummary();
    });

    pileLay->addWidget(pileTitle);
    pileLay->addWidget(m_chargeHint);
    pileLay->addWidget(m_chargePileTable);

    auto *orderCard = new QFrame(w);
    orderCard->setObjectName(QStringLiteral("Card"));
    auto *orderLay = new QVBoxLayout(orderCard);
    orderLay->setContentsMargins(16, 16, 16, 16);
    orderLay->setSpacing(8);
    auto *orderTitle = new QLabel(QStringLiteral("我的订单"), orderCard);
    QFont orderTitleFont = orderTitle->font();
    orderTitleFont.setPointSize(14);
    orderTitleFont.setBold(true);
    orderTitle->setFont(orderTitleFont);

    m_chargeOrderTable = new QTableWidget(0, 6, orderCard);
    m_chargeOrderTable->setHorizontalHeaderLabels({
        QStringLiteral("订单号"), QStringLiteral("电桩"), QStringLiteral("状态"),
        QStringLiteral("金额"), QStringLiteral("预约时间"), QStringLiteral("结算")
    });
    m_chargeOrderTable->horizontalHeader()->setStretchLastSection(true);
    m_chargeOrderTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_chargeOrderTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Stretch);
    m_chargeOrderTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_chargeOrderTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_chargeOrderTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_chargeOrderTable->setAlternatingRowColors(true);
    m_chargeOrderTable->verticalHeader()->setVisible(false);

    orderLay->addWidget(orderTitle);
    orderLay->addWidget(m_chargeOrderTable);

    lay->addWidget(summary);
    lay->addWidget(actionCard);
    lay->addWidget(pileCard, 1);
    lay->addWidget(orderCard, 1);

    connect(m_chargeRefreshBtn, &QPushButton::clicked, this, [this] {
        requestChargeUnfinishedOrder();
        requestChargeOrders();
        if (m_selectedNearbyStationId > 0) {
            requestStationPiles(m_selectedNearbyStationId);
        }
    });
    connect(m_chargeStationCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        const qint64 stationId = m_chargeStationCombo
                                     ? m_chargeStationCombo->currentData().toLongLong()
                                     : -1;
        if (stationId > 0) {
            m_selectedNearbyStationId = stationId;
            requestStationPiles(stationId);
        }
    });
    connect(m_chargeReserveBtn, &QPushButton::clicked, this, [this] {
        reserveChargePile(m_selectedChargePileId);
    });
    connect(m_chargeStartBtn, &QPushButton::clicked, this, &MainWindow::startChargeOrder);
    connect(m_chargeStopBtn, &QPushButton::clicked, this, &MainWindow::stopChargeOrder);
    connect(m_chargeSettleBtn, &QPushButton::clicked, this, &MainWindow::settleChargeOrder);
    connect(m_chargeCancelBtn, &QPushButton::clicked, this, [this] {
        if (m_chargeOrderId <= 0) {
            QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("当前没有可取消的预约"));
            return;
        }
        if (!m_net || !m_net->isConnected()) {
            setStatus(QStringLiteral("未连接到服务器"), true);
            return;
        }
        setStatus(QStringLiteral("正在取消预约..."));
        m_pendingChargeCancelSeq = m_net->send(ecp::CMD_ORDER_CANCEL,
                                               QJsonObject{{QStringLiteral("orderId"), m_chargeOrderId}});
    });

    m_chargeSummaryTimer = new QTimer(this);
    m_chargeSummaryTimer->setInterval(1000);
    connect(m_chargeSummaryTimer, &QTimer::timeout, this, &MainWindow::updateChargeSummary);
    m_chargeSummaryTimer->start();

    updateChargeSummary();
    requestChargeUnfinishedOrder();
    requestChargeOrders();
    return w;
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

    const QString locationText = m_nearbySearch ? m_nearbySearch->text().trimmed() : QString();
    if (m_pendingGeocodeReply) {
        m_pendingGeocodeReply->abort();
        m_pendingGeocodeReply = nullptr;
    }

    if (locationText.isEmpty()) {
        m_nearbyLocationText = QStringLiteral("默认位置");
        sendNearbyStationsRequest(QString());
        return;
    }

    const auto cachedLocation = m_nearbyGeocodeCache.constFind(locationText);
    if (cachedLocation != m_nearbyGeocodeCache.constEnd()) {
        m_mapDefaultLat = cachedLocation.value().first;
        m_mapDefaultLng = cachedLocation.value().second;
        m_nearbyLocationText = locationText;
        m_nearbyStatus->setText(QStringLiteral("已使用地址定位结果，正在加载附近站点..."));
        m_nearbyStatus->setStyleSheet(QStringLiteral("color:#6b7280;"));
        sendNearbyStationsRequest(QString());
        return;
    }

    if (m_mapKey.isEmpty()) {
        m_nearbyLocationText.clear();
        m_nearbyStatus->setText(QStringLiteral("未配置腾讯地图 Key，已按站点名或地址关键词查询。"));
        m_nearbyStatus->setStyleSheet(QStringLiteral("color:#b45309;"));
        sendNearbyStationsRequest(locationText);
        return;
    }

    if (!m_mapNetwork) {
        m_mapNetwork = new QNetworkAccessManager(this);
    }

    QUrl geocodeUrl(QStringLiteral("https://apis.map.qq.com/ws/geocoder/v1/"));
    QUrlQuery geocodeQuery;
    geocodeQuery.addQueryItem(QStringLiteral("address"), locationText);
    geocodeQuery.addQueryItem(QStringLiteral("key"), m_mapKey);
    geocodeUrl.setQuery(geocodeQuery);

    QNetworkRequest geocodeRequest(geocodeUrl);
    geocodeRequest.setHeader(QNetworkRequest::UserAgentHeader,
                             QStringLiteral("ecp-user/1.0"));
    m_nearbyStatus->setText(QStringLiteral("正在解析地址..."));
    m_nearbyStatus->setStyleSheet(QStringLiteral("color:#6b7280;"));

    QNetworkReply *reply = m_mapNetwork->get(geocodeRequest);
    m_pendingGeocodeReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply, locationText] {
        if (reply != m_pendingGeocodeReply) {
            reply->deleteLater();
            return;
        }
        m_pendingGeocodeReply = nullptr;

        const QByteArray body = reply->readAll();
        const QNetworkReply::NetworkError networkError = reply->error();
        const QJsonDocument document = QJsonDocument::fromJson(body);
        const QJsonObject root = document.isObject() ? document.object() : QJsonObject();
        const int apiStatus = root.value(QStringLiteral("status")).toInt(-1);
        const QJsonObject result = root.value(QStringLiteral("result")).toObject();
        const QJsonObject location = result.value(QStringLiteral("location")).toObject();

        const bool validLocation = networkError == QNetworkReply::NoError
            && apiStatus == 0
            && location.contains(QStringLiteral("lat"))
            && location.contains(QStringLiteral("lng"));
        if (!validLocation) {
            m_nearbyLocationText.clear();
            if (m_nearbyStatus) {
                m_nearbyStatus->setText(QStringLiteral(
                    "地址解析失败，已按站点名或地址关键词查询。"));
                m_nearbyStatus->setStyleSheet(QStringLiteral("color:#b45309;"));
            }
            sendNearbyStationsRequest(locationText);
            reply->deleteLater();
            return;
        }

        const double lat = location.value(QStringLiteral("lat")).toDouble();
        const double lng = location.value(QStringLiteral("lng")).toDouble();
        if (lat < -90.0 || lat > 90.0 || lng < -180.0 || lng > 180.0) {
            m_nearbyLocationText.clear();
            if (m_nearbyStatus) {
                m_nearbyStatus->setText(QStringLiteral(
                    "地址解析返回了无效坐标，已按站点名或地址关键词查询。"));
                m_nearbyStatus->setStyleSheet(QStringLiteral("color:#b45309;"));
            }
            sendNearbyStationsRequest(locationText);
            reply->deleteLater();
            return;
        }

        m_mapDefaultLat = lat;
        m_mapDefaultLng = lng;
        m_nearbyLocationText = locationText;
        m_nearbyGeocodeCache.insert(locationText, qMakePair(lat, lng));
        sendNearbyStationsRequest(QString());
        reply->deleteLater();
    });
}

void MainWindow::sendNearbyStationsRequest(const QString &keyword)
{
    if (!m_nearbyStatus) return;

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

    const QString locationLabel = m_nearbyLocationText.isEmpty()
        ? QStringLiteral("关键词查询")
        : m_nearbyLocationText;
    m_nearbySummary->setText(QStringLiteral("定位地址：%1 · 坐标：%2, %3 · 已找到 %4 个站点")
                             .arg(locationLabel)
                             .arg(m_mapDefaultLat, 0, 'f', 6)
                             .arg(m_mapDefaultLng, 0, 'f', 6)
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
    renderChargeStations();
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
    renderChargePiles();
}

void MainWindow::renderChargeStations()
{
    if (!m_chargeStationCombo) return;

    const qint64 currentStationId = m_chargeStationCombo->currentData().toLongLong();
    m_chargeStationCombo->blockSignals(true);
    m_chargeStationCombo->clear();
    m_chargeStationCombo->addItem(QStringLiteral("请选择充电站"), QVariant::fromValue<qint64>(-1));
    for (const QJsonValue &value : m_nearbyStations) {
        const QJsonObject station = value.toObject();
        const qint64 stationId = station.value(QStringLiteral("stationId")).toVariant().toLongLong();
        const QString name = station.value(QStringLiteral("name")).toString();
        const QString label = QStringLiteral("%1 · %2")
                                  .arg(name.isEmpty() ? QStringLiteral("未命名站点") : name)
                                  .arg(formatDistance(station.value(QStringLiteral("distance")).toVariant().toLongLong()));
        m_chargeStationCombo->addItem(label, stationId);
    }
    int targetIndex = m_chargeStationCombo->findData(currentStationId);
    if (targetIndex < 0 && m_selectedNearbyStationId > 0) {
        targetIndex = m_chargeStationCombo->findData(m_selectedNearbyStationId);
    }
    if (targetIndex < 0) targetIndex = 0;
    m_chargeStationCombo->setCurrentIndex(targetIndex);
    m_chargeStationCombo->blockSignals(false);
}

void MainWindow::renderChargePiles()
{
    if (!m_chargePileTable) return;
    m_chargePileTable->setRowCount(0);

    if (m_nearbyPiles.isEmpty()) {
        if (m_chargeHint) {
            m_chargeHint->setText(QStringLiteral("当前没有可用电桩，请先在附近页刷新站点。"));
        }
        return;
    }

    m_chargePileTable->setRowCount(m_nearbyPiles.size());
    int row = 0;
    for (const QJsonValue &value : m_nearbyPiles) {
        const QJsonObject pile = value.toObject();
        const qint64 pileId = pile.value(QStringLiteral("pileId")).toVariant().toLongLong();
        const QString code = pile.value(QStringLiteral("code")).toString();
        const QString type = pileTypeText(pile.value(QStringLiteral("type")).toInt());
        const QString power = QStringLiteral("%1 kW")
                                  .arg(pile.value(QStringLiteral("power")).toVariant().toDouble(), 0, 'f', 0);
        const QString status = pileStatusText(pile.value(QStringLiteral("status")).toInt());

        m_chargePileTable->setItem(row, 0, new QTableWidgetItem(code));
        m_chargePileTable->setItem(row, 1, new QTableWidgetItem(type));
        m_chargePileTable->setItem(row, 2, new QTableWidgetItem(power));
        m_chargePileTable->setItem(row, 3, new QTableWidgetItem(status));

        auto *btn = new QPushButton(QStringLiteral("预约"), m_chargePileTable);
        btn->setObjectName(QStringLiteral("Primary"));
        btn->setEnabled(m_chargeOrder.isEmpty() && pile.value(QStringLiteral("status")).toInt() == 1);
        connect(btn, &QPushButton::clicked, this, [this, pileId] {
            m_selectedChargePileId = pileId;
            reserveChargePile(pileId);
        });
        m_chargePileTable->setCellWidget(row, 4, btn);
        ++row;
    }
    if (m_chargeHint) {
        m_chargeHint->setText(QStringLiteral("从表格里选一个闲置电桩再预约。"));
    }
}

void MainWindow::renderChargeOrders()
{
    if (!m_chargeOrderTable) return;
    m_chargeOrderTable->setRowCount(0);

    if (m_chargeOrders.isEmpty()) return;

    m_chargeOrderTable->setRowCount(m_chargeOrders.size());
    int row = 0;
    for (const QJsonValue &value : m_chargeOrders) {
        const QJsonObject order = value.toObject();
        const qint64 orderId = order.value(QStringLiteral("orderId")).toVariant().toLongLong();
        const QString orderNo = order.value(QStringLiteral("orderNo")).toString();
        const QString pileCode = order.value(QStringLiteral("pileCode")).toString();
        const int status = order.value(QStringLiteral("status")).toInt();
        const qint64 amount = order.value(QStringLiteral("amount")).toVariant().toLongLong();
        const QString reserveTime = order.value(QStringLiteral("reserveTime")).toString();

        m_chargeOrderTable->setItem(row, 0, new QTableWidgetItem(orderNo));
        m_chargeOrderTable->setItem(row, 1, new QTableWidgetItem(pileCode));
        m_chargeOrderTable->setItem(row, 2, new QTableWidgetItem(orderStatusText(status)));
        m_chargeOrderTable->setItem(row, 3, new QTableWidgetItem(formatMoney(amount)));
        m_chargeOrderTable->setItem(row, 4, new QTableWidgetItem(reserveTime));

        auto *settleBtn = new QPushButton(QStringLiteral("结算"), m_chargeOrderTable);
        settleBtn->setEnabled(status == ecp::ORDER_TO_SETTLE);
        connect(settleBtn, &QPushButton::clicked, this, [this, orderId] {
            m_chargeOrderId = orderId;
            settleChargeOrder();
        });
        m_chargeOrderTable->setCellWidget(row, 5, settleBtn);
        ++row;
    }
}

void MainWindow::requestChargeUnfinishedOrder()
{
    if (!m_net || !m_net->isConnected()) {
        if (m_chargeStatus) m_chargeStatus->setText(QStringLiteral("未连接到服务器"));
        return;
    }
    m_pendingChargeUnfinishedSeq = m_net->send(ecp::CMD_ORDER_UNFINISHED);
    if (m_chargeStatus) m_chargeStatus->setText(QStringLiteral("正在检查未完成订单..."));
}

void MainWindow::requestChargeOrders()
{
    if (!m_net || !m_net->isConnected()) {
        return;
    }
    m_pendingChargeOrdersSeq = m_net->send(ecp::CMD_ORDER_LIST,
                                           QJsonObject{
                                               {QStringLiteral("page"), 1},
                                               {QStringLiteral("size"), 20},
                                               {QStringLiteral("status"), -1}
                                           });
}

void MainWindow::sendChargeReserveRequest(qint64 pileId)
{
    if (pileId <= 0) return;
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    m_selectedChargePileId = pileId;
    setStatus(QStringLiteral("正在预约电桩..."));
    m_pendingChargeReserveSeq = m_net->send(ecp::CMD_ORDER_RESERVE,
                                            QJsonObject{{QStringLiteral("pileId"), pileId}});
}

void MainWindow::reserveChargePile(qint64 pileId)
{
    if (pileId <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("请先选择一个电桩"));
        return;
    }
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    m_pendingChargeReservePileId = pileId;
    m_suppressChargeUnfinishedPrompt = false;
    requestChargeUnfinishedOrder();
}

void MainWindow::startChargeOrder()
{
    if (m_chargeOrderId <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("先预约电桩再开始充电"));
        return;
    }
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    setStatus(QStringLiteral("正在开始充电..."));
    m_pendingChargeStartSeq = m_net->send(ecp::CMD_ORDER_START,
                                         QJsonObject{{QStringLiteral("orderId"), m_chargeOrderId}});
}

void MainWindow::stopChargeOrder()
{
    if (m_chargeOrderId <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("当前没有可结束的充电订单"));
        return;
    }
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    setStatus(QStringLiteral("正在结束充电并计费..."));
    m_pendingChargeStopSeq = m_net->send(ecp::CMD_ORDER_STOP,
                                        QJsonObject{{QStringLiteral("orderId"), m_chargeOrderId}});
}

void MainWindow::settleChargeOrder()
{
    if (m_chargeOrderId <= 0) {
        QMessageBox::information(this, QStringLiteral("提示"), QStringLiteral("当前没有可结算的订单"));
        return;
    }
    if (!m_net || !m_net->isConnected()) {
        setStatus(QStringLiteral("未连接到服务器"), true);
        return;
    }
    setStatus(QStringLiteral("正在结算订单..."));
    m_pendingChargeSettleSeq = m_net->send(ecp::CMD_ORDER_SETTLE,
                                          QJsonObject{{QStringLiteral("orderId"), m_chargeOrderId}});
}

void MainWindow::setChargeOrder(const QJsonObject &order)
{
    m_chargeOrder = order;
    m_chargeOrderId = order.value(QStringLiteral("orderId")).toVariant().toLongLong();
    m_selectedChargePileId = order.value(QStringLiteral("pileId")).toVariant().toLongLong();
    updateChargeSummary();
    renderChargePiles();
}

void MainWindow::clearChargeOrder()
{
    m_chargeOrder = QJsonObject();
    m_chargeOrderId = -1;
    updateChargeSummary();
    renderChargePiles();
}

QString MainWindow::chargeOrderStatusText(int status) const
{
    return orderStatusText(status);
}

QString MainWindow::chargeStageText() const
{
    if (m_chargeOrder.isEmpty()) return QStringLiteral("暂无未完成订单");
    return QStringLiteral("当前订单状态：%1").arg(chargeOrderStatusText(m_chargeOrder.value(QStringLiteral("status")).toInt()));
}

void MainWindow::updateChargeSummary()
{
    if (m_chargeOrderMeta) {
        if (m_chargeOrder.isEmpty()) {
            m_chargeOrderMeta->setText(QStringLiteral("当前订单：无"));
        } else {
            m_chargeOrderMeta->setText(QStringLiteral("当前订单：%1 / %2")
                                       .arg(m_chargeOrder.value(QStringLiteral("orderNo")).toString())
                                       .arg(m_chargeOrder.value(QStringLiteral("pileCode")).toString()));
        }
    }
    if (m_chargeOrderMoney) {
        if (m_chargeOrder.isEmpty()) {
            m_chargeOrderMoney->setText(QStringLiteral("费用：-"));
        } else {
            m_chargeOrderMoney->setText(QStringLiteral("费用：%1")
                                        .arg(formatMoney(estimateChargeAmountFen(m_chargeOrder))));
        }
    }
    if (m_chargeOrderTime) {
        if (m_chargeOrder.isEmpty()) {
            m_chargeOrderTime->setText(QStringLiteral("已充电时长：-"));
        } else {
            const QString startTime = m_chargeOrder.value(QStringLiteral("startTime")).toString();
            const QString endTime = m_chargeOrder.value(QStringLiteral("endTime")).toString();
            m_chargeOrderTime->setText(QStringLiteral("已充电时长：%1")
                                       .arg(formatElapsedTime(startTime, endTime)));
        }
    }
    if (m_chargeOrderPrice) {
        if (m_chargeOrder.isEmpty()) {
            m_chargeOrderPrice->setText(QStringLiteral("单价：-"));
        } else {
            m_chargeOrderPrice->setText(QStringLiteral("单价：%1")
                                        .arg(formatPrice(m_chargeOrder.value(QStringLiteral("price")).toVariant().toLongLong())));
        }
    }
    if (m_chargeStage) {
        m_chargeStage->setText(chargeStageText());
    }
    if (m_chargeStatus) {
        if (m_chargeOrder.isEmpty()) {
            m_chargeStatus->setText(QStringLiteral("可以预约新的电桩"));
        } else {
            m_chargeStatus->setText(QStringLiteral("当前状态：%1").arg(chargeOrderStatusText(m_chargeOrder.value(QStringLiteral("status")).toInt())));
        }
    }
    const int status = m_chargeOrder.value(QStringLiteral("status")).toInt(-1);
    const bool hasOrder = !m_chargeOrder.isEmpty();
    if (m_chargeReserveBtn) m_chargeReserveBtn->setEnabled(!hasOrder && m_selectedChargePileId > 0);
    if (m_chargeStartBtn) m_chargeStartBtn->setEnabled(hasOrder && status == ecp::ORDER_RESERVED);
    if (m_chargeStopBtn) m_chargeStopBtn->setEnabled(hasOrder && status == ecp::ORDER_CHARGING);
    if (m_chargeSettleBtn) m_chargeSettleBtn->setEnabled(hasOrder && status == ecp::ORDER_TO_SETTLE);
    if (m_chargeCancelBtn) m_chargeCancelBtn->setEnabled(hasOrder && status == ecp::ORDER_RESERVED);
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
        if (m_pendingChargeUnfinishedSeq >= 0 && seq != m_pendingChargeUnfinishedSeq) {
            return;
        }
        m_pendingChargeUnfinishedSeq = -1;
        if (code != ecp::ERR_OK) {
            if (m_chargeStatus) m_chargeStatus->setText(msg);
            m_suppressChargeUnfinishedPrompt = false;
            return;
        }
        const bool hasUnfinished = data.value(QStringLiteral("hasUnfinished")).toBool();
        if (hasUnfinished) {
            setChargeOrder(data.value(QStringLiteral("order")).toObject());
            m_pendingChargeReservePileId = -1;
            if (!m_suppressChargeUnfinishedPrompt) {
                QMessageBox::information(this, QStringLiteral("提示"),
                                         QStringLiteral("您有未完成的充电订单，请先结算。"));
            }
            m_suppressChargeUnfinishedPrompt = false;
        } else {
            clearChargeOrder();
            if (m_pendingChargeReservePileId > 0) {
                const qint64 pileId = m_pendingChargeReservePileId;
                m_pendingChargeReservePileId = -1;
                sendChargeReserveRequest(pileId);
            }
        }
        updateChargeSummary();
        return;
    }

    if (cmd == ecp::CMD_ORDER_LIST) {
        if (m_pendingChargeOrdersSeq >= 0 && seq != m_pendingChargeOrdersSeq) {
            return;
        }
        m_pendingChargeOrdersSeq = -1;
        if (code != ecp::ERR_OK) {
            if (m_chargeStatus) m_chargeStatus->setText(msg);
            return;
        }
        m_chargeOrders = data.value(QStringLiteral("list")).toArray();
        renderChargeOrders();
        return;
    }

    if (cmd == ecp::CMD_ORDER_RESERVE) {
        if (m_pendingChargeReserveSeq >= 0 && seq != m_pendingChargeReserveSeq) return;
        m_pendingChargeReserveSeq = -1;
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        m_pendingChargeReservePileId = -1;
        m_chargeOrderId = data.value(QStringLiteral("orderId")).toVariant().toLongLong();
        setStatus(QStringLiteral("预约成功"));
        m_suppressChargeUnfinishedPrompt = true;
        requestChargeUnfinishedOrder();
        requestChargeOrders();
        return;
    }

    if (cmd == ecp::CMD_ORDER_CANCEL) {
        if (m_pendingChargeCancelSeq >= 0 && seq != m_pendingChargeCancelSeq) return;
        m_pendingChargeCancelSeq = -1;
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        m_pendingChargeReservePileId = -1;
        clearChargeOrder();
        m_suppressChargeUnfinishedPrompt = false;
        setStatus(QStringLiteral("预约已取消"));
        requestChargeUnfinishedOrder();
        requestChargeOrders();
        return;
    }

    if (cmd == ecp::CMD_ORDER_START) {
        if (m_pendingChargeStartSeq >= 0 && seq != m_pendingChargeStartSeq) return;
        m_pendingChargeStartSeq = -1;
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        if (!m_chargeOrder.isEmpty()) {
            m_chargeOrder[QStringLiteral("status")] = ecp::ORDER_CHARGING;
            m_chargeOrder[QStringLiteral("startTime")] = data.value(QStringLiteral("startTime"));
        }
        setStatus(QStringLiteral("充电已开始"));
        requestChargeOrders();
        updateChargeSummary();
        return;
    }

    if (cmd == ecp::CMD_ORDER_STOP) {
        if (m_pendingChargeStopSeq >= 0 && seq != m_pendingChargeStopSeq) return;
        m_pendingChargeStopSeq = -1;
        if (code != ecp::ERR_OK) {
            setStatus(msg, true);
            return;
        }
        if (!m_chargeOrder.isEmpty()) {
            m_chargeOrder[QStringLiteral("status")] = ecp::ORDER_TO_SETTLE;
            m_chargeOrder[QStringLiteral("endTime")] = data.value(QStringLiteral("endTime"));
            m_chargeOrder[QStringLiteral("kwh")] = data.value(QStringLiteral("kwh"));
            m_chargeOrder[QStringLiteral("amount")] = data.value(QStringLiteral("amount"));
        }
        setStatus(QStringLiteral("已结束充电，等待结算"));
        QMessageBox::information(this, QStringLiteral("提示"),
                                 QStringLiteral("本次充电已结束，请立即结算。"));
        requestChargeOrders();
        updateChargeSummary();
        return;
    }

    if (cmd == ecp::CMD_ORDER_SETTLE) {
        if (m_pendingChargeSettleSeq >= 0 && seq != m_pendingChargeSettleSeq) return;
        m_pendingChargeSettleSeq = -1;
        if (code != ecp::ERR_OK) {
            if (code == ecp::ERR_BALANCE_NOT_ENOUGH) {
                QMessageBox::warning(this, QStringLiteral("余额不足"), msg);
            } else {
                setStatus(msg, true);
            }
            return;
        }
        m_pendingChargeReservePileId = -1;
        if (data.contains(QStringLiteral("balance"))) {
            m_profile[QStringLiteral("balance")] = data.value(QStringLiteral("balance"));
            updateMineTexts();
        }
        clearChargeOrder();
        setStatus(QStringLiteral("结算成功"));
        requestChargeUnfinishedOrder();
        requestChargeOrders();
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

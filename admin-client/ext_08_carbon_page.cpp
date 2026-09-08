// -----------------------------------------------------------------------------
//  admin-client/ext_08_carbon_page.cpp  —  扩展模块 08 碳排放报告页　归属 L5
//  设计与口径见同名头文件与 docs/expand/08-实现规划.md 第 2、6 节。
// -----------------------------------------------------------------------------
#include "ext_08_carbon_page.h"
#include "net_client.h"
#include "protocol.h"
#include "protocol_ext.h"
#include "error_code.h"
#include "error_code_ext.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDateEdit>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

#ifdef HAVE_CHARTS
#include <QChart>
#include <QChartView>
#include <QDateTimeAxis>
#include <QDateTime>
#include <QLineSeries>
#include <QPainter>
#include <QValueAxis>
#endif

namespace {

constexpr int READ_RESPONSE_TIMEOUT_MS = 30000;   // 首次懒聚合要跑全量，比总览页给得宽
constexpr int NOT_APPLICABLE = -1;
const char *const DATE_FMT = "yyyy-MM-dd";

// 三处必须逐字一致：页眉、导出文件头、快照 JSON（实现规划第 2 节）
QString disclaimerText()
{
    return QStringLiteral("课程项目估算，非认证碳数据，不可用于碳交易或监管申报");
}

// 整数格式化两位小数。金额禁用 double（硬性规则 3），电量与排放同理 ——
// 显示层没有任何理由先把整数转成 double 再截断回去。
QString twoDecimals(qint64 hundredths)
{
    const qint64 sign = hundredths < 0 ? -1 : 1;
    const qint64 v = hundredths * sign;
    return QStringLiteral("%1%2.%3").arg(sign < 0 ? QStringLiteral("-") : QString())
                                    .arg(v / 100)
                                    .arg(v % 100, 2, 10, QLatin1Char('0'));
}

QString kwhText(qint64 kwhX100)
{
    return QStringLiteral("%1 度").arg(twoDecimals(kwhX100));
}

// 克 → 千克，保留两位。(g + 5) / 10 = 千克 ×100 的四舍五入整数写法
QString kgText(qint64 emissionG)
{
    return QStringLiteral("%1 kg").arg(twoDecimals((emissionG + 5) / 10));
}

// 占比，保留一位小数；分母为 0 时不显示 0.0% 而是「—」
QString shareText(qint64 part, qint64 total)
{
    if (total <= 0) return QStringLiteral("—");
    const qint64 tenths = (part * 1000 + total / 2) / total;
    return QStringLiteral("%1.%2%").arg(tenths / 10).arg(tenths % 10);
}

// 总量为 0 时服务端返回 -1「不适用」，页面必须照实显示，
// 不能把它当成 0 或 100 —— 那正是 00 第 4.5 节点名要防的粉饰
QString percentOrNa(int value)
{
    return value == NOT_APPLICABLE ? QStringLiteral("不适用") : QStringLiteral("%1%").arg(value);
}

QString intensityOrNa(qint64 value)
{
    return value == NOT_APPLICABLE ? QStringLiteral("不适用") : QStringLiteral("%1 g/度").arg(value);
}

QTableWidgetItem *cell(const QString &text, int align = Qt::AlignCenter)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(align);
    return item;
}

} // namespace

// =============================================================================
//  新增排放因子对话框
// =============================================================================
CarbonFactorDialog::CarbonFactorDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("新增排放因子版本"));
    setMinimumWidth(460);

    m_region  = new QLineEdit(this);
    m_region->setPlaceholderText(QStringLiteral("电网区域名，如 南方电网"));
    m_version = new QLineEdit(this);
    m_version->setPlaceholderText(QStringLiteral("版本号，如 2024，写入后不可修改"));
    m_source  = new QLineEdit(this);
    m_source->setPlaceholderText(QStringLiteral("数据来源说明（必填）"));

    m_factor = new QSpinBox(this);
    m_factor->setRange(1, 100000);            // 与建表 CHECK (factor_g_per_kwh > 0) 一致
    m_factor->setValue(581);
    m_factor->setSuffix(QStringLiteral(" g/度"));

    // 裁决 D6：边界必须对齐自然日。用 QDateEdit 后界面根本表达不出日内时刻。
    m_effectFrom = new QDateEdit(QDate::currentDate(), this);
    m_effectFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_effectFrom->setCalendarPopup(true);
    m_effectTo = new QDateEdit(QDate::currentDate().addYears(1), this);
    m_effectTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_effectTo->setCalendarPopup(true);

    m_noEndDate = new QCheckBox(QStringLiteral("无期限（右开无穷）"), this);
    m_noEndDate->setChecked(true);
    m_effectTo->setEnabled(false);
    connect(m_noEndDate, &QCheckBox::toggled, m_effectTo, &QDateEdit::setDisabled);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("电网区域"), m_region);
    form->addRow(QStringLiteral("版本号"),   m_version);
    form->addRow(QStringLiteral("排放因子"), m_factor);
    form->addRow(QStringLiteral("生效起"),   m_effectFrom);
    form->addRow(QStringLiteral("生效止"),   m_effectTo);
    form->addRow(QString(),                  m_noEndDate);
    form->addRow(QStringLiteral("来源说明"), m_source);

    auto *hint = new QLabel(QStringLiteral(
        "生效区间为左闭右开 [生效起, 生效止)，且只能落在自然日边界。\n"
        "区间不得与同区域已启用的因子重叠；版本一经写入不可修改，修订请发新版本号。"), this);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#667085"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &CarbonFactorDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &CarbonFactorDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(hint);
    layout->addWidget(buttons);
}

void CarbonFactorDialog::accept()
{
    const QString region  = m_region->text().trimmed();
    const QString version = m_version->text().trimmed();
    const QString source  = m_source->text().trimmed();
    if (region.isEmpty() || version.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("电网区域与版本号均为必填。"));
        return;
    }
    // 服务端同样会拒（08 第 9 节把来源不可靠列为风险），这里先挡一次省一个来回
    if (source.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("来源说明必填：没有出处的排放因子不允许入库。"));
        return;
    }
    if (!m_noEndDate->isChecked() && m_effectTo->date() <= m_effectFrom->date()) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("生效止必须晚于生效起。"));
        return;
    }

    m_data.region        = region;
    m_data.version       = version;
    m_data.source        = source;
    m_data.factorGPerKwh = m_factor->value();
    m_data.effectFrom    = m_effectFrom->date().toString(QLatin1String(DATE_FMT))
                         + QStringLiteral(" 00:00:00");
    m_data.effectTo      = m_noEndDate->isChecked()
                         ? QString()
                         : m_effectTo->date().toString(QLatin1String(DATE_FMT))
                           + QStringLiteral(" 00:00:00");
    QDialog::accept();
}

// =============================================================================
//  碳排放报告页
// =============================================================================
Ext08CarbonPage::Ext08CarbonPage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();

    // 每个在途请求一个超时定时器，与 overview_page 一致：
    // 服务端不回时页面要能自己收场，而不是永远停在「正在加载」
    const auto makeTimer = [this](int *seqField, LoadState *state, QString *error) {
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        connect(timer, &QTimer::timeout, this, [this, seqField, state, error] {
            if (*seqField < 0) return;
            *seqField = -1;
            if (state) *state = LoadState::Failed;
            if (error) *error = QStringLiteral("请求超时，请重试");
            else m_actionNote = QStringLiteral("操作超时，请重试");
            updateStatusLabel();
        });
        return timer;
    };
    m_metricTimer    = makeTimer(&m_metricSeq,    &m_metricState, &m_metricError);
    m_factorTimer    = makeTimer(&m_factorSeq,    &m_factorState, &m_factorError);
    m_aggregateTimer = makeTimer(&m_aggregateSeq, nullptr, nullptr);
    m_factorSetTimer = makeTimer(&m_factorSetSeq, nullptr, nullptr);
    m_stationTimer   = makeTimer(&m_stationSeq,   nullptr, nullptr);
    m_reportListTimer   = makeTimer(&m_reportListSeq,   nullptr, nullptr);
    m_reportGenTimer    = makeTimer(&m_reportGenSeq,    nullptr, nullptr);
    m_reportExportTimer = makeTimer(&m_reportExportSeq, nullptr, nullptr);

    connect(m_net, &NetClient::response, this, &Ext08CarbonPage::handleResponse);

    requestStationList();
    requestFactorList();
    requestReportList();
    requestMetric();
}

QWidget *Ext08CarbonPage::createHeaderBanner()
{
    auto *banner = new QFrame(this);
    banner->setFrameShape(QFrame::StyledPanel);
    banner->setStyleSheet(QStringLiteral(
        "QFrame { background:#fff7e6; border:1px solid #ffd591; border-radius:6px; }"
        "QLabel { border:none; color:#874d00; }"));
    auto *layout = new QVBoxLayout(banner);
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(4);

    auto *disclaimer = new QLabel(QStringLiteral("⚠ %1").arg(disclaimerText()), banner);
    QFont font = disclaimer->font();
    font.setBold(true);
    disclaimer->setFont(font);
    disclaimer->setWordWrap(true);
    layout->addWidget(disclaimer);

    // 模块 05 分时电价未落地，峰平谷走固定时段。口径降级必须写在脸上，
    // 否则读数的人会以为这是真实分时电价切分出来的结果。
    auto *tariff = new QLabel(QStringLiteral(
        "峰平谷采用「固定时段口径」：峰 10:00–15:00、18:00–21:00；谷 23:00–07:00；"
        "其余为平段（左闭右开）。跨午夜订单按真实钟点切分，日归属按结算时刻。"), banner);
    tariff->setWordWrap(true);
    layout->addWidget(tariff);

    m_provenanceLabel = new QLabel(QStringLiteral("因子版本 —　算法版本 —　数据截止 —"), banner);
    m_provenanceLabel->setWordWrap(true);
    layout->addWidget(m_provenanceLabel);
    return banner;
}

void Ext08CarbonPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(12);

    auto *titleLayout = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("碳排放报告"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    titleLayout->addWidget(title);
    titleLayout->addStretch();
    m_aggregateButton = new QPushButton(QStringLiteral("重算所选范围"), this);
    m_aggregateButton->setToolTip(QStringLiteral(
        "无条件重算该范围的日聚合。平时不必点 —— 查询本身就会自动发现迟到数据并重算。"));
    titleLayout->addWidget(m_aggregateButton);
    pageLayout->addLayout(titleLayout);

    pageLayout->addWidget(createHeaderBanner());

    m_statusLabel = new QLabel(QStringLiteral("准备加载碳排放指标"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#667085"));
    m_statusLabel->setWordWrap(true);
    pageLayout->addWidget(m_statusLabel);

    // ---- 筛选条 ----
    auto *filterLayout = new QHBoxLayout;
    filterLayout->setSpacing(8);
    filterLayout->addWidget(new QLabel(QStringLiteral("电站"), this));
    m_stationBox = new QComboBox(this);
    m_stationBox->addItem(QStringLiteral("全部电站"), 0);
    m_stationBox->setMinimumWidth(160);
    filterLayout->addWidget(m_stationBox);

    filterLayout->addSpacing(12);
    filterLayout->addWidget(new QLabel(QStringLiteral("起始"), this));
    m_dateFrom = new QDateEdit(QDate::currentDate().addDays(-29), this);
    m_dateFrom->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateFrom->setCalendarPopup(true);
    filterLayout->addWidget(m_dateFrom);

    filterLayout->addWidget(new QLabel(QStringLiteral("结束"), this));
    m_dateTo = new QDateEdit(QDate::currentDate(), this);
    m_dateTo->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_dateTo->setCalendarPopup(true);
    filterLayout->addWidget(m_dateTo);

    m_queryButton = new QPushButton(QStringLiteral("查询"), this);
    filterLayout->addWidget(m_queryButton);
    filterLayout->addStretch();
    pageLayout->addLayout(filterLayout);

    // ---- 指标卡 ----
    auto *metricsLayout = new QHBoxLayout;
    metricsLayout->setSpacing(16);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("总充电量"), m_totalKwh), 1);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("估算碳排放"), m_emission), 1);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("排放强度"), m_intensity), 1);
    metricsLayout->addWidget(createMetricCard(QStringLiteral("数据完整度"), m_completeness), 1);
    pageLayout->addLayout(metricsLayout);

    // ---- 主体：左趋势，右构成 + 因子 ----
    auto *contentLayout = new QHBoxLayout;
    contentLayout->setSpacing(16);

    auto *trendGroup = new QGroupBox(QStringLiteral("每日估算排放趋势"), this);
    auto *trendLayout = new QVBoxLayout(trendGroup);
#ifdef HAVE_CHARTS
    m_trendSeries = new QLineSeries;
    m_trendSeries->setName(QStringLiteral("估算排放（kg）"));
    m_trendChart = new QChart;
    m_trendChart->addSeries(m_trendSeries);
    m_trendChart->legend()->setVisible(false);

    m_trendAxisX = new QDateTimeAxis;
    m_trendAxisX->setFormat(QStringLiteral("MM-dd"));
    m_trendAxisX->setTitleText(QStringLiteral("日期"));
    m_trendAxisY = new QValueAxis;
    m_trendAxisY->setLabelFormat(QStringLiteral("%.1f"));
    m_trendAxisY->setTitleText(QStringLiteral("估算排放（kg）"));
    m_trendAxisY->setRange(0, 1);
    m_trendChart->addAxis(m_trendAxisX, Qt::AlignBottom);
    m_trendChart->addAxis(m_trendAxisY, Qt::AlignLeft);
    m_trendSeries->attachAxis(m_trendAxisX);
    m_trendSeries->attachAxis(m_trendAxisY);

    auto *chartView = new QChartView(m_trendChart, trendGroup);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setMinimumHeight(320);
    trendLayout->addWidget(chartView, 1);
#else
    // 与 overview_page 一致的降级：没有 QtCharts 就给表格，不是空白
    auto *warn = new QLabel(QStringLiteral(
        "⚠ QtCharts 未安装，趋势以表格呈现。\n"
        "安装：sudo apt install libqt6charts6-dev，然后重新 qmake6 && make"), trendGroup);
    warn->setWordWrap(true);
    warn->setStyleSheet(QStringLiteral("color:#a33;border:1px dashed #a33;padding:8px"));
    trendLayout->addWidget(warn);
#endif
    m_trendTable = new QTableWidget(0, 4, trendGroup);
    m_trendTable->setHorizontalHeaderLabels({ QStringLiteral("日期"), QStringLiteral("电量"),
                                              QStringLiteral("估算排放"), QStringLiteral("完整度") });
    m_trendTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_trendTable->verticalHeader()->setVisible(false);
    m_trendTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_trendTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_trendTable->setAlternatingRowColors(true);
#ifdef HAVE_CHARTS
    m_trendTable->setMaximumHeight(160);      // 有图时表格只作明细补充
#endif
    trendLayout->addWidget(m_trendTable, 1);
    contentLayout->addWidget(trendGroup, 3);

    auto *sideLayout = new QVBoxLayout;
    sideLayout->setSpacing(12);

    auto *shareGroup = new QGroupBox(QStringLiteral("峰平谷构成"), this);
    auto *shareLayout = new QVBoxLayout(shareGroup);
    m_shareTable = new QTableWidget(4, 3, shareGroup);
    m_shareTable->setHorizontalHeaderLabels({ QStringLiteral("时段"), QStringLiteral("电量"),
                                              QStringLiteral("占比") });
    m_shareTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_shareTable->verticalHeader()->setVisible(false);
    m_shareTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_shareTable->setSelectionMode(QAbstractItemView::NoSelection);
    m_shareTable->setAlternatingRowColors(true);
    shareLayout->addWidget(m_shareTable);
    sideLayout->addWidget(shareGroup);

    auto *factorGroup = new QGroupBox(QStringLiteral("排放因子版本"), this);
    auto *factorLayout = new QVBoxLayout(factorGroup);
    m_factorTable = new QTableWidget(0, 4, factorGroup);
    m_factorTable->setHorizontalHeaderLabels({ QStringLiteral("区域"), QStringLiteral("版本"),
                                               QStringLiteral("因子"), QStringLiteral("生效区间") });
    m_factorTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_factorTable->horizontalHeader()->setStretchLastSection(true);
    m_factorTable->verticalHeader()->setVisible(false);
    m_factorTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_factorTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_factorTable->setAlternatingRowColors(true);
    factorLayout->addWidget(m_factorTable, 1);
    m_addFactorButton = new QPushButton(QStringLiteral("新增因子版本"), factorGroup);
    factorLayout->addWidget(m_addFactorButton);
    sideLayout->addWidget(factorGroup, 1);

    contentLayout->addLayout(sideLayout, 2);
    pageLayout->addLayout(contentLayout, 1);

    // ---- 报告版本 ----
    auto *reportGroup = new QGroupBox(QStringLiteral("报告版本"), this);
    auto *reportLayout = new QVBoxLayout(reportGroup);
    m_reportTable = new QTableWidget(0, 8, reportGroup);
    m_reportTable->setHorizontalHeaderLabels({
        QStringLiteral("编号"), QStringLiteral("范围"), QStringLiteral("版本"),
        QStringLiteral("状态"), QStringLiteral("总电量"), QStringLiteral("估算排放"),
        QStringLiteral("生成时间"), QStringLiteral("导出文件") });
    m_reportTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_reportTable->horizontalHeader()->setStretchLastSection(true);
    m_reportTable->verticalHeader()->setVisible(false);
    m_reportTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_reportTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_reportTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_reportTable->setAlternatingRowColors(true);
    m_reportTable->setMaximumHeight(170);
    reportLayout->addWidget(m_reportTable);

    auto *reportToolbar = new QHBoxLayout;
    m_genReportButton = new QPushButton(QStringLiteral("按当前筛选生成报告"), reportGroup);
    m_exportCsvButton = new QPushButton(QStringLiteral("导出 CSV"), reportGroup);
    m_exportHtmlButton = new QPushButton(QStringLiteral("导出可打印 HTML"), reportGroup);
    reportToolbar->addWidget(m_genReportButton);
    reportToolbar->addWidget(m_exportCsvButton);
    reportToolbar->addWidget(m_exportHtmlButton);
    reportToolbar->addStretch();
    // 文件落盘不走 Socket（docs/protocol.md 第 2 节），因此路径是**服务端机器**上的位置。
    // 演示时管理端与服务端同机，这一条必须写在界面上，否则异机部署时用户会对着路径发愣。
    auto *pathHint = new QLabel(QStringLiteral(
        "导出文件写在服务端机器的 export/carbon/ 下（文件不走 Socket，仅返回路径）"), reportGroup);
    pathHint->setStyleSheet(QStringLiteral("color:#667085"));
    reportToolbar->addWidget(pathHint);
    reportLayout->addLayout(reportToolbar);
    pageLayout->addWidget(reportGroup);

    connect(m_queryButton,     &QPushButton::clicked, this, &Ext08CarbonPage::requestMetric);
    connect(m_aggregateButton, &QPushButton::clicked, this, &Ext08CarbonPage::requestAggregate);
    connect(m_addFactorButton, &QPushButton::clicked, this, &Ext08CarbonPage::openFactorDialog);
    connect(m_genReportButton,  &QPushButton::clicked, this, &Ext08CarbonPage::requestReportGen);
    connect(m_exportCsvButton,  &QPushButton::clicked, this,
            [this] { requestReportExport(QStringLiteral("csv"), false); });
    connect(m_exportHtmlButton, &QPushButton::clicked, this,
            [this] { requestReportExport(QStringLiteral("html"), false); });
}

QWidget *Ext08CarbonPage::createMetricCard(const QString &title, QLabel *&valueLabel)
{
    auto *card = new QFrame(this);
    card->setFrameShape(QFrame::StyledPanel);
    card->setStyleSheet(QStringLiteral(
        "QFrame { background:#f7f9fc; border:1px solid #dfe5ec; border-radius:6px; }"
        "QLabel { border:none; }"));
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(18, 14, 18, 14);

    auto *titleLabel = new QLabel(title, card);
    titleLabel->setStyleSheet(QStringLiteral("color:#667085"));
    valueLabel = new QLabel(QStringLiteral("—"), card);
    QFont valueFont = valueLabel->font();
    valueFont.setPointSize(16);
    valueFont.setBold(true);
    valueLabel->setFont(valueFont);

    layout->addWidget(titleLabel);
    layout->addWidget(valueLabel);
    return card;
}

// -----------------------------------------------------------------------------
//  请求
// -----------------------------------------------------------------------------
void Ext08CarbonPage::requestStationList()
{
    const int seq = m_net->send(ecp::CMD_STATION_LIST, QJsonObject{
        { QStringLiteral("page"), 1 }, { QStringLiteral("size"), 200 } });
    if (seq < 0) return;                      // 电站下拉拿不到不算致命，仍可查全部电站
    m_stationSeq = seq;
    m_stationTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

void Ext08CarbonPage::requestMetric()
{
    if (m_dateTo->date() < m_dateFrom->date()) {
        m_metricState = LoadState::Failed;
        m_metricError = QStringLiteral("结束日期不能早于起始日期");
        updateStatusLabel();
        return;
    }
    m_metricState = LoadState::Loading;
    m_metricError.clear();
    m_actionNote.clear();
    updateStatusLabel();

    const int seq = m_net->send(ecp::CMD_EXT_CARBON_METRIC, QJsonObject{
        { QStringLiteral("stationId"), m_stationBox->currentData().toInt() },
        { QStringLiteral("dateFrom"),  m_dateFrom->date().toString(QLatin1String(DATE_FMT)) },
        { QStringLiteral("dateTo"),    m_dateTo->date().toString(QLatin1String(DATE_FMT)) } });
    if (seq < 0) {
        m_metricTimer->stop();
        m_metricSeq = -1;
        m_metricState = LoadState::Failed;
        m_metricError = QStringLiteral("请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_metricSeq = seq;
    m_metricTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

void Ext08CarbonPage::requestFactorList()
{
    m_factorState = LoadState::Loading;
    m_factorError.clear();
    updateStatusLabel();
    const int seq = m_net->send(ecp::CMD_EXT_FACTOR_LIST);
    if (seq < 0) {
        m_factorTimer->stop();
        m_factorSeq = -1;
        m_factorState = LoadState::Failed;
        m_factorError = QStringLiteral("请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_factorSeq = seq;
    m_factorTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

void Ext08CarbonPage::requestAggregate()
{
    if (m_dateTo->date() < m_dateFrom->date()) return;
    m_aggregateButton->setEnabled(false);
    m_actionNote = QStringLiteral("正在重算所选范围…");
    updateStatusLabel();

    const int seq = m_net->send(ecp::CMD_EXT_CARBON_AGGREGATE, QJsonObject{
        { QStringLiteral("stationId"), m_stationBox->currentData().toInt() },
        { QStringLiteral("dateFrom"),  m_dateFrom->date().toString(QLatin1String(DATE_FMT)) },
        { QStringLiteral("dateTo"),    m_dateTo->date().toString(QLatin1String(DATE_FMT)) } });
    if (seq < 0) {
        m_aggregateButton->setEnabled(true);
        m_actionNote = QStringLiteral("重算请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_aggregateSeq = seq;
    m_aggregateTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

void Ext08CarbonPage::requestReportList()
{
    const int seq = m_net->send(ecp::CMD_EXT_REPORT_LIST);
    if (seq < 0) return;                      // 报告列表拿不到不影响看指标，不打断主流程
    m_reportListSeq = seq;
    m_reportListTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

void Ext08CarbonPage::requestReportGen()
{
    if (m_dateTo->date() < m_dateFrom->date()) return;
    const int stationId = m_stationBox->currentData().toInt();

    m_genReportButton->setEnabled(false);
    m_actionNote = QStringLiteral("正在生成报告…");
    updateStatusLabel();

    const int seq = m_net->send(ecp::CMD_EXT_REPORT_GEN, QJsonObject{
        { QStringLiteral("scope"),     stationId == 0 ? QStringLiteral("ALL")
                                                      : QStringLiteral("STATION") },
        { QStringLiteral("stationId"), stationId },
        { QStringLiteral("dateFrom"),  m_dateFrom->date().toString(QLatin1String(DATE_FMT)) },
        { QStringLiteral("dateTo"),    m_dateTo->date().toString(QLatin1String(DATE_FMT)) },
        // 幂等键（00 第 4.6 节）：重发不会多生成一个版本。每次点击换一个新的，
        // 因为「再点一次」的本意就是要一份新版本；防的是同一次点击被重复投递。
        { QStringLiteral("reqId"),     QUuid::createUuid().toString(QUuid::WithoutBraces) } });
    if (seq < 0) {
        m_genReportButton->setEnabled(true);
        m_actionNote = QStringLiteral("生成报告请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_reportGenSeq = seq;
    m_reportGenTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

int Ext08CarbonPage::selectedReportId() const
{
    const int row = m_reportTable->currentRow();
    if (row < 0 || !m_reportTable->item(row, 0)) return -1;
    return m_reportTable->item(row, 0)->text().toInt();
}

void Ext08CarbonPage::requestReportExport(const QString &format, bool allowStale)
{
    const int reportId = selectedReportId();
    if (reportId <= 0) {
        QMessageBox::information(this, QStringLiteral("导出报告"),
                                 QStringLiteral("请先在下方报告列表里选中一行。"));
        return;
    }
    m_pendingExportFormat = format;
    m_exportCsvButton->setEnabled(false);
    m_exportHtmlButton->setEnabled(false);
    m_actionNote = QStringLiteral("正在导出报告…");
    updateStatusLabel();

    QJsonObject data{ { QStringLiteral("reportId"), reportId },
                      { QStringLiteral("format"),   format } };
    if (allowStale) data[QStringLiteral("allowStale")] = true;

    const int seq = m_net->send(ecp::CMD_EXT_REPORT_EXPORT, data);
    if (seq < 0) {
        m_exportCsvButton->setEnabled(true);
        m_exportHtmlButton->setEnabled(true);
        m_actionNote = QStringLiteral("导出请求发送失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_reportExportSeq = seq;
    m_reportExportTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

void Ext08CarbonPage::openFactorDialog()
{
    CarbonFactorDialog dialog(this);
    if (dialog.exec() == QDialog::Accepted) submitFactor(dialog.formData());
}

void Ext08CarbonPage::submitFactor(const CarbonFactorForm &form)
{
    QJsonObject data{
        { QStringLiteral("region"),        form.region },
        { QStringLiteral("version"),       form.version },
        { QStringLiteral("source"),        form.source },
        { QStringLiteral("factorGPerKwh"), form.factorGPerKwh },
        { QStringLiteral("effectFrom"),    form.effectFrom }
    };
    if (!form.effectTo.isEmpty()) data[QStringLiteral("effectTo")] = form.effectTo;

    m_addFactorButton->setEnabled(false);
    m_actionNote = QStringLiteral("正在提交排放因子…");
    updateStatusLabel();

    const int seq = m_net->send(ecp::CMD_EXT_FACTOR_SET, data);
    if (seq < 0) {
        m_addFactorButton->setEnabled(true);
        m_actionNote = QStringLiteral("提交失败，请检查网络连接");
        updateStatusLabel();
        return;
    }
    m_factorSetSeq = seq;
    m_factorSetTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

// -----------------------------------------------------------------------------
//  响应
// -----------------------------------------------------------------------------
QString Ext08CarbonPage::describeError(int code, const QString &serverMsg)
{
    // 裁决 D5：6700 段的中文文案由页面本地映射。服务端的 msg 来自冻结的
    // errMsg()，对扩展码只会给「未知错误(NNNN)」—— 那不该端到用户面前。
    switch (code) {
    case ecp::ERR_CARBON_NO_FACTOR:
        return QStringLiteral("该时间段没有生效的排放因子，请先在右侧新增因子版本");
    case ecp::ERR_CARBON_FACTOR_OVERLAP:
        return QStringLiteral("生效区间与同区域已有因子重叠，请调整生效时间");
    case ecp::ERR_CARBON_REPORT_STALE:
        return QStringLiteral("源数据已变更，该报告需重新生成");
    default:
        break;
    }
    return serverMsg.trimmed().isEmpty() ? QStringLiteral("请求失败（错误码 %1）").arg(code)
                                         : serverMsg;
}

void Ext08CarbonPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                                     const QJsonObject &data)
{
    if (cmd == ecp::CMD_STATION_LIST && seq == m_stationSeq) {
        m_stationTimer->stop();
        m_stationSeq = -1;
        handleStationListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_EXT_CARBON_METRIC && seq == m_metricSeq) {
        m_metricTimer->stop();
        m_metricSeq = -1;
        handleMetricResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_EXT_FACTOR_LIST && seq == m_factorSeq) {
        m_factorTimer->stop();
        m_factorSeq = -1;
        handleFactorListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_EXT_CARBON_AGGREGATE && seq == m_aggregateSeq) {
        m_aggregateTimer->stop();
        m_aggregateSeq = -1;
        handleAggregateResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_EXT_FACTOR_SET && seq == m_factorSetSeq) {
        m_factorSetTimer->stop();
        m_factorSetSeq = -1;
        handleFactorSetResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_EXT_REPORT_LIST && seq == m_reportListSeq) {
        m_reportListTimer->stop();
        m_reportListSeq = -1;
        handleReportListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_EXT_REPORT_GEN && seq == m_reportGenSeq) {
        m_reportGenTimer->stop();
        m_reportGenSeq = -1;
        handleReportGenResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_EXT_REPORT_EXPORT && seq == m_reportExportSeq) {
        m_reportExportTimer->stop();
        m_reportExportSeq = -1;
        handleReportExportResponse(code, msg, data);
    }
}

void Ext08CarbonPage::handleReportListResponse(int code, const QString &msg,
                                               const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_actionNote = QStringLiteral("报告列表加载失败：%1").arg(describeError(code, msg));
        updateStatusLabel();
        return;
    }
    // 刚生成的报告优先选中；否则保持用户原来的选择
    const int keep = m_selectReportId > 0 ? m_selectReportId : selectedReportId();
    m_selectReportId = -1;
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
    m_reportTable->setRowCount(list.size());
    int row = 0, staleCount = 0, selectRow = -1;
    for (const QJsonValue &value : list) {
        const QJsonObject item = value.toObject();
        const int reportId  = item.value(QStringLiteral("reportId")).toInt();
        const QString status = item.value(QStringLiteral("status")).toString();
        const int stationId = item.value(QStringLiteral("stationId")).toInt();
        const bool stale = status == QLatin1String("STALE");
        if (stale) ++staleCount;

        m_reportTable->setItem(row, 0, cell(QString::number(reportId)));
        m_reportTable->setItem(row, 1, cell(QStringLiteral("%1　%2 ~ %3")
            .arg(item.value(QStringLiteral("scope")).toString() == QLatin1String("ALL")
                     ? QStringLiteral("全部电站") : QStringLiteral("电站 %1").arg(stationId))
            .arg(item.value(QStringLiteral("dateFrom")).toString(),
                 item.value(QStringLiteral("dateTo")).toString()),
            Qt::AlignLeft | Qt::AlignVCenter));
        m_reportTable->setItem(row, 2, cell(QStringLiteral("v%1")
            .arg(item.value(QStringLiteral("version")).toInt())));
        // STALE 角标：源数据变了，报告里的数字还是生成当时那批 —— 必须一眼看得出来
        auto *statusItem = cell(stale ? QStringLiteral("已过期") : QStringLiteral("可用"));
        if (stale) {
            statusItem->setForeground(QColor(QStringLiteral("#a8071a")));
            statusItem->setToolTip(QStringLiteral(
                "生成之后源数据发生了变化。报告里的数字仍是生成当时的快照，未被覆盖；"
                "需要最新数字请重新生成。"));
        }
        m_reportTable->setItem(row, 3, statusItem);
        m_reportTable->setItem(row, 4, cell(kwhText(item.value(QStringLiteral("totalKwhX100")).toInteger()),
                                            Qt::AlignRight | Qt::AlignVCenter));
        m_reportTable->setItem(row, 5, cell(kgText(item.value(QStringLiteral("emissionG")).toInteger()),
                                            Qt::AlignRight | Qt::AlignVCenter));
        m_reportTable->setItem(row, 6, cell(item.value(QStringLiteral("runTime")).toString()));
        const QString path = item.value(QStringLiteral("outputPath")).toString();
        m_reportTable->setItem(row, 7, cell(path.isEmpty() ? QStringLiteral("—") : path,
                                            Qt::AlignLeft | Qt::AlignVCenter));
        if (reportId == keep) selectRow = row;
        ++row;
    }
    if (selectRow >= 0) m_reportTable->selectRow(selectRow);
    else if (m_reportTable->rowCount() > 0 && keep <= 0) m_reportTable->selectRow(0);

    if (staleCount > 0) {
        m_actionNote = QStringLiteral("%1 份报告因源数据变化已标记为过期，需重新生成")
                           .arg(staleCount);
        updateStatusLabel();
    }
}

void Ext08CarbonPage::handleReportGenResponse(int code, const QString &msg,
                                              const QJsonObject &data)
{
    m_genReportButton->setEnabled(true);
    if (code != ecp::ERR_OK) {
        m_actionNote.clear();
        updateStatusLabel();
        QMessageBox::warning(this, QStringLiteral("生成报告失败"), describeError(code, msg));
        return;
    }
    m_selectReportId = data.value(QStringLiteral("reportId")).toInt();
    m_actionNote = QStringLiteral("已生成报告 %1 v%2")
                       .arg(m_selectReportId)
                       .arg(data.value(QStringLiteral("version")).toInt());
    updateStatusLabel();
    requestReportList();
}

void Ext08CarbonPage::handleReportExportResponse(int code, const QString &msg,
                                                 const QJsonObject &data)
{
    m_exportCsvButton->setEnabled(true);
    m_exportHtmlButton->setEnabled(true);

    // 6703：报告已过期。先把「这是旧数字」讲清楚，用户确认后再带 allowStale 重发 ——
    // 既不让人误把过期数字当成最新，也保住「旧数字仍可查」。
    if (code == ecp::ERR_CARBON_REPORT_STALE) {
        m_actionNote.clear();
        updateStatusLabel();
        const auto answer = QMessageBox::question(this, QStringLiteral("报告已过期"),
            QStringLiteral("该报告生成之后源数据发生了变化。\n\n"
                           "继续导出得到的是**生成当时**的数字，不是最新结果。\n"
                           "如需最新数字，请先重新生成报告。\n\n仍要导出这份历史快照吗？"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer == QMessageBox::Yes && !m_pendingExportFormat.isEmpty())
            requestReportExport(m_pendingExportFormat, /*allowStale=*/true);
        return;
    }
    if (code != ecp::ERR_OK) {
        m_actionNote.clear();
        updateStatusLabel();
        QMessageBox::warning(this, QStringLiteral("导出失败"), describeError(code, msg));
        return;
    }

    const QString path = data.value(QStringLiteral("path")).toString();
    const bool stale = data.value(QStringLiteral("stale")).toBool();
    m_actionNote = QStringLiteral("已导出到服务端 %1%2")
                       .arg(path, stale ? QStringLiteral("（历史快照，报告已过期）") : QString());
    updateStatusLabel();
    requestReportList();                      // 刷新「导出文件」列
}

void Ext08CarbonPage::handleStationListResponse(int code, const QString &,
                                                const QJsonObject &data)
{
    if (code != ecp::ERR_OK) return;          // 下拉退化为「全部电站」，不打断主流程
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
    const int keep = m_stationBox->currentData().toInt();
    m_stationBox->clear();
    m_stationBox->addItem(QStringLiteral("全部电站"), 0);
    for (const QJsonValue &value : list) {
        const QJsonObject item = value.toObject();
        const int stationId = item.value(QStringLiteral("stationId")).toInt();
        if (stationId <= 0) continue;
        m_stationBox->addItem(item.value(QStringLiteral("name")).toString(), stationId);
    }
    const int index = m_stationBox->findData(keep);
    if (index >= 0) m_stationBox->setCurrentIndex(index);
}

void Ext08CarbonPage::handleMetricResponse(int code, const QString &msg,
                                           const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_metricState = LoadState::Failed;
        m_metricError = describeError(code, msg);
        // 查询失败就把数字清干净。留着上一次的结果，旁边还挂着因子版本与数据截止时刻，
        // 看上去就像是当前范围算出来的 —— 这正是本模块最不该犯的错。
        clearMetrics();
        updateStatusLabel();
        return;
    }

    const QJsonObject totals = data.value(QStringLiteral("totals")).toObject();
    updateMetricCards(totals, data);
    updateShareTable(totals);

    QVector<DayPoint> points;
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
    m_trendTable->setRowCount(list.size());
    int row = 0;
    for (const QJsonValue &value : list) {
        const QJsonObject item = value.toObject();
        const QDate date = QDate::fromString(item.value(QStringLiteral("date")).toString(),
                                             QLatin1String(DATE_FMT));
        const qint64 emission = item.value(QStringLiteral("emissionG")).toInteger();
        if (date.isValid()) points.append({ date, emission });

        m_trendTable->setItem(row, 0, cell(item.value(QStringLiteral("date")).toString()));
        m_trendTable->setItem(row, 1, cell(kwhText(item.value(QStringLiteral("totalKwhX100")).toInteger()),
                                           Qt::AlignRight | Qt::AlignVCenter));
        m_trendTable->setItem(row, 2, cell(kgText(emission), Qt::AlignRight | Qt::AlignVCenter));
        m_trendTable->setItem(row, 3, cell(percentOrNa(item.value(QStringLiteral("completeness")).toInt())));
        ++row;
    }
    std::sort(points.begin(), points.end(),
              [](const DayPoint &a, const DayPoint &b) { return a.date < b.date; });
    updateTrend(points);

    m_metricState = LoadState::Success;
    m_metricError.clear();
    updateStatusLabel();
}

// 数字与出处一起清空：任何时候界面上的数字都必须有配套的因子版本与截止时刻
void Ext08CarbonPage::clearMetrics()
{
    for (QLabel *label : { m_totalKwh, m_emission, m_intensity, m_completeness })
        label->setText(QStringLiteral("—"));
    m_provenanceLabel->setText(QStringLiteral("因子版本 —　算法版本 —　数据截止 —"));
    m_trendTable->setRowCount(0);
    for (int row = 0; row < m_shareTable->rowCount(); ++row)
        for (int col = 0; col < m_shareTable->columnCount(); ++col)
            m_shareTable->setItem(row, col, cell(QStringLiteral("—")));
    updateTrend({});
}

void Ext08CarbonPage::updateMetricCards(const QJsonObject &totals, const QJsonObject &data)
{
    m_totalKwh->setText(kwhText(totals.value(QStringLiteral("totalKwhX100")).toInteger()));
    m_emission->setText(kgText(totals.value(QStringLiteral("emissionG")).toInteger()));
    m_intensity->setText(intensityOrNa(totals.value(QStringLiteral("intensityGPerKwh")).toInteger()));
    m_completeness->setText(percentOrNa(data.value(QStringLiteral("completeness")).toInt()));

    // 因子版本 / 算法版本 / 数据截止时刻必须随数字一起显示：
    // 没有这三项，报告里的数字就无从追溯是怎么算出来的（08 第 4.2 节不变量 4）
    const QString cutoff = data.value(QStringLiteral("cutoffTime")).toString();
    m_provenanceLabel->setText(QStringLiteral("因子版本 %1　算法版本 %2　数据截止 %3　口径 %4")
        .arg(data.value(QStringLiteral("factorVersion")).toString(QStringLiteral("—")))
        .arg(data.value(QStringLiteral("algoVersion")).toString(QStringLiteral("—")))
        .arg(cutoff.isEmpty() ? QStringLiteral("—") : cutoff)
        .arg(data.value(QStringLiteral("tariffMode")).toString() == QLatin1String("FIXED_RANGE")
             ? QStringLiteral("固定时段") : data.value(QStringLiteral("tariffMode")).toString()));
}

void Ext08CarbonPage::updateShareTable(const QJsonObject &totals)
{
    const qint64 total   = totals.value(QStringLiteral("totalKwhX100")).toInteger();
    const qint64 peak    = totals.value(QStringLiteral("peakKwhX100")).toInteger();
    const qint64 flat    = totals.value(QStringLiteral("flatKwhX100")).toInteger();
    const qint64 valley  = totals.value(QStringLiteral("valleyKwhX100")).toInteger();
    const qint64 unalloc = totals.value(QStringLiteral("unallocKwhX100")).toInteger();

    const QStringList names = { QStringLiteral("峰段"), QStringLiteral("平段"),
                                QStringLiteral("谷段"), QStringLiteral("未分摊") };
    const qint64 values[] = { peak, flat, valley, unalloc };
    for (int row = 0; row < 4; ++row) {
        m_shareTable->setItem(row, 0, cell(names.at(row)));
        m_shareTable->setItem(row, 1, cell(kwhText(values[row]), Qt::AlignRight | Qt::AlignVCenter));
        m_shareTable->setItem(row, 2, cell(shareText(values[row], total)));
    }
    // 未分摊不为零说明有订单缺起止时间，读数的人应当看见这件事而不是只看见一个漂亮的总数
    if (unalloc > 0) m_shareTable->item(3, 1)->setForeground(QColor(QStringLiteral("#a33")));
}

void Ext08CarbonPage::updateTrend(const QVector<DayPoint> &points)
{
#ifdef HAVE_CHARTS
    m_trendSeries->clear();
    if (points.isEmpty()) {
        m_trendAxisY->setRange(0, 1);
        m_trendAxisX->setRange(QDateTime(QDate::currentDate().addDays(-1), QTime(0, 0)),
                               QDateTime(QDate::currentDate(), QTime(0, 0)));
        return;
    }
    qint64 maxG = 0;
    for (const DayPoint &p : points) {
        m_trendSeries->append(QDateTime(p.date, QTime(0, 0)).toMSecsSinceEpoch(),
                              static_cast<double>(p.emissionG) / 1000.0);   // 仅绘图坐标，非业务量
        maxG = std::max(maxG, p.emissionG);
    }
    m_trendAxisX->setRange(QDateTime(points.first().date, QTime(0, 0)),
                           QDateTime(points.last().date, QTime(0, 0)));
    m_trendAxisY->setRange(0, maxG > 0 ? static_cast<double>(maxG) / 1000.0 * 1.1 : 1.0);
#else
    Q_UNUSED(points);
#endif
}

void Ext08CarbonPage::handleFactorListResponse(int code, const QString &msg,
                                               const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_factorState = LoadState::Failed;
        m_factorError = describeError(code, msg);
        updateStatusLabel();
        return;
    }
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
    m_factorTable->setRowCount(list.size());
    int row = 0;
    for (const QJsonValue &value : list) {
        const QJsonObject item = value.toObject();
        const QString to = item.value(QStringLiteral("effectTo")).toString();
        const bool enabled = item.value(QStringLiteral("enabled")).toInt() != 0;

        m_factorTable->setItem(row, 0, cell(item.value(QStringLiteral("region")).toString()));
        m_factorTable->setItem(row, 1, cell(item.value(QStringLiteral("version")).toString()
                                            + (enabled ? QString() : QStringLiteral("（停用）"))));
        m_factorTable->setItem(row, 2, cell(QStringLiteral("%1 g/度")
                                            .arg(item.value(QStringLiteral("factorGPerKwh")).toInt())));
        // 区间左闭右开，界面照实写出来 —— 这是不变量 5，含糊显示会让人以为是闭区间
        m_factorTable->setItem(row, 3, cell(QStringLiteral("[%1, %2)")
            .arg(item.value(QStringLiteral("effectFrom")).toString().left(10))
            .arg(to.isEmpty() ? QStringLiteral("无穷") : to.left(10)),
            Qt::AlignLeft | Qt::AlignVCenter));
        const QString source = item.value(QStringLiteral("source")).toString();
        for (int col = 0; col < 4; ++col) m_factorTable->item(row, col)->setToolTip(source);
        ++row;
    }
    m_factorState = LoadState::Success;
    m_factorError.clear();
    updateStatusLabel();
}

void Ext08CarbonPage::handleAggregateResponse(int code, const QString &msg,
                                              const QJsonObject &data)
{
    m_aggregateButton->setEnabled(true);
    if (code != ecp::ERR_OK) {
        m_actionNote = QStringLiteral("重算失败：%1").arg(describeError(code, msg));
        updateStatusLabel();
        return;
    }
    m_actionNote = QStringLiteral("已重算 %1 天 / %2 行")
                       .arg(data.value(QStringLiteral("days")).toInt())
                       .arg(data.value(QStringLiteral("rewritten")).toInt());
    requestMetric();                          // 重算完立刻刷新，避免页面还停在旧数字
    requestReportList();                      // 重算可能把某些报告置为 STALE，角标要同步
}

void Ext08CarbonPage::handleFactorSetResponse(int code, const QString &msg,
                                              const QJsonObject &data)
{
    m_addFactorButton->setEnabled(true);
    if (code != ecp::ERR_OK) {
        m_actionNote.clear();
        updateStatusLabel();
        QMessageBox::warning(this, QStringLiteral("新增因子失败"), describeError(code, msg));
        return;
    }
    m_actionNote = data.value(QStringLiteral("created")).toBool()
        ? QStringLiteral("已新增排放因子（factorId=%1）。受影响日期将在下次查询时自动重算")
              .arg(data.value(QStringLiteral("factorId")).toInt())
        : QStringLiteral("该因子版本已存在，内容一致，未重复写入");
    updateStatusLabel();
    requestFactorList();
}

void Ext08CarbonPage::updateStatusLabel()
{
    QStringList parts;
    if (m_metricState == LoadState::Loading) parts.append(QStringLiteral("正在加载碳排放指标…"));
    if (m_metricState == LoadState::Failed)
        parts.append(QStringLiteral("指标加载失败：%1").arg(m_metricError));
    if (m_factorState == LoadState::Failed)
        parts.append(QStringLiteral("因子列表加载失败：%1").arg(m_factorError));
    if (!m_actionNote.isEmpty()) parts.append(m_actionNote);

    if (parts.isEmpty()) {
        parts.append(m_metricState == LoadState::Success ? QStringLiteral("碳排放指标已更新")
                                                         : QStringLiteral("准备加载碳排放指标"));
    }
    m_statusLabel->setText(parts.join(QStringLiteral("　")));
}

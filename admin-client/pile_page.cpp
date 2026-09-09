#include "pile_page.h"
#include "net_client.h"
#include "loading_status.h"
#include "protocol.h"
#include <QAbstractItemView>
#include <QComboBox>
#include <QFont>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>

namespace {

constexpr int READ_RESPONSE_TIMEOUT_MS = 10000;
constexpr int WRITE_RESPONSE_TIMEOUT_MS = 10000;

QTableWidgetItem *centeredItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}

} // namespace

PilePage::PilePage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();
    m_stationOptionsTimer = new QTimer(this);
    m_stationOptionsTimer->setSingleShot(true);
    connect(m_stationOptionsTimer, &QTimer::timeout, this, [this] {
        if (m_stationOptionsSeq < 0) return;
        abortStationOptionsLoad(QStringLiteral("电站筛选项请求超时，请重试"));
    });

    m_pileListTimer = new QTimer(this);
    m_pileListTimer->setSingleShot(true);
    connect(m_pileListTimer, &QTimer::timeout, this, [this] {
        if (m_pileListSeq < 0) return;
        m_pileListSeq = -1;
        updateLoadingState();
        m_requestedPage = m_currentPage;
        m_requestedStationId = m_currentStationId;
        m_requestedStatus = m_currentStatus;
        m_statusLabel->setMessage(QStringLiteral("电桩列表请求超时，请重试"), LoadingStatus::Tone::Error);
        updatePaginationControls();
    });

    // 写操作单独一个定时器：超时后必须把按钮放回可用，否则一次丢包就再也点不动了
    m_pileRebootTimer = new QTimer(this);
    m_pileRebootTimer->setSingleShot(true);
    connect(m_pileRebootTimer, &QTimer::timeout, this, [this] {
        if (m_pileRebootSeq < 0) return;
        m_pileRebootSeq = -1;
        const QString code = m_pendingRebootCode;
        m_pendingRebootCode.clear();
        updateLoadingState();
        updateRebootButton();
        m_statusLabel->setMessage(
            QStringLiteral("电桩 %1 重启指令响应超时，请重试").arg(code),
            LoadingStatus::Tone::Error);
    });

    connect(m_net, &NetClient::response, this, &PilePage::handleResponse);
    requestStationOptions();
    requestPileList(1, 0, -1);
}

void PilePage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("充电桩管理"), this);
    title->setObjectName(QStringLiteral("PageTitle"));
    pageLayout->addWidget(title);

    auto *filterLayout = new QHBoxLayout;
    filterLayout->addWidget(new QLabel(QStringLiteral("所属电站"), this));
    m_stationFilter = new QComboBox(this);
    m_stationFilter->setMinimumWidth(190);
    m_stationFilter->addItem(QStringLiteral("全部"), qint64(0));
    filterLayout->addWidget(m_stationFilter);

    filterLayout->addSpacing(16);
    filterLayout->addWidget(new QLabel(QStringLiteral("状态"), this));
    m_statusFilter = new QComboBox(this);
    m_statusFilter->addItem(QStringLiteral("全部"), -1);
    m_statusFilter->addItem(QStringLiteral("在用"), ecp::PILE_IN_USE);
    m_statusFilter->addItem(QStringLiteral("闲置"), ecp::PILE_IDLE);
    m_statusFilter->addItem(QStringLiteral("故障"), ecp::PILE_FAULT);
    filterLayout->addWidget(m_statusFilter);

    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    searchButton->setObjectName(QStringLiteral("Primary"));
    auto *resetButton = new QPushButton(QStringLiteral("重置"), this);
    resetButton->setObjectName(QStringLiteral("Secondary"));
    filterLayout->addWidget(searchButton);
    filterLayout->addWidget(resetButton);
    filterLayout->addStretch();

    m_rebootButton = new QPushButton(QStringLiteral("远程重启"), this);
    m_rebootButton->setObjectName(QStringLiteral("Danger"));
    m_rebootButton->setEnabled(false);          // 未选中行时不可点，由 updateRebootButton() 接管
    m_rebootButton->setToolTip(QStringLiteral("请先在下方列表选中一台电桩"));
    filterLayout->addWidget(m_rebootButton);
    auto *filterCard = new QFrame(this);
    filterCard->setObjectName(QStringLiteral("Card"));
    filterLayout->setContentsMargins(16, 14, 16, 14);
    filterCard->setLayout(filterLayout);
    pageLayout->addWidget(filterCard);

    m_statusLabel = new LoadingStatus(QStringLiteral("准备加载电桩列表"), this);
    pageLayout->addWidget(m_statusLabel);

    m_table = new QTableWidget(0, 7, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("电桩编号"), QStringLiteral("所属电站"), QStringLiteral("类型"),
        QStringLiteral("功率（kW）"), QStringLiteral("当前状态"),
        QStringLiteral("累计充电次数"), QStringLiteral("累计充电时长")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setMouseTracking(true);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->setDefaultSectionSize(40);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    pageLayout->addWidget(m_table, 1);

    auto *pagination = new QHBoxLayout;
    m_previousPageButton = new QPushButton(QStringLiteral("上一页"), this);
    m_previousPageButton->setObjectName(QStringLiteral("Secondary"));
    m_pageLabel = new QLabel(this);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_nextPageButton = new QPushButton(QStringLiteral("下一页"), this);
    m_nextPageButton->setObjectName(QStringLiteral("Secondary"));
    pagination->addStretch();
    pagination->addWidget(m_previousPageButton);
    pagination->addWidget(m_pageLabel);
    pagination->addWidget(m_nextPageButton);
    pagination->addStretch();
    pageLayout->addLayout(pagination);
    updatePaginationControls();

    connect(searchButton, &QPushButton::clicked, this, [this] {
        requestPileList(1, m_stationFilter->currentData().toLongLong(),
                        m_statusFilter->currentData().toInt());
    });
    connect(resetButton, &QPushButton::clicked, this, &PilePage::resetFilters);
    connect(m_previousPageButton, &QPushButton::clicked, this, [this] {
        requestPileList(m_currentPage - 1, m_currentStationId, m_currentStatus);
    });
    connect(m_nextPageButton, &QPushButton::clicked, this, [this] {
        requestPileList(m_currentPage + 1, m_currentStationId, m_currentStatus);
    });
    connect(m_rebootButton, &QPushButton::clicked, this, &PilePage::requestPileReboot);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &PilePage::updateRebootButton);
}

void PilePage::requestStationOptions()
{
    m_stationOptionsTimer->stop();
    m_stationOptionsSeq = -1;
    updateLoadingState();
    m_stationOptionsPage = 0;
    m_stationOptionsTotal = 0;
    m_stationOptionsSelectedId = m_stationFilter->currentData().toLongLong();
    m_pendingStationOptions.clear();
    m_pendingStationOptionIds.clear();
    m_stationFilter->setToolTip(QStringLiteral("正在加载电站筛选项…"));
    requestStationOptionsPage(1);
}

void PilePage::requestStationOptionsPage(int page)
{
    const int seq = m_net->send(ecp::CMD_STATION_LIST, QJsonObject{
        { QStringLiteral("page"), page },
        { QStringLiteral("size"), STATION_OPTION_PAGE_SIZE }
    });
    if (seq < 0) {
        abortStationOptionsLoad(
            QStringLiteral("电站筛选项请求发送失败，请检查网络连接"));
        return;
    }
    m_stationOptionsSeq = seq;
    updateLoadingState();
    m_stationOptionsPage = page;
    m_stationOptionsTimer->start(READ_RESPONSE_TIMEOUT_MS);
}

void PilePage::abortStationOptionsLoad(const QString &message)
{
    m_stationOptionsTimer->stop();
    m_stationOptionsSeq = -1;
    updateLoadingState();
    m_stationOptionsPage = 0;
    m_stationOptionsTotal = 0;
    m_pendingStationOptions.clear();
    m_pendingStationOptionIds.clear();
    m_stationFilter->setToolTip(message);
}

void PilePage::commitStationOptions()
{
    const int acceptedCount = m_pendingStationOptions.size();
    const QSignalBlocker blocker(m_stationFilter);
    m_stationFilter->clear();
    m_stationFilter->addItem(QStringLiteral("全部"), qint64(0));
    for (const StationOption &option : m_pendingStationOptions)
        m_stationFilter->addItem(option.name, option.stationId);

    const int selectedIndex = m_stationFilter->findData(m_stationOptionsSelectedId);
    m_stationFilter->setCurrentIndex(selectedIndex >= 0 ? selectedIndex : 0);
    m_stationFilter->setToolTip(
        QStringLiteral("已加载 %1 / %2 个电站筛选项")
            .arg(acceptedCount).arg(m_stationOptionsTotal));

    m_stationOptionsPage = 0;
    m_pendingStationOptions.clear();
    m_pendingStationOptionIds.clear();
}

void PilePage::requestPileList(int page, qint64 stationId, int status)
{
    if (page < 1) return;

    m_statusLabel->setMessage(QStringLiteral("正在加载电桩列表…"), LoadingStatus::Tone::Loading);
    const int seq = m_net->send(ecp::CMD_PILE_LIST, QJsonObject{
        { QStringLiteral("page"), page },
        { QStringLiteral("size"), PAGE_SIZE },
        { QStringLiteral("stationId"), stationId },
        { QStringLiteral("status"), status }
    });
    if (seq < 0) {
        m_pileListTimer->stop();
        m_pileListSeq = -1;
        updateLoadingState();
        m_requestedPage = m_currentPage;
        m_requestedStationId = m_currentStationId;
        m_requestedStatus = m_currentStatus;
        m_statusLabel->setMessage(QStringLiteral("电桩列表请求发送失败，请检查网络连接"), LoadingStatus::Tone::Error);
        updatePaginationControls();
        return;
    }
    m_pileListSeq = seq;
    updateLoadingState();
    m_requestedPage = page;
    m_requestedStationId = stationId;
    m_requestedStatus = status;
    m_pileListTimer->start(READ_RESPONSE_TIMEOUT_MS);
    updatePaginationControls();
}

void PilePage::handleResponse(int cmd, int seq, int code, const QString &msg,
                              const QJsonObject &data)
{
    if (cmd == ecp::CMD_STATION_LIST) {
        if (seq != m_stationOptionsSeq) return;
        m_stationOptionsTimer->stop();
        m_stationOptionsSeq = -1;
        updateLoadingState();
        handleStationOptionsResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_PILE_LIST) {
        if (seq != m_pileListSeq) return;
        m_pileListTimer->stop();
        m_pileListSeq = -1;
        updateLoadingState();
        handlePileListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_PILE_REBOOT) {
        if (seq != m_pileRebootSeq) return;
        m_pileRebootTimer->stop();
        m_pileRebootSeq = -1;
        updateLoadingState();
        handlePileRebootResponse(code, msg);
    }
}

void PilePage::handleStationOptionsResponse(int code, const QString &msg,
                                            const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        abortStationOptionsLoad(
            QStringLiteral("电站筛选项加载失败：%1").arg(msg));
        return;
    }

    const QJsonValue totalValue = data.value(QStringLiteral("total"));
    const QJsonValue listValue = data.value(QStringLiteral("list"));
    const qint64 total = totalValue.toInteger(-1);
    if (!totalValue.isDouble() || total < 0 || !listValue.isArray()) {
        abortStationOptionsLoad(
            QStringLiteral("电站筛选项加载失败：服务器响应格式异常"));
        return;
    }

    m_stationOptionsTotal = total;
    const QJsonArray list = listValue.toArray();
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        const qint64 stationId = item.value(QStringLiteral("stationId")).toInteger();
        const QString name = item.value(QStringLiteral("name")).toString();
        if (stationId <= 0 || name.isEmpty()
            || m_pendingStationOptionIds.contains(stationId)) {
            continue;
        }
        m_pendingStationOptions.append({stationId, name});
        m_pendingStationOptionIds.insert(stationId);
    }

    if (qint64(m_stationOptionsPage) * STATION_OPTION_PAGE_SIZE < total) {
        requestStationOptionsPage(m_stationOptionsPage + 1);
        return;
    }
    commitStationOptions();
}

void PilePage::handlePileListResponse(int code, const QString &msg,
                                      const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_requestedPage = m_currentPage;
        m_requestedStationId = m_currentStationId;
        m_requestedStatus = m_currentStatus;
        m_statusLabel->setMessage(QStringLiteral("电桩列表加载失败：%1").arg(msg), LoadingStatus::Tone::Error);
        updatePaginationControls();
        return;
    }

    const QJsonValue totalValue = data.value(QStringLiteral("total"));
    const QJsonValue listValue = data.value(QStringLiteral("list"));
    const qint64 total = totalValue.toInteger(-1);
    if (!totalValue.isDouble() || total < 0 || !listValue.isArray()) {
        m_requestedPage = m_currentPage;
        m_requestedStationId = m_currentStationId;
        m_requestedStatus = m_currentStatus;
        m_statusLabel->setMessage(QStringLiteral("电桩列表加载失败：服务器响应格式异常"), LoadingStatus::Tone::Error);
        updatePaginationControls();
        return;
    }

    QVector<PileData> piles;
    const QJsonArray list = listValue.toArray();
    piles.reserve(list.size());
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        piles.append({
            item.value(QStringLiteral("pileId")).toInteger(),
            item.value(QStringLiteral("code")).toString(),
            item.value(QStringLiteral("stationName")).toString(),
            item.value(QStringLiteral("type")).toInt(),
            item.value(QStringLiteral("power")).toDouble(),
            item.value(QStringLiteral("status")).toInt(),
            item.value(QStringLiteral("chargeCount")).toInteger(),
            item.value(QStringLiteral("chargeDuration")).toInteger()
        });
    }

    m_piles = piles;
    m_currentPage = total == 0 ? 1 : m_requestedPage;
    m_total = total;
    m_currentStationId = m_requestedStationId;
    m_currentStatus = m_requestedStatus;
    m_requestedPage = m_currentPage;
    m_requestedStationId = m_currentStationId;
    m_requestedStatus = m_currentStatus;
    refreshTable();
    m_statusLabel->setMessage(QStringLiteral("已加载 %1 个电桩，共 %2 个")
                               .arg(m_piles.size()).arg(total), total == 0 ? LoadingStatus::Tone::Neutral : LoadingStatus::Tone::Success);
    updatePaginationControls();
}

// -----------------------------------------------------------------------------
//  [说明书] 1.4 远程重启（模拟向电桩发送重启指令），用于处理死机等异常
//  链路：管理端 2112 → 服务端校验管理员/查桩/判在线 → 写 t_pile_log + t_admin_oplog
//        → pushToDevice() 用 9003 推给电桩模拟器
//  服务端不改电桩状态（t_pile_log 的 old_status/new_status 均为 NULL），
//  所以这里的成功提示只能说「指令已下发」，不能说「已重启完成」。
// -----------------------------------------------------------------------------
const PilePage::PileData *PilePage::selectedPile() const
{
    const int row = m_table->currentRow();
    const QTableWidgetItem *codeItem = row >= 0 ? m_table->item(row, 0) : nullptr;
    if (!codeItem) return nullptr;

    const qint64 pileId = codeItem->data(Qt::UserRole).toLongLong();
    for (const PileData &pile : m_piles) {
        if (pile.pileId == pileId) return &pile;
    }
    return nullptr;
}

void PilePage::updateRebootButton()
{
    // 请求在途时一律禁用，避免同一台桩被连点出多条指令
    const bool busy = m_pileRebootSeq >= 0 || m_pileListSeq >= 0;
    const PileData *pile = selectedPile();

    m_rebootButton->setEnabled(!busy && pile != nullptr);
    if (busy) {
        m_rebootButton->setToolTip(QStringLiteral("请求处理中，请稍候"));
    } else if (!pile) {
        m_rebootButton->setToolTip(QStringLiteral("请先在下方列表选中一台电桩"));
    } else {
        m_rebootButton->setToolTip(
            QStringLiteral("向电桩 %1 下发远程重启指令").arg(pile->code));
    }
}

void PilePage::requestPileReboot()
{
    const PileData *pile = selectedPile();
    if (!pile) {
        QMessageBox::information(this, QStringLiteral("远程重启"),
                                 QStringLiteral("请先选择一台电桩"));
        return;
    }

    const QString code = pile->code;
    const qint64 pileId = pile->pileId;
    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认远程重启"),
        QStringLiteral("确定要重启电桩 %1 吗？\n重启期间该桩将短暂不可用。").arg(code),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    const int seq = m_net->send(ecp::CMD_PILE_REBOOT, QJsonObject{
        { QStringLiteral("pileId"), pileId }
    });
    if (seq < 0) {
        m_pileRebootTimer->stop();
        m_pileRebootSeq = -1;
        m_pendingRebootCode.clear();
        updateLoadingState();
        updateRebootButton();
        QMessageBox::warning(this, QStringLiteral("远程重启失败"),
                             QStringLiteral("请求发送失败，请检查网络连接"));
        return;
    }

    m_pileRebootSeq = seq;
    m_pendingRebootCode = code;
    updateLoadingState();
    updateRebootButton();
    m_pileRebootTimer->start(WRITE_RESPONSE_TIMEOUT_MS);
    m_statusLabel->setMessage(QStringLiteral("正在向电桩 %1 下发重启指令…").arg(code),
                              LoadingStatus::Tone::Loading);
}

void PilePage::handlePileRebootResponse(int code, const QString &msg)
{
    const QString pileCode = m_pendingRebootCode;
    m_pendingRebootCode.clear();

    if (code != ecp::ERR_OK) {
        // 3005 电桩离线是最常见的一种失败，服务端已给出可读文案，直接透传
        m_statusLabel->setMessage(
            QStringLiteral("电桩 %1 重启失败：%2").arg(pileCode, msg),
            LoadingStatus::Tone::Error);
        QMessageBox::warning(this, QStringLiteral("远程重启失败"), msg);
        updateRebootButton();
        return;
    }

    m_statusLabel->setMessage(QStringLiteral("电桩 %1 重启指令已下发").arg(pileCode),
                              LoadingStatus::Tone::Success);
    QMessageBox::information(this, QStringLiteral("操作成功"),
                             QStringLiteral("已向电桩 %1 下发重启指令").arg(pileCode));
    // 刷新列表：设备重启后会重新上报状态，顺带把操作前后的差异带回界面
    requestPileList(m_currentPage, m_currentStationId, m_currentStatus);
}

void PilePage::refreshTable()
{
    m_table->setRowCount(0);
    for (const PileData &pile : m_piles) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *codeItem = centeredItem(pile.code);
        codeItem->setData(Qt::UserRole, pile.pileId);
        m_table->setItem(row, 0, codeItem);
        m_table->setItem(row, 1, new QTableWidgetItem(pile.stationName));
        m_table->setItem(row, 2, centeredItem(typeText(pile.type)));
        m_table->setItem(row, 3, centeredItem(QString::number(pile.powerKw, 'f', 1)));
        m_table->setItem(row, 4, centeredItem(statusText(pile.status)));
        m_table->setItem(row, 5, centeredItem(QString::number(pile.chargeCount)));
        m_table->setItem(row, 6, centeredItem(durationText(pile.chargeDurationSeconds)));
    }

    m_table->clearSelection();
    updateRebootButton();
}

void PilePage::resetFilters()
{
    m_stationFilter->setCurrentIndex(0);
    m_statusFilter->setCurrentIndex(0);
    requestPileList(1, 0, -1);
}

void PilePage::updatePaginationControls()
{
    const qint64 totalPages = m_total > 0
        ? (m_total + PAGE_SIZE - 1) / PAGE_SIZE
        : 1;
    m_pageLabel->setText(QStringLiteral("第 %1 / %2 页，共 %3 个电桩")
                             .arg(m_currentPage).arg(totalPages).arg(m_total));

    const bool requestPending = m_pileListSeq >= 0;
    m_previousPageButton->setEnabled(!requestPending && m_currentPage > 1);
    m_nextPageButton->setEnabled(
        !requestPending && qint64(m_currentPage) * PAGE_SIZE < m_total);
}

QString PilePage::typeText(int type)
{
    switch (type) {
    case ecp::PILE_FAST: return QStringLiteral("快充");
    case ecp::PILE_SLOW: return QStringLiteral("慢充");
    default:             return QStringLiteral("未知");
    }
}

QString PilePage::statusText(int status)
{
    switch (status) {
    case ecp::PILE_IN_USE: return QStringLiteral("在用");
    case ecp::PILE_IDLE:   return QStringLiteral("闲置");
    case ecp::PILE_FAULT:  return QStringLiteral("故障");
    default:               return QStringLiteral("未知");
    }
}

QString PilePage::durationText(qint64 seconds)
{
    const qint64 hours = seconds / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (hours > 0) {
        return QStringLiteral("%1小时%2分钟").arg(hours).arg(minutes);
    }
    return QStringLiteral("%1分钟").arg(minutes);
}

void PilePage::updateLoadingState()
{
    m_statusLabel->setLoading(m_stationOptionsSeq >= 0 || m_pileListSeq >= 0
                              || m_pileRebootSeq >= 0);
    // 每次 seq 变化都会走到这里，重启按钮的可用性正好取决于同一组 seq，
    // 挂在此处比在各个请求分支里散着调更不容易漏
    updateRebootButton();
}

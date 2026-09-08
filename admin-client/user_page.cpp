#include "user_page.h"
#include "net_client.h"
#include "protocol.h"
#include "time_util.h"
#include <QAbstractItemView>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

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

UserPage::UserPage(NetClient *net, QWidget *parent)
    : QWidget(parent), m_net(net)
{
    setupUi();
    m_userListTimer = new QTimer(this);
    m_userListTimer->setSingleShot(true);
    connect(m_userListTimer, &QTimer::timeout, this, [this] {
        if (m_userListSeq < 0) return;
        m_userListSeq = -1;
        m_requestedPage = m_currentPage;
        m_requestedPhoneLike = m_currentPhoneLike;
        m_statusLabel->setText(m_userStatusReconcilePending
            ? QStringLiteral("用户状态更新结果仍未知；列表核对请求超时，请重试查询")
            : QStringLiteral("用户列表请求超时，请重试"));
        updatePaginationControls();
    });

    m_userStatusTimer = new QTimer(this);
    m_userStatusTimer->setSingleShot(true);
    connect(m_userStatusTimer, &QTimer::timeout, this, [this] {
        if (m_userStatusSeq < 0) return;
        m_userStatusSeq = -1;
        m_userStatusReconcilePending = true;
        updateStatusButton();
        requestUserList(m_currentPage, m_currentPhoneLike);
    });

    connect(m_net, &NetClient::response, this, &UserPage::handleResponse);
    requestUserList(1, QString());
}

void UserPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(12);

    auto *title = new QLabel(QStringLiteral("用户管理"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(18);
    titleFont.setBold(true);
    title->setFont(titleFont);
    pageLayout->addWidget(title);

    auto *toolbar = new QHBoxLayout;
    toolbar->addWidget(new QLabel(QStringLiteral("手机号"), this));
    m_phoneSearch = new QLineEdit(this);
    m_phoneSearch->setPlaceholderText(QStringLiteral("输入手机号片段"));
    m_phoneSearch->setMaxLength(11);
    m_phoneSearch->setClearButtonEnabled(true);
    m_phoneSearch->setMinimumWidth(220);
    toolbar->addWidget(m_phoneSearch);

    auto *searchButton = new QPushButton(QStringLiteral("查询"), this);
    auto *clearButton = new QPushButton(QStringLiteral("清空"), this);
    toolbar->addWidget(searchButton);
    toolbar->addWidget(clearButton);
    toolbar->addStretch();

    m_statusButton = new QPushButton(QStringLiteral("冻结用户"), this);
    m_statusButton->setEnabled(false);
    toolbar->addWidget(m_statusButton);
    pageLayout->addLayout(toolbar);

    m_statusLabel = new QLabel(QStringLiteral("准备加载用户列表"), this);
    m_statusLabel->setStyleSheet(QStringLiteral("color:#667085"));
    pageLayout->addWidget(m_statusLabel);

    m_table = new QTableWidget(0, 6, this);
    m_table->setHorizontalHeaderLabels({
        QStringLiteral("用户ID"), QStringLiteral("手机号"), QStringLiteral("昵称"),
        QStringLiteral("钱包余额（元）"), QStringLiteral("注册时间"), QStringLiteral("状态")
    });
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->verticalHeader()->setVisible(false);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    pageLayout->addWidget(m_table, 1);

    auto *pagination = new QHBoxLayout;
    m_previousPageButton = new QPushButton(QStringLiteral("上一页"), this);
    m_pageLabel = new QLabel(this);
    m_pageLabel->setAlignment(Qt::AlignCenter);
    m_nextPageButton = new QPushButton(QStringLiteral("下一页"), this);
    pagination->addStretch();
    pagination->addWidget(m_previousPageButton);
    pagination->addWidget(m_pageLabel);
    pagination->addWidget(m_nextPageButton);
    pagination->addStretch();
    pageLayout->addLayout(pagination);
    updatePaginationControls();

    connect(searchButton, &QPushButton::clicked, this, &UserPage::searchUsers);
    connect(m_phoneSearch, &QLineEdit::returnPressed, this, &UserPage::searchUsers);
    connect(clearButton, &QPushButton::clicked, this, &UserPage::clearSearch);
    connect(m_previousPageButton, &QPushButton::clicked, this, [this] {
        requestUserList(m_currentPage - 1, m_currentPhoneLike);
    });
    connect(m_nextPageButton, &QPushButton::clicked, this, [this] {
        requestUserList(m_currentPage + 1, m_currentPhoneLike);
    });
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &UserPage::updateStatusButton);
    connect(m_statusButton, &QPushButton::clicked,
            this, &UserPage::handleUserStatusChange);
}

void UserPage::requestUserList(int page, const QString &phoneLike)
{
    if (page < 1) return;

    const QString normalizedPhoneLike = phoneLike.trimmed();
    m_statusLabel->setText(m_userStatusReconcilePending
        ? QStringLiteral("用户状态更新响应超时，操作结果未知，正在刷新列表核对…")
        : QStringLiteral("正在加载用户列表…"));
    const int seq = m_net->send(ecp::CMD_ADMIN_USER_LIST, QJsonObject{
        { QStringLiteral("page"), page },
        { QStringLiteral("size"), PAGE_SIZE },
        { QStringLiteral("phoneLike"), normalizedPhoneLike }
    });
    if (seq < 0) {
        m_userListTimer->stop();
        m_userListSeq = -1;
        m_requestedPage = m_currentPage;
        m_requestedPhoneLike = m_currentPhoneLike;
        m_statusLabel->setText(m_userStatusReconcilePending
            ? QStringLiteral("用户状态更新结果仍未知；列表核对失败：请求发送失败，请检查网络连接")
            : QStringLiteral("用户列表请求发送失败，请检查网络连接"));
        updatePaginationControls();
        return;
    }
    m_userListSeq = seq;
    m_requestedPage = page;
    m_requestedPhoneLike = normalizedPhoneLike;
    m_userListTimer->start(READ_RESPONSE_TIMEOUT_MS);
    updatePaginationControls();
}

void UserPage::searchUsers()
{
    requestUserList(1, m_phoneSearch->text());
}

void UserPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                              const QJsonObject &data)
{
    if (cmd == ecp::CMD_ADMIN_USER_LIST) {
        if (seq != m_userListSeq) return;
        m_userListTimer->stop();
        m_userListSeq = -1;
        handleUserListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_ADMIN_USER_STATUS) {
        if (seq != m_userStatusSeq) return;
        m_userStatusTimer->stop();
        m_userStatusSeq = -1;
        m_userStatusReconcilePending = false;
        handleUserStatusResponse(code, msg);
    }
}

void UserPage::handleUserListResponse(int code, const QString &msg,
                                      const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_requestedPage = m_currentPage;
        m_requestedPhoneLike = m_currentPhoneLike;
        m_statusLabel->setText(m_userStatusReconcilePending
            ? QStringLiteral("用户状态更新结果仍未知；列表核对失败：%1").arg(msg)
            : QStringLiteral("用户列表加载失败：%1").arg(msg));
        updatePaginationControls();
        return;
    }

    const QJsonValue totalValue = data.value(QStringLiteral("total"));
    const QJsonValue listValue = data.value(QStringLiteral("list"));
    const qint64 total = totalValue.toInteger(-1);
    if (!totalValue.isDouble() || total < 0 || !listValue.isArray()) {
        m_requestedPage = m_currentPage;
        m_requestedPhoneLike = m_currentPhoneLike;
        m_statusLabel->setText(m_userStatusReconcilePending
            ? QStringLiteral("用户状态更新结果仍未知；列表核对失败：服务器响应格式异常")
            : QStringLiteral("用户列表加载失败：服务器响应格式异常"));
        updatePaginationControls();
        return;
    }

    QVector<UserData> users;
    const QJsonArray list = listValue.toArray();
    users.reserve(list.size());
    for (const QJsonValue &value : list) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        users.append({
            item.value(QStringLiteral("userId")).toInteger(),
            item.value(QStringLiteral("phone")).toString(),
            item.value(QStringLiteral("nickname")).toString(),
            item.value(QStringLiteral("balance")).toInteger(),
            item.value(QStringLiteral("createTime")).toString(),
            item.value(QStringLiteral("status")).toInt()
        });
    }

    QString reconcileMessage;
    if (m_userStatusReconcilePending) {
        const auto target = std::find_if(users.cbegin(), users.cend(), [this](const UserData &user) {
            return user.userId == m_pendingStatusUserId;
        });
        if (target == users.cend()) {
            reconcileMessage = QStringLiteral(
                "用户状态更新结果仍未知，当前列表未定位到用户 %1，请搜索该手机号继续核对")
                                   .arg(m_pendingStatusPhone);
        } else {
            const QString phone = m_pendingStatusPhone;
            const int targetStatus = m_pendingNewStatus;
            const QString action = targetStatus == ecp::USER_FROZEN
                ? QStringLiteral("冻结") : QStringLiteral("解冻");
            const bool reachedTarget = target->status == targetStatus;
            m_userStatusReconcilePending = false;
            m_pendingStatusUserId = 0;
            m_pendingStatusPhone.clear();
            m_pendingNewStatus = ecp::USER_NORMAL;
            reconcileMessage = reachedTarget
                ? QStringLiteral("用户 %1 状态更新响应超时，但回读确认已%2")
                      .arg(phone, action)
                : QStringLiteral("用户 %1 状态更新未达到目标状态，请确认后重试")
                      .arg(phone);
        }
    }

    m_users = users;
    m_currentPage = total == 0 ? 1 : m_requestedPage;
    m_total = total;
    m_currentPhoneLike = m_requestedPhoneLike;
    m_requestedPage = m_currentPage;
    m_requestedPhoneLike = m_currentPhoneLike;
    refreshTable();
    m_statusLabel->setText(reconcileMessage.isEmpty()
        ? QStringLiteral("已加载 %1 个用户，共 %2 个")
              .arg(m_users.size()).arg(total)
        : reconcileMessage);
    updatePaginationControls();
}

void UserPage::handleUserStatusResponse(int code, const QString &msg)
{
    const QString phone = m_pendingStatusPhone;
    const int newStatus = m_pendingNewStatus;
    m_pendingStatusUserId = 0;
    m_pendingStatusPhone.clear();
    m_pendingNewStatus = ecp::USER_NORMAL;

    if (code != ecp::ERR_OK) {
        m_statusLabel->setText(QStringLiteral("用户状态更新失败：%1").arg(msg));
        QMessageBox::warning(this, QStringLiteral("用户状态更新失败"), msg);
        updateStatusButton();
        return;
    }

    m_table->clearSelection();
    updateStatusButton();
    const QString action = newStatus == ecp::USER_FROZEN
        ? QStringLiteral("冻结") : QStringLiteral("解冻");
    QMessageBox::information(this, QStringLiteral("操作成功"),
                             QStringLiteral("用户 %1 已%2").arg(phone, action));
    requestUserList(m_currentPage, m_currentPhoneLike);
}

void UserPage::refreshTable()
{
    m_table->setRowCount(0);
    for (const UserData &user : m_users) {
        const int row = m_table->rowCount();
        m_table->insertRow(row);
        auto *idItem = centeredItem(QString::number(user.userId));
        idItem->setData(Qt::UserRole, user.userId);
        m_table->setItem(row, 0, idItem);
        m_table->setItem(row, 1, centeredItem(user.phone));
        m_table->setItem(row, 2, new QTableWidgetItem(user.nickname));
        m_table->setItem(row, 3, centeredItem(ecp::fenToYuan(user.balanceFen)));
        m_table->setItem(row, 4, centeredItem(user.createTime));
        m_table->setItem(row, 5, centeredItem(statusText(user.status)));
    }

    m_table->clearSelection();
    updateStatusButton();
}

void UserPage::clearSearch()
{
    m_phoneSearch->clear();
    requestUserList(1, QString());
    m_phoneSearch->setFocus();
}

void UserPage::updateStatusButton()
{
    const UserData *user = selectedUser();
    const bool waitingForStatus = m_userStatusSeq >= 0
        || m_userStatusReconcilePending;
    m_statusButton->setEnabled(user != nullptr && !waitingForStatus);
    m_statusButton->setText(user && user->status == ecp::USER_FROZEN
        ? QStringLiteral("解冻用户")
        : QStringLiteral("冻结用户"));
}

void UserPage::updatePaginationControls()
{
    const qint64 totalPages = m_total > 0
        ? (m_total + PAGE_SIZE - 1) / PAGE_SIZE
        : 1;
    m_pageLabel->setText(QStringLiteral("第 %1 / %2 页，共 %3 个用户")
                             .arg(m_currentPage).arg(totalPages).arg(m_total));

    const bool requestPending = m_userListSeq >= 0;
    m_previousPageButton->setEnabled(!requestPending && m_currentPage > 1);
    m_nextPageButton->setEnabled(
        !requestPending && qint64(m_currentPage) * PAGE_SIZE < m_total);
}

void UserPage::handleUserStatusChange()
{
    const UserData *user = selectedUser();
    if (!user) {
        QMessageBox::information(this, QStringLiteral("用户状态"),
                                 QStringLiteral("请先选择一个用户"));
        return;
    }

    const bool freezing = user->status == ecp::USER_NORMAL;
    const int newStatus = freezing ? ecp::USER_FROZEN : ecp::USER_NORMAL;
    const QString action = freezing ? QStringLiteral("冻结") : QStringLiteral("解冻");
    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认%1用户").arg(action),
        QStringLiteral("确定要%1用户 %2 吗？").arg(action, user->phone),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    const int seq = m_net->send(ecp::CMD_ADMIN_USER_STATUS, QJsonObject{
        { QStringLiteral("userId"), user->userId },
        { QStringLiteral("status"), newStatus }
    });
    if (seq < 0) {
        m_userStatusTimer->stop();
        m_userStatusSeq = -1;
        m_userStatusReconcilePending = false;
        QMessageBox::warning(this, QStringLiteral("用户状态更新失败"),
                             QStringLiteral("请求发送失败，请检查网络连接"));
        return;
    }

    m_userStatusSeq = seq;
    m_pendingStatusUserId = user->userId;
    m_pendingStatusPhone = user->phone;
    m_pendingNewStatus = newStatus;
    m_userStatusReconcilePending = false;
    m_userStatusTimer->start(WRITE_RESPONSE_TIMEOUT_MS);
    m_statusButton->setEnabled(false);
    m_statusLabel->setText(QStringLiteral("正在%1用户 %2…").arg(action, user->phone));
}

const UserPage::UserData *UserPage::selectedUser() const
{
    const int row = m_table->currentRow();
    const QTableWidgetItem *idItem = row >= 0 ? m_table->item(row, 0) : nullptr;
    if (!idItem) return nullptr;

    const qint64 userId = idItem->data(Qt::UserRole).toLongLong();
    for (const UserData &user : m_users) {
        if (user.userId == userId) return &user;
    }
    return nullptr;
}

QString UserPage::statusText(int status)
{
    switch (status) {
    case ecp::USER_NORMAL: return QStringLiteral("正常");
    case ecp::USER_FROZEN: return QStringLiteral("冻结");
    default:               return QStringLiteral("未知");
    }
}

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
#include <QVBoxLayout>

namespace {

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
    connect(m_net, &NetClient::response, this, &UserPage::handleResponse);
    requestUserList();
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

    connect(searchButton, &QPushButton::clicked, this, &UserPage::requestUserList);
    connect(m_phoneSearch, &QLineEdit::returnPressed, this, &UserPage::requestUserList);
    connect(clearButton, &QPushButton::clicked, this, &UserPage::clearSearch);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &UserPage::updateStatusButton);
    connect(m_statusButton, &QPushButton::clicked,
            this, &UserPage::handleUserStatusChange);
}

void UserPage::requestUserList()
{
    const QString phoneLike = m_phoneSearch->text().trimmed();
    m_statusLabel->setText(QStringLiteral("正在加载用户列表…"));
    const int seq = m_net->send(ecp::CMD_ADMIN_USER_LIST, QJsonObject{
        { QStringLiteral("page"), 1 },
        { QStringLiteral("size"), 100 },
        { QStringLiteral("phoneLike"), phoneLike }
    });
    if (seq < 0) {
        m_userListSeq = -1;
        m_statusLabel->setText(QStringLiteral("用户列表请求发送失败，请检查网络连接"));
        return;
    }
    m_userListSeq = seq;
}

void UserPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                              const QJsonObject &data)
{
    if (cmd == ecp::CMD_ADMIN_USER_LIST) {
        if (seq != m_userListSeq) return;
        m_userListSeq = -1;
        handleUserListResponse(code, msg, data);
        return;
    }
    if (cmd == ecp::CMD_ADMIN_USER_STATUS) {
        if (seq != m_userStatusSeq) return;
        m_userStatusSeq = -1;
        handleUserStatusResponse(code, msg);
    }
}

void UserPage::handleUserListResponse(int code, const QString &msg,
                                      const QJsonObject &data)
{
    if (code != ecp::ERR_OK) {
        m_statusLabel->setText(QStringLiteral("用户列表加载失败：%1").arg(msg));
        return;
    }

    QVector<UserData> users;
    const QJsonArray list = data.value(QStringLiteral("list")).toArray();
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

    m_users = users;
    refreshTable();
    const qint64 total = data.value(QStringLiteral("total")).toInteger(m_users.size());
    m_statusLabel->setText(QStringLiteral("已加载 %1 个用户，共 %2 个")
                               .arg(m_users.size()).arg(total));
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
    requestUserList();
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
    requestUserList();
    m_phoneSearch->setFocus();
}

void UserPage::updateStatusButton()
{
    const UserData *user = selectedUser();
    const bool waitingForStatus = m_userStatusSeq >= 0;
    m_statusButton->setEnabled(user != nullptr && !waitingForStatus);
    m_statusButton->setText(user && user->status == ecp::USER_FROZEN
        ? QStringLiteral("解冻用户")
        : QStringLiteral("冻结用户"));
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
        QMessageBox::warning(this, QStringLiteral("用户状态更新失败"),
                             QStringLiteral("请求发送失败，请检查网络连接"));
        return;
    }

    m_userStatusSeq = seq;
    m_pendingStatusUserId = user->userId;
    m_pendingStatusPhone = user->phone;
    m_pendingNewStatus = newStatus;
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

#include "user_page.h"
#include "protocol.h"
#include "time_util.h"
#include <QAbstractItemView>
#include <QFont>
#include <QHeaderView>
#include <QHBoxLayout>
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
    loadMockData();
}

void UserPage::setupUi()
{
    auto *pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(24, 20, 24, 20);
    pageLayout->setSpacing(16);

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

    connect(searchButton, &QPushButton::clicked, this, &UserPage::refreshTable);
    connect(m_phoneSearch, &QLineEdit::returnPressed, this, &UserPage::refreshTable);
    connect(clearButton, &QPushButton::clicked, this, &UserPage::clearSearch);
    connect(m_table, &QTableWidget::itemSelectionChanged,
            this, &UserPage::updateStatusButton);
    connect(m_statusButton, &QPushButton::clicked,
            this, &UserPage::handleUserStatusChange);
}

void UserPage::loadMockData()
{
    // Mock 数据集中于此，待 2201 接入后替换为服务端响应。
    m_users = {
        { 1,  QStringLiteral("13800138001"), QStringLiteral("用户8001"), 20000,
          QStringLiteral("2026-07-03 09:12:30"), ecp::USER_NORMAL },
        { 2,  QStringLiteral("13800138002"), QStringLiteral("用户8002"), 5000,
          QStringLiteral("2026-07-04 11:28:16"), ecp::USER_NORMAL },
        { 3,  QStringLiteral("13900139003"), QStringLiteral("南山车主"), 12680,
          QStringLiteral("2026-07-08 18:05:42"), ecp::USER_NORMAL },
        { 4,  QStringLiteral("13800138004"), QStringLiteral("用户8004"), 150,
          QStringLiteral("2026-07-12 08:44:09"), ecp::USER_NORMAL },
        { 5,  QStringLiteral("13600136005"), QStringLiteral("鹏城小李"), 30800,
          QStringLiteral("2026-07-18 14:21:55"), ecp::USER_NORMAL },
        { 6,  QStringLiteral("13800138006"), QStringLiteral("用户8006"), 8000,
          QStringLiteral("2026-07-23 20:16:38"), ecp::USER_FROZEN },
        { 7,  QStringLiteral("13700137007"), QStringLiteral("福田通勤族"), 15640,
          QStringLiteral("2026-08-01 07:35:24"), ecp::USER_NORMAL },
        { 8,  QStringLiteral("13500135008"), QStringLiteral("新能源达人"), 42150,
          QStringLiteral("2026-08-05 16:48:11"), ecp::USER_NORMAL },
        { 9,  QStringLiteral("18800188009"), QStringLiteral("湾区出行"), 980,
          QStringLiteral("2026-08-09 10:02:47"), ecp::USER_FROZEN },
        { 10, QStringLiteral("13812345610"), QStringLiteral("用户5610"), 24300,
          QStringLiteral("2026-08-13 13:19:05"), ecp::USER_NORMAL },
        { 11, QStringLiteral("18600186011"), QStringLiteral("宝安车友"), 6750,
          QStringLiteral("2026-08-18 19:27:33"), ecp::USER_NORMAL },
        { 12, QStringLiteral("13876543212"), QStringLiteral("用户3212"), 11200,
          QStringLiteral("2026-08-22 12:08:58"), ecp::USER_NORMAL },
        { 13, QStringLiteral("15900159013"), QStringLiteral("龙岗小陈"), 35680,
          QStringLiteral("2026-08-27 17:42:20"), ecp::USER_FROZEN },
        { 14, QStringLiteral("13888888814"), QStringLiteral("用户8814"), 18600,
          QStringLiteral("2026-09-01 09:56:14"), ecp::USER_NORMAL }
    };

    refreshTable();
}

void UserPage::refreshTable()
{
    const QString phoneLike = m_phoneSearch->text().trimmed();

    // TODO(L3)：服务端实现后，将本地筛选替换为 2201 请求：
    // {page, size, phoneLike}。当前不得发送真实网络请求。
    m_table->setRowCount(0);
    for (const UserMock &user : m_users) {
        if (!phoneLike.isEmpty() && !user.phone.contains(phoneLike)) continue;

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
    refreshTable();
    m_phoneSearch->setFocus();
}

void UserPage::updateStatusButton()
{
    const UserMock *user = selectedUser();
    m_statusButton->setEnabled(user != nullptr);
    m_statusButton->setText(user && user->status == ecp::USER_FROZEN
        ? QStringLiteral("解冻用户")
        : QStringLiteral("冻结用户"));
}

void UserPage::handleUserStatusChange()
{
    UserMock *user = selectedUser();
    if (!user) {
        QMessageBox::information(this, QStringLiteral("用户状态"),
                                 QStringLiteral("请先选择一个用户"));
        return;
    }

    const bool freezing = user->status == ecp::USER_NORMAL;
    const QString action = freezing ? QStringLiteral("冻结") : QStringLiteral("解冻");
    const auto answer = QMessageBox::question(
        this, QStringLiteral("确认%1用户").arg(action),
        QStringLiteral("确定要%1用户 %2 吗？").arg(action, user->phone),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    const int userId = user->userId;
    const QString phone = user->phone;
    const int newStatus = freezing ? ecp::USER_FROZEN : ecp::USER_NORMAL;

    // TODO(L3)：服务端实现后替换为 2202 请求：{userId, status}。
    // 当前仅修改本地 Mock 数据，不发送真实网络请求。
    user->status = newStatus;
    refreshTable();
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *idItem = m_table->item(row, 0);
        if (idItem && idItem->data(Qt::UserRole).toInt() == userId) {
            m_table->selectRow(row);
            m_table->scrollToItem(idItem);
            break;
        }
    }

    QMessageBox::information(this, QStringLiteral("操作成功"),
                             QStringLiteral("用户 %1 已%2").arg(phone, action));
}

UserPage::UserMock *UserPage::selectedUser()
{
    const int row = m_table->currentRow();
    const QTableWidgetItem *idItem = row >= 0 ? m_table->item(row, 0) : nullptr;
    if (!idItem) return nullptr;

    const int userId = idItem->data(Qt::UserRole).toInt();
    for (UserMock &user : m_users) {
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

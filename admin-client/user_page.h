#pragma once
// -----------------------------------------------------------------------------
//  admin-client/user_page.h  —  PC 管理端用户管理页
//  归属 L3。 [说明书] 1.4 用户列表、手机号模糊搜索与冻结/解冻
// -----------------------------------------------------------------------------
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QLineEdit;
class QPushButton;
class QTableWidget;

class UserPage : public QWidget
{
public:
    explicit UserPage(NetClient *net, QWidget *parent = nullptr);

private:
    struct UserMock
    {
        int userId = 0;
        QString phone;
        QString nickname;
        qint64 balanceFen = 0;
        QString createTime;
        int status = 0;
    };

    void setupUi();
    void loadMockData();
    void refreshTable();
    void clearSearch();
    void updateStatusButton();
    void handleUserStatusChange();
    UserMock *selectedUser();

    static QString statusText(int status);

    NetClient    *m_net = nullptr;
    QLineEdit    *m_phoneSearch = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton  *m_statusButton = nullptr;
    QVector<UserMock> m_users;
};

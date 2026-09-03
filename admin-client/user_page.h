#pragma once
// -----------------------------------------------------------------------------
//  admin-client/user_page.h  —  PC 管理端用户管理页
//  归属 L3。 [说明书] 1.4 用户列表、手机号模糊搜索与冻结/解冻
// -----------------------------------------------------------------------------
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QJsonObject;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;

class UserPage : public QWidget
{
public:
    explicit UserPage(NetClient *net, QWidget *parent = nullptr);

private:
    struct UserData
    {
        qint64 userId = 0;
        QString phone;
        QString nickname;
        qint64 balanceFen = 0;
        QString createTime;
        int status = 0;
    };

    void setupUi();
    void requestUserList();
    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleUserListResponse(int code, const QString &msg,
                                const QJsonObject &data);
    void handleUserStatusResponse(int code, const QString &msg);
    void refreshTable();
    void clearSearch();
    void updateStatusButton();
    void handleUserStatusChange();
    const UserData *selectedUser() const;

    static QString statusText(int status);

    NetClient    *m_net = nullptr;
    QLineEdit    *m_phoneSearch = nullptr;
    QTableWidget *m_table = nullptr;
    QPushButton  *m_statusButton = nullptr;
    QLabel       *m_statusLabel = nullptr;
    QVector<UserData> m_users;

    int m_userListSeq = -1;
    int m_userStatusSeq = -1;
    qint64 m_pendingStatusUserId = 0;
    QString m_pendingStatusPhone;
    int m_pendingNewStatus = 0;
};

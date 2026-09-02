#pragma once
// -----------------------------------------------------------------------------
//  net_client.h  —  客户端网络层
//
//  用 QTcpSocket（Qt 事件循环友好），帧格式仍走冻结契约 common/frame.h。
//  ⚠ CLAUDE.md 硬性规则第 1 条：收到的字节必须喂给 FrameParser，
//    不能把一次 readyRead 的内容当成一个完整包。
//  ⚠ 硬性规则第 4 条：本类工作在 UI 线程，通过信号把结果交给窗口，
//    若日后改为独立线程，必须用 Qt::QueuedConnection。
// -----------------------------------------------------------------------------
#include <QObject>
#include <QTcpSocket>
#include <QJsonObject>
#include "frame.h"

class NetClient : public QObject
{
    Q_OBJECT
public:
    explicit NetClient(QObject *parent = nullptr);

    void connectToServer(const QString &host, quint16 port);
    bool isConnected() const;
    void setToken(const QString &t) { m_token = t; }
    QString token() const { return m_token; }

    // 发送请求，返回本次 seq
    int send(int cmd, const QJsonObject &data = QJsonObject());

signals:
    void connected();
    void disconnected();
    void errorText(const QString &msg);
    // 收到一条完整响应：cmd / seq / code / msg / data
    void response(int cmd, int seq, int code, const QString &msg, const QJsonObject &data);

private slots:
    void onReadyRead();

private:
    QTcpSocket      *m_sock = nullptr;
    ecp::FrameParser m_parser;      // 每条连接一个，处理粘包/半包
    QString          m_token;
    int              m_seq = 0;
};

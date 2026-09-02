#include "net_client.h"
#include "protocol.h"
#include <QJsonDocument>

NetClient::NetClient(QObject *parent) : QObject(parent), m_sock(new QTcpSocket(this))
{
    connect(m_sock, &QTcpSocket::connected,    this, &NetClient::connected);
    connect(m_sock, &QTcpSocket::disconnected, this, &NetClient::disconnected);
    connect(m_sock, &QTcpSocket::readyRead,    this, &NetClient::onReadyRead);
    connect(m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
        // [说明书] 2.3 完整的错误处理机制：把原因告诉用户，不要静默失败
        emit errorText(QStringLiteral("网络错误：%1").arg(m_sock->errorString()));
    });
}

void NetClient::connectToServer(const QString &host, quint16 port)
{
    m_parser.reset();
    m_sock->abort();
    m_sock->connectToHost(host, port);
}

bool NetClient::isConnected() const
{
    return m_sock->state() == QAbstractSocket::ConnectedState;
}

int NetClient::send(int cmd, const QJsonObject &data)
{
    if (!isConnected()) { emit errorText(QStringLiteral("尚未连接到服务器")); return -1; }
    const int seq = ++m_seq;
    m_sock->write(ecp::encodeFrame(ecp::buildRequest(cmd, seq, m_token, data)));
    return seq;
}

void NetClient::onReadyRead()
{
    m_parser.append(m_sock->readAll());     // 有多少喂多少
    QByteArray payload;
    while (m_parser.next(payload)) {        // 逐条取出完整报文
        QJsonObject env;
        if (!ecp::parseEnvelope(payload, env)) { emit errorText(QStringLiteral("报文解析失败")); continue; }
        emit response(env.value("cmd").toInt(), env.value("seq").toInt(),
                      env.value("code").toInt(), env.value("msg").toString(),
                      env.value("data").toObject());
    }
    if (m_parser.overflow()) {
        emit errorText(QStringLiteral("报文长度越界，连接已断开"));
        m_sock->abort();
    }
}

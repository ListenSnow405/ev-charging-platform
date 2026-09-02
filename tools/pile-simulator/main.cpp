// -----------------------------------------------------------------------------
//  tools/pile-simulator/main.cpp  —  电桩模拟器　归属 L1
//
//  [说明书] 1.4 远程重启（模拟向电桩发送重启指令），用于处理死机等异常
//  模拟设备侧：注册上线 → 周期上报状态与电量 → 接收服务端重启指令
//  docs/protocol.md 第 4.5 节 命令字 9001–9004
//
//  用法：./ecp-pile-sim SZ001-01 [host] [port]
// -----------------------------------------------------------------------------
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include "frame.h"
#include "protocol.h"
#include "logger.h"

using namespace ecp;

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QString pileCode = (argc > 1) ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("SZ001-01");
    const QString host     = (argc > 2) ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("127.0.0.1");
    const quint16 port     = (argc > 3) ? static_cast<quint16>(QString::fromLocal8Bit(argv[3]).toUInt()) : 9527;

    auto *sock = new QTcpSocket(&app);
    auto *parser = new FrameParser;
    int seq = 0;

    QObject::connect(sock, &QTcpSocket::connected, [&] {
        LOG_I(QStringLiteral("电桩 %1 已连接服务端，发送注册").arg(pileCode));
        sock->write(encodeFrame(buildRequest(CMD_DEV_REGISTER, ++seq, QString(),
                                             QJsonObject{{"pileCode", pileCode}})));
    });
    QObject::connect(sock, &QTcpSocket::readyRead, [&] {
        parser->append(sock->readAll());          // 处理粘包/半包
        QByteArray payload;
        while (parser->next(payload)) {
            QJsonObject env;
            if (!parseEnvelope(payload, env)) continue;
            const int cmd = env.value("cmd").toInt();
            if (cmd == CMD_DEV_REBOOT) {
                // [说明书] 1.4 收到重启指令：模拟重启动作
                LOG_I(QStringLiteral("收到远程重启指令，电桩 %1 正在重启…").arg(pileCode));
            } else {
                LOG_I(QStringLiteral("服务端响应 cmd=%1 code=%2 msg=%3")
                          .arg(cmd).arg(env.value("code").toInt()).arg(env.value("msg").toString()));
            }
        }
    });
    QObject::connect(sock, &QTcpSocket::errorOccurred, [&](QAbstractSocket::SocketError) {
        LOG_E(QStringLiteral("连接失败：%1（服务端是否已启动？）").arg(sock->errorString()));
    });

    // 周期上报：状态 + 电量　TODO(L1)：接入真实的充电量累积逻辑
    auto *timer = new QTimer(&app);
    QObject::connect(timer, &QTimer::timeout, [&] {
        if (sock->state() != QAbstractSocket::ConnectedState) return;
        sock->write(encodeFrame(buildRequest(CMD_DEV_REPORT, ++seq, QString(), QJsonObject{
            {"pileCode", pileCode}, {"status", PILE_IDLE}, {"kwh", 0}, {"power", 120.0}})));
    });
    timer->start(5000);

    LOG_I(QStringLiteral("电桩模拟器启动：%1 -> %2:%3").arg(pileCode, host).arg(port));
    sock->connectToHost(host, port);
    return app.exec();
}

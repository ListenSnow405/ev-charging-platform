// -----------------------------------------------------------------------------
//  tools/pile-simulator/main.cpp  —  电桩模拟器　归属 L1
//
//  [说明书] 1.4 远程重启（模拟向电桩发送重启指令）
//  模拟设备侧：注册上线 → 周期上报状态与电量 → 接收服务端重启/开始/结束指令
//  docs/protocol.md 第 4.5 节 命令字 9001–9006
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

    // 充电累计状态：收到 9005 开始累积（清零），9006 停止；按功率×时间累积 kWh
    bool   charging = false;
    double kwh      = 0.0;
    const double power = 120.0;   // 额定功率 kW（模拟快充）

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
            if (!parseEnvelope(payload, env, EnvelopeType::Response)) continue;
            const int cmd = env.value("cmd").toInt();
            if (cmd == CMD_DEV_REBOOT) {
                LOG_I(QStringLiteral("收到远程重启指令，电桩 %1 正在重启…").arg(pileCode));
            } else if (cmd == CMD_DEV_START) {
                charging = true; kwh = 0.0;
                LOG_I(QStringLiteral("收到开始充电指令，累计清零"));
            } else if (cmd == CMD_DEV_STOP) {
                charging = false;
                LOG_I(QStringLiteral("收到结束充电指令，停止累计"));
            } else {
                LOG_I(QStringLiteral("服务端响应 cmd=%1 code=%2 msg=%3")
                          .arg(cmd).arg(env.value("code").toInt()).arg(env.value("msg").toString()));
            }
        }
    });
    QObject::connect(sock, &QTcpSocket::errorOccurred, [&](QAbstractSocket::SocketError) {
        LOG_E(QStringLiteral("连接失败：%1（服务端是否已启动？）").arg(sock->errorString()));
    });

    // 周期上报：状态 + 累计电量
    auto *timer = new QTimer(&app);
    QObject::connect(timer, &QTimer::timeout, [&] {
        if (sock->state() != QAbstractSocket::ConnectedState) return;
        if (charging) kwh += power * 5.0 / 3600.0;   // 5s × kW / 3600s/h = kWh
        sock->write(encodeFrame(buildRequest(CMD_DEV_REPORT, ++seq, QString(), QJsonObject{
            {"pileCode", pileCode},
            {"status", charging ? PILE_IN_USE : PILE_IDLE},
            {"kwh", kwh},
            {"power", power}})));
    });
    timer->start(5000);

    LOG_I(QStringLiteral("电桩模拟器启动：%1 -> %2:%3").arg(pileCode, host).arg(port));
    sock->connectToHost(host, port);
    return app.exec();
}

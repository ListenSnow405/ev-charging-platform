// -----------------------------------------------------------------------------
//  tools/pile-simulator/main.cpp  —  电桩模拟器　归属 L1
//
//  [说明书] 1.4 远程重启（模拟向电桩发送重启指令）
//  模拟设备侧：注册上线 → 周期心跳 + 状态/电量上报 → 接收服务端重启/起停指令。
//  docs/protocol.md 第 4.5 节 命令字 9001–9006。
//
//  单进程模拟多台桩：默认 4 站 × 4 桩 = 16 台（SZ001-01 ~ SZ004-04）。
//  用法：./ecp-pile-sim                    # 默认 16 台
//        ./ecp-pile-sim SZ001-01 SZ002-02  # 只起指定桩号
// -----------------------------------------------------------------------------
#include <QCoreApplication>
#include <QTcpSocket>
#include <QTimer>
#include <QJsonObject>
#include <QList>
#include <QStringList>

#include "frame.h"
#include "protocol.h"
#include "logger.h"

using namespace ecp;

// 单台模拟桩：一个 socket 连接 + 周期心跳/上报 + 起停充电累积
class PileSim : public QObject
{
public:
    PileSim(const QString &pileCode, const QString &host, quint16 port, QObject *parent = nullptr)
        : QObject(parent), m_pileCode(pileCode),
          m_sock(new QTcpSocket(this)), m_timer(new QTimer(this))
    {
        connect(m_sock, &QTcpSocket::connected, this, [this] {
            LOG_I(QStringLiteral("电桩 %1 已连接服务端，发送注册").arg(m_pileCode));
            send(CMD_DEV_REGISTER, QJsonObject{{"pileCode", m_pileCode}});
        });
        connect(m_sock, &QTcpSocket::readyRead, this, [this] {
            m_parser.append(m_sock->readAll());          // 处理粘包/半包
            QByteArray payload;
            while (m_parser.next(payload)) {
                QJsonObject env;
                if (!parseEnvelope(payload, env, EnvelopeType::Response)) continue;
                const int cmd = env.value("cmd").toInt();
                if (cmd == CMD_DEV_REBOOT) {
                    LOG_I(QStringLiteral("%1 收到远程重启指令，正在重启…").arg(m_pileCode));
                } else if (cmd == CMD_DEV_START) {
                    m_charging = true; m_kwh = 0.0;
                    LOG_I(QStringLiteral("%1 收到开始充电，累计清零").arg(m_pileCode));
                } else if (cmd == CMD_DEV_STOP) {
                    m_charging = false;
                    LOG_I(QStringLiteral("%1 收到结束充电，停止累计").arg(m_pileCode));
                }
                // 其余响应不逐条打印，避免多台刷屏
            }
        });
        connect(m_sock, &QTcpSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) {
            LOG_E(QStringLiteral("%1 连接失败：%2（服务端是否已启动？）")
                      .arg(m_pileCode, m_sock->errorString()));
        });

        m_timer->setInterval(5000);
        connect(m_timer, &QTimer::timeout, this, [this] {
            if (m_sock->state() != QAbstractSocket::ConnectedState) return;
            // 心跳：刷新 last_heartbeat，否则服务端 15s 判离线
            send(CMD_DEV_HEARTBEAT, QJsonObject{{"pileCode", m_pileCode}});
            if (m_charging) m_kwh += m_power * 5.0 / 3600.0;   // 5s × kW / 3600s/h = kWh
            send(CMD_DEV_REPORT, QJsonObject{
                {"pileCode", m_pileCode},
                {"status", m_charging ? PILE_IN_USE : PILE_IDLE},
                {"kwh", m_kwh},
                {"power", m_power}});
        });

        LOG_I(QStringLiteral("电桩模拟器 %1 启动 -> %2:%3").arg(m_pileCode, host).arg(port));
        m_sock->connectToHost(host, port);
        m_timer->start();
    }

private:
    void send(int cmd, const QJsonObject &data)
    {
        m_sock->write(encodeFrame(buildRequest(cmd, ++m_seq, QString(), data)));
    }

    QString     m_pileCode;
    QTcpSocket *m_sock;
    QTimer     *m_timer;
    FrameParser m_parser;
    int         m_seq = 0;
    bool        m_charging = false;
    double      m_kwh = 0.0;
    const double m_power = 120.0;   // 额定功率 kW（模拟快充）
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString host = QStringLiteral("127.0.0.1");
    const quint16 port = 9527;

    // 默认 4 站 × 4 桩 = 16 台；传桩号则只起指定几台
    QStringList piles;
    if (argc > 1) {
        for (int i = 1; i < argc; ++i)
            piles << QString::fromLocal8Bit(argv[i]);
    } else {
        for (int s = 1; s <= 4; ++s)
            for (int p = 1; p <= 4; ++p)
                piles << QStringLiteral("SZ%1-%2")
                              .arg(s, 3, 10, QLatin1Char('0'))
                              .arg(p, 2, 10, QLatin1Char('0'));
    }

    QList<PileSim *> sims;
    for (const QString &code : piles)
        sims.append(new PileSim(code, host, port, &app));

    LOG_I(QStringLiteral("共启动 %1 台电桩模拟器").arg(sims.size()));
    return app.exec();
}

#pragma once
// -----------------------------------------------------------------------------
//  user-client/main_window.h  —  充电用户端主窗口（骨架）
//  归属 L4。 [说明书] 1.4 模拟手机端交互体验
// -----------------------------------------------------------------------------
#include <QWidget>
#include <QTabWidget>
#include <QLabel>
#include "net_client.h"

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(NetClient *net, QWidget *parent = nullptr);

private:
    QWidget *makePlaceholder(const QString &title, const QString &todo);
    QWidget *makeNavPage();          // [说明书] 1.4 一键导航（QWebEngineView）

    NetClient  *m_net = nullptr;
    QTabWidget *m_tabs = nullptr;
};

#pragma once
// -----------------------------------------------------------------------------
//  admin-client/main_window.h  —  PC 管理端主窗口（骨架）
//  归属 L3。 [说明书] 1.4 宽屏 PC 风格，以表格、图表为主
// -----------------------------------------------------------------------------
#include <QWidget>
#include <QListWidget>
#include <QStackedWidget>
#include <QLabel>
#include "net_client.h"

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(NetClient *net, QWidget *parent = nullptr);

private:
    QWidget *makePlaceholder(const QString &title, const QString &todo);

    NetClient      *m_net = nullptr;
    QListWidget    *m_nav = nullptr;
    QStackedWidget *m_pages = nullptr;
};

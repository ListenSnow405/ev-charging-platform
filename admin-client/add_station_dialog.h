#pragma once
// -----------------------------------------------------------------------------
//  admin-client/add_station_dialog.h  —  新增充电站输入对话框
//  归属 L3。 [说明书] 1.4 新增电站（模拟新增）
// -----------------------------------------------------------------------------
#include <QDialog>
#include <QString>

class QLineEdit;
class QSpinBox;

struct StationFormData
{
    QString name;
    QString address;
    qreal longitude = 0;
    qreal latitude = 0;
    qint64 priceFen = 0;
    int pileCount = 0;
};

class AddStationDialog : public QDialog
{
public:
    explicit AddStationDialog(QWidget *parent = nullptr);

    const StationFormData &stationData() const { return m_data; }

protected:
    void accept() override;

private:
    QLineEdit *m_name = nullptr;
    QLineEdit *m_address = nullptr;
    QLineEdit *m_longitude = nullptr;
    QLineEdit *m_latitude = nullptr;
    QLineEdit *m_priceYuan = nullptr;
    QSpinBox  *m_pileCount = nullptr;
    StationFormData m_data;
};

#include "add_station_dialog.h"
#include "time_util.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSpinBox>
#include <QVBoxLayout>

AddStationDialog::AddStationDialog(QWidget *parent) : QDialog(parent)
{
    setWindowTitle(QStringLiteral("新增电站"));
    setModal(true);
    resize(440, 330);

    m_name = new QLineEdit(this);
    m_address = new QLineEdit(this);
    m_longitude = new QLineEdit(this);
    m_latitude = new QLineEdit(this);
    m_priceYuan = new QLineEdit(this);
    m_pileCount = new QSpinBox(this);

    m_name->setPlaceholderText(QStringLiteral("例如：福田中心充电站"));
    m_address->setPlaceholderText(QStringLiteral("请输入详细地址"));
    m_longitude->setPlaceholderText(QStringLiteral("-180 至 180，例如 114.0579"));
    m_latitude->setPlaceholderText(QStringLiteral("-90 至 90，例如 22.5410"));
    m_priceYuan->setPlaceholderText(QStringLiteral("例如 1.52"));
    m_pileCount->setRange(1, 200);
    m_pileCount->setValue(4);

    auto *form = new QFormLayout;
    form->addRow(QStringLiteral("站名"), m_name);
    form->addRow(QStringLiteral("地址"), m_address);
    form->addRow(QStringLiteral("经度"), m_longitude);
    form->addRow(QStringLiteral("纬度"), m_latitude);
    form->addRow(QStringLiteral("充电价格（元/度）"), m_priceYuan);
    form->addRow(QStringLiteral("电桩数量"), m_pileCount);

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("确认新增"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
    connect(buttons, &QDialogButtonBox::accepted, this, &AddStationDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addStretch();
    layout->addWidget(buttons);
}

void AddStationDialog::accept()
{
    const QString name = m_name->text().trimmed();
    const QString address = m_address->text().trimmed();
    if (name.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("输入有误"), QStringLiteral("站名不能为空"));
        m_name->setFocus();
        return;
    }
    if (address.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("输入有误"), QStringLiteral("地址不能为空"));
        m_address->setFocus();
        return;
    }

    bool longitudeOk = false;
    const qreal longitude = m_longitude->text().trimmed().toDouble(&longitudeOk);
    if (!longitudeOk || !qIsFinite(longitude) || longitude < -180 || longitude > 180) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("经度必须是 -180 至 180 之间的数字"));
        m_longitude->setFocus();
        return;
    }

    bool latitudeOk = false;
    const qreal latitude = m_latitude->text().trimmed().toDouble(&latitudeOk);
    if (!latitudeOk || !qIsFinite(latitude) || latitude < -90 || latitude > 90) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("纬度必须是 -90 至 90 之间的数字"));
        m_latitude->setFocus();
        return;
    }

    // 协议 2102 的 price 单位为分/度；UI 输入元/度，内部只保存 qint64 分。
    static const QRegularExpression pricePattern(QStringLiteral("^\\d+(?:\\.\\d{1,2})?$"));
    const QString priceText = m_priceYuan->text().trimmed();
    if (!pricePattern.match(priceText).hasMatch()) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("充电价格格式无效，最多保留两位小数"));
        m_priceYuan->setFocus();
        return;
    }
    const qint64 priceFen = ecp::yuanToFen(priceText);
    if (priceFen <= 0) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("充电价格必须是大于 0 的有效金额"));
        m_priceYuan->setFocus();
        return;
    }
    if (m_pileCount->value() <= 0) {
        QMessageBox::warning(this, QStringLiteral("输入有误"),
                             QStringLiteral("电桩数量必须大于 0"));
        m_pileCount->setFocus();
        return;
    }

    m_data = { name, address, longitude, latitude, priceFen, m_pileCount->value() };
    QDialog::accept();
}

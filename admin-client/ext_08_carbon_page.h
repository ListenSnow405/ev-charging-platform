#pragma once
// -----------------------------------------------------------------------------
//  admin-client/ext_08_carbon_page.h  —  扩展模块 08「碳减排与能源报告」管理端页
//  归属 L5（模块 08 认领人）。接缝规则见 docs/expand/00 第 5.2 节：
//  新建独立文件，main_window.cpp 只加两行（导航项 + addWidget）。
//
//  照 overview_page 的范式：NetClient::send + response 信号 + 每请求一个 seq 与超时定时器。
//  ⚠ 硬性规则 4：不新增线程，全部工作在 UI 线程，不在网络回调里碰控件以外的东西。
//  ⚠ 硬性规则 5：Qt 6.2.4，charts 类不在 QtCharts 命名空间，不写 QT_CHARTS_USE_NAMESPACE。
//
//  口径见 docs/expand/08-实现规划.md 第 2 节。页眉常驻两条标注：
//    · 免责声明（与服务端、导出文件三处同一份字符串）
//    · 「固定时段口径」—— 模块 05 分时电价未落地时的降级说明
// -----------------------------------------------------------------------------
#include <QDate>
#include <QDialog>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QWidget>

class NetClient;
class QCheckBox;
class QComboBox;
class QDateEdit;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

#ifdef HAVE_CHARTS
class QChart;
class QDateTimeAxis;
class QLineSeries;
class QValueAxis;
#endif

// -----------------------------------------------------------------------------
//  新增排放因子对话框（3742）
//
//  生效起止用 QDateEdit 而不是文本框：裁决 D6 要求边界对齐自然日 00:00:00，
//  用日期控件后界面**根本无法表达**一个日内时刻，非法入参在这一层就没了。
// -----------------------------------------------------------------------------
struct CarbonFactorForm
{
    QString region;
    QString version;
    QString source;
    int     factorGPerKwh = 581;
    QString effectFrom;                 // 'yyyy-MM-dd 00:00:00'
    QString effectTo;                   // 空串 = 右开无穷
};

class CarbonFactorDialog : public QDialog
{
public:
    explicit CarbonFactorDialog(QWidget *parent = nullptr);
    const CarbonFactorForm &formData() const { return m_data; }

protected:
    void accept() override;

private:
    QLineEdit *m_region = nullptr;
    QLineEdit *m_version = nullptr;
    QLineEdit *m_source = nullptr;
    QSpinBox  *m_factor = nullptr;
    QDateEdit *m_effectFrom = nullptr;
    QDateEdit *m_effectTo = nullptr;
    QCheckBox *m_noEndDate = nullptr;
    CarbonFactorForm m_data;
};

// -----------------------------------------------------------------------------
//  碳排放报告页
// -----------------------------------------------------------------------------
class Ext08CarbonPage : public QWidget
{
public:
    explicit Ext08CarbonPage(NetClient *net, QWidget *parent = nullptr);

private:
    enum class LoadState { Idle, Loading, Success, Failed };

    struct DayPoint
    {
        QDate  date;
        qint64 emissionG = 0;
    };

    void setupUi();
    QWidget *createHeaderBanner();
    QWidget *createMetricCard(const QString &title, QLabel *&valueLabel);

    void requestStationList();
    void requestMetric();
    void requestFactorList();
    void requestAggregate();
    void requestReportList();
    void requestReportGen();
    void requestReportExport(const QString &format, bool allowStale);
    int  selectedReportId() const;
    void openFactorDialog();
    void submitFactor(const CarbonFactorForm &form);

    void handleResponse(int cmd, int seq, int code, const QString &msg,
                        const QJsonObject &data);
    void handleStationListResponse(int code, const QString &msg, const QJsonObject &data);
    void handleMetricResponse(int code, const QString &msg, const QJsonObject &data);
    void handleFactorListResponse(int code, const QString &msg, const QJsonObject &data);
    void handleAggregateResponse(int code, const QString &msg, const QJsonObject &data);
    void handleFactorSetResponse(int code, const QString &msg, const QJsonObject &data);
    void handleReportListResponse(int code, const QString &msg, const QJsonObject &data);
    void handleReportGenResponse(int code, const QString &msg, const QJsonObject &data);
    void handleReportExportResponse(int code, const QString &msg, const QJsonObject &data);

    void updateStatusLabel();
    void clearMetrics();
    void updateMetricCards(const QJsonObject &totals, const QJsonObject &data);
    void updateShareTable(const QJsonObject &totals);
    void updateTrend(const QVector<DayPoint> &points);

    // 扩展错误码 → 中文。服务端已能返回中文（error_code.h 的 ExtMsgProvider 挂钩），
    // 本地映射作为兜底保留：旧版服务端仍能显示人话，也便于给出贴合本页的指引。
    static QString describeError(int code, const QString &serverMsg);

    NetClient *m_net = nullptr;

    QLabel      *m_statusLabel = nullptr;
    QComboBox   *m_stationBox = nullptr;
    QDateEdit   *m_dateFrom = nullptr;
    QDateEdit   *m_dateTo = nullptr;
    QPushButton *m_queryButton = nullptr;
    QPushButton *m_aggregateButton = nullptr;
    QPushButton *m_addFactorButton = nullptr;
    QPushButton *m_genReportButton = nullptr;
    QPushButton *m_exportCsvButton = nullptr;
    QPushButton *m_exportHtmlButton = nullptr;

    QLabel *m_totalKwh = nullptr;
    QLabel *m_emission = nullptr;
    QLabel *m_intensity = nullptr;
    QLabel *m_completeness = nullptr;
    QLabel *m_provenanceLabel = nullptr;      // 因子版本 / 算法版本 / 数据截止时刻

    QTableWidget *m_shareTable = nullptr;     // 峰平谷构成
    QTableWidget *m_factorTable = nullptr;    // 排放因子版本
    QTableWidget *m_trendTable = nullptr;     // QtCharts 缺席时的降级表格
    QTableWidget *m_reportTable = nullptr;    // 已生成的报告版本，含 STALE 角标

#ifdef HAVE_CHARTS
    QChart        *m_trendChart = nullptr;
    QLineSeries   *m_trendSeries = nullptr;
    QDateTimeAxis *m_trendAxisX = nullptr;
    QValueAxis    *m_trendAxisY = nullptr;
#endif

    QTimer *m_metricTimer = nullptr;
    QTimer *m_factorTimer = nullptr;
    QTimer *m_aggregateTimer = nullptr;
    QTimer *m_stationTimer = nullptr;
    QTimer *m_factorSetTimer = nullptr;
    QTimer *m_reportListTimer = nullptr;
    QTimer *m_reportGenTimer = nullptr;
    QTimer *m_reportExportTimer = nullptr;

    LoadState m_metricState = LoadState::Idle;
    LoadState m_factorState = LoadState::Idle;
    QString   m_metricError;
    QString   m_factorError;
    QString   m_actionNote;                   // 重算 / 新增因子的一次性提示

    int m_stationSeq = -1;
    int m_metricSeq = -1;
    int m_factorSeq = -1;
    int m_aggregateSeq = -1;
    int m_factorSetSeq = -1;
    int m_reportListSeq = -1;
    int m_reportGenSeq = -1;
    int m_reportExportSeq = -1;

    // 收到 6703 后记住用户本来要导的格式，确认过期后原样重发（带 allowStale）
    QString m_pendingExportFormat;
    // 刚生成的报告号：列表刷新后要选中它，否则用户紧接着点「导出」导的是上一份
    int m_selectReportId = -1;
};

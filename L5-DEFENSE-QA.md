# L5 答辩问答参考

> 电动汽车充电桩管理平台 · 小组提问环节备稿
> 工作线 **L5 · 数据可视化与机器学习**，兼任配置管理与文档归档
> 全部行号对应 2026-09-09 的仓库状态。**被问到时按行号直接翻，不要现场找。**

---

## 0. 三十秒定位（先说这段）

> 我负责 L5 数据端，做三件事：**一是造数据**——写历史数据生成器，给全组和模型提供燃料；**二是建模型**——特征工程加时序建模，预测未来 1、6、24 小时的充电负荷和空闲桩数，结果回写数据库；**三是做展示**——ECharts 运营大屏。另外我认领了一个扩展模块「碳减排与能源报告」，那个是全栈的，服务端 8 个命令字、管理端页面、大屏和对拍脚本都是我写的。
>
> 所以我的代码横跨 Python、C++ 服务端和 Qt 客户端三层，一共约 **8,900 行**。

---

## 1. 我的代码在哪 —— 完整索引

**建议做一张纸质小抄，只印这张表。**

### 1.1 本职：数据端 `ml/` + `dataviz/`

| 文件 | 行数 | 作用 |
| --- | --- | --- |
| `ml/gen_history.py` | 630 | 历史数据生成器：站点画像、AR(1) 波动、天气马尔可夫链、增长趋势 |
| `ml/build_features.py` | 314 | 特征工程：小时面板、季节均值、滞后特征、日历特征，含穿越检查 |
| `ml/train_forecast.py` | 336 | 建模与评估：按时间切分、双基线对照、验证集选超参、生成评估报告 |
| `ml/predict.py` | 342 | 预测并回写 `t_load_forecast`，带连接级只写保护 |
| `ml/export_snapshot.py` | 212 | 只读 SQLite 导出大屏快照 JSON |
| `ml/selftest.py` | 745 | 数据端自测：口径、守恒、穿越、边界 |
| `ml/check_signal.py` | 248 | 信号诊断：判断特征是否真的带信息 |
| `dataviz/index.html` | 332 | ECharts 运营大屏，七块图表 |

### 1.2 扩展模块 08「碳减排与能源报告」（全栈，跨三个目录）

| 文件 | 行数 | 层 | 作用 |
| --- | --- | --- | --- |
| `server/biz/ext_08_carbon_calc.h` | 145 | C++ 服务端 | 计算核心的类型与函数声明 |
| `server/biz/ext_08_carbon_calc.cpp` | 283 | C++ 服务端 | **纯函数**：时段切分、最大余数分摊、排放计算、因子选择 |
| `server/biz/ext_08_carbon_service.cpp` | 1642 | C++ 服务端 | 8 个 handler、数据库读写、报告生成与导出 |
| `server/biz/ext_08_carbon_calc_test.cpp` | 330 | C++ 测试 | 手算夹具 + 5000 例模糊测试，独立编译不连库 |
| `admin-client/ext_08_carbon_page.h/.cpp` | 1493 | Qt 客户端 | 碳排放报告页 + 新增因子对话框 |
| `common/protocol_ext.h` | 37 | 契约 | 命令字 3740–3747 |
| `common/error_code_ext.h` | 50 | 契约 | 错误码 6701–6704 |
| `docs/db-schema-ext-08.sql` | 156 | 建表 | 三张碳排放表，幂等可重复执行 |
| `ml/carbon_crosscheck.py` | 243 | Python | 独立重算对拍，逐格比对服务端结果 |
| `ml/export_carbon_snapshot.py` | 164 | Python | 碳排放大屏快照导出 |
| `dataviz/carbon.html` | 261 | 大屏 | 碳排放专题屏 |
| `scripts/smoke-carbon.py` | 693 | 测试 | 8 个命令字的协议冒烟测试 |
| `scripts/test-carbon-integration.sh` | 287 | 测试 | 隔离式集成测试 |

**合计约 8,900 行。**

---

## 2. 逐题预备答案

### Q1 · 你在这个项目中承担了哪些工作？

三块，按投入从大到小：

1. **历史数据生成器**（`ml/gen_history.py`）。这是全组的基础设施——数据库里那八千多笔订单是它造的，管理端的营收趋势、大屏的所有图表、我的预测模型，用的都是这一份数据。它不是随机数：每个站点有自己的画像（办公型、商圈型、居民区型），叠加 AR(1) 时序波动、天气的马尔可夫链转移、以及月度增长趋势。

2. **负荷预测**（`build_features.py` → `train_forecast.py` → `predict.py`）。说明书要求预测未来 1、6、24 小时的负荷、空闲桩数和高峰时段。特征工程 + 梯度提升回归树，三个周期各训一个模型，结果回写 `t_load_forecast`。

3. **可视化与扩展模块**。ECharts 大屏；以及我额外认领的碳减排模块，这个是全栈的，从建表、服务端 8 个命令字、Qt 管理端页面到大屏和对拍脚本。

另外我承担团队职责里的**配置管理与文档归档**。

### Q2 · 你完成的工作的代码在哪？

照第 1 节的索引表答。**开场先给三个坐标**：

> Python 部分在 `ml/` 目录，九个脚本；大屏在 `dataviz/`；我的扩展模块因为是全栈的，代码分散在三个目录，文件名统一以 `ext_08_carbon_` 开头，`grep -r ext_08 .` 能一次列全。

### Q3 · 用到了哪些技术？

分三层答，**每层给一个具体的技术决策，不要只报名词**。

| 层 | 技术 | 可展开的点 |
| --- | --- | --- |
| Python 数据侧 | pandas、scikit-learn、sqlite3、joblib | `HistGradientBoostingRegressor`，损失函数用 `absolute_error` 而不是默认的平方损失——负荷分布右偏，平方损失会被少数峰值样本主导 |
| C++ 服务端 | Qt SQL、`QJsonObject`、pthread 线程池下的线程安全 | 全整数运算避免浮点误差；最大余数法保证守恒；短事务不持长锁 |
| Qt 客户端 | Qt Widgets、信号槽、`QTimer` 超时恢复 | 不新增线程，全部工作在 UI 线程；每个请求一个 seq 配一个超时定时器 |
| 前端大屏 | ECharts 5.5、`fetch` 轮询 | 只读静态 JSON，不连服务端 |

### Q4 · 用到了哪些类、哪些功能？

**我自己定义的类型**，都在 `server/biz/ext_08_carbon_calc.h`：

| 位置 | 类型 | 作用 |
| --- | --- | --- |
| `ext_08_carbon_calc.h:40` | `enum Quality` | 数据质量档：表计区间 / 时长估算 / 不可用 |
| `ext_08_carbon_calc.h:45` | `struct TimeSlice` | 一个时段区间 |
| `ext_08_carbon_calc.h:52` | `struct TariffPlan` | 峰谷时段表 |
| `ext_08_carbon_calc.h:64` | `struct SplitResult` | 单笔订单的峰/平/谷/未分配分摊结果 |
| `ext_08_carbon_calc.h:90` | `struct Factor` | 排放因子，含生效区间与版本 |
| `ext_08_carbon_page.h` | `class Ext08CarbonPage : public QWidget` | 碳排放报告页 |
| `ext_08_carbon_page.h` | `class CarbonFactorDialog : public QDialog` | 新增因子对话框 |
| `ext_08_carbon_page.h` | `struct CarbonFactorForm` | 对话框的表单数据 |

**我调用的框架类**：`QSqlDatabase` / `QSqlQuery`（数据库）、`QJsonObject` / `QJsonArray`（报文）、`QWidget` / `QTableWidget` / `QComboBox` / `QDateEdit` / `QTimer`（界面）、`QDateTime`（时间计算）。

**核心功能函数**：

```cpp
// server/biz/ext_08_carbon_calc.h:76
SplitResult splitOrder(const QDateTime &start, const QDateTime &end,
                       qint64 kwhX100, const TariffPlan &plan);   // 峰平谷分摊
// :81
qint64 emissionG(qint64 kwhX100, qint64 factorGPerKwh);           // 碳排放量
// :84
qint64 intensityGPerKwh(qint64 emissionG, qint64 totalKwhX100);   // 排放强度
// :112
bool pickFactor(const QVector<Factor> &factors, const QString &atTime, Factor *out);
```

### Q5 · 登录界面在哪？

**如实说明，然后转到自己负责的部分。**

> 登录界面不是我写的：管理端登录在 `admin-client/login_window.cpp`，用户端在 `user-client/login_window.cpp`。
>
> 但我的页面接在登录之后，权限校验是我写的。服务端每个 handler 第一件事就是校验管理员角色：

```cpp
// server/biz/ext_08_carbon_service.cpp:584（其余 7 个 handler 同样位置也有一行）
if (req.session.role != ROLE_ADMIN) return ERR_NO_PERMISSION;
```

> 会话 token 由网络层的分发器在调用 handler 之前就校验过了，我这一层只判角色。也就是说未登录拿不到 token、登录了但不是管理员角色也进不来，两道门。

### Q6 · 传输功能在哪？

**分两头答：客户端怎么发，服务端怎么收。**

**① 客户端发起请求** —— `admin-client/ext_08_carbon_page.cpp:583`

```cpp
const int seq = m_net->send(ecp::CMD_EXT_CARBON_METRIC, QJsonObject{
    { QStringLiteral("stationId"), m_stationBox->currentData().toInt() },
    { QStringLiteral("dateFrom"),  m_dateFrom->date().toString(QLatin1String(DATE_FMT)) },
    { QStringLiteral("dateTo"),    m_dateTo->date().toString(QLatin1String(DATE_FMT)) } });
// ...
m_metricSeq = seq;
m_metricTimer->start(READ_RESPONSE_TIMEOUT_MS);   // 每个请求配一个超时定时器
```

**② 响应通过信号槽回来** —— `ext_08_carbon_page.cpp:292` 建立连接，`:844` 分发

```cpp
// :292
connect(m_net, &NetClient::response, this, &Ext08CarbonPage::handleResponse);

// :844  按「命令字 + seq 双重匹配」认领响应，避免串包
void Ext08CarbonPage::handleResponse(int cmd, int seq, int code, const QString &msg,
                                     const QJsonObject &data)
{
    if (cmd == ecp::CMD_EXT_CARBON_METRIC && seq == m_metricSeq) {
        m_metricTimer->stop();
        m_metricSeq = -1;
        handleMetricResponse(code, msg, data);
        return;
    }
    // ...
```

**③ 服务端注册命令字** —— `server/biz/ext_08_carbon_service.cpp:1626`

```cpp
void registerExt08CarbonService()
{
    Dispatcher::instance().registerHandler(CMD_EXT_CARBON_METRIC,    handleCarbonMetric);
    Dispatcher::instance().registerHandler(CMD_EXT_FACTOR_LIST,      handleFactorList);
    Dispatcher::instance().registerHandler(CMD_EXT_FACTOR_SET,       handleFactorSet);
    Dispatcher::instance().registerHandler(CMD_EXT_CARBON_AGGREGATE, handleCarbonAggregate);
    Dispatcher::instance().registerHandler(CMD_EXT_REPORT_GEN,       handleReportGen);
    Dispatcher::instance().registerHandler(CMD_EXT_REPORT_EXPORT,    handleReportExport);
    Dispatcher::instance().registerHandler(CMD_EXT_REPORT_LIST,      handleReportList);
    Dispatcher::instance().registerHandler(CMD_EXT_FACTOR_DELETE,    handleFactorDelete);
}
```

**④ 底层帧协议不是我写的**，是 L1 的 `common/frame.h`：4 字节大端长度头 + UTF-8 JSON 体。我遵守它，没有绕过。

**⑤ 一个例外值得主动提**：报告导出的文件**不走 Socket**。

> 导出的 CSV 和 HTML 是落到服务端磁盘上的，命令字返回的是文件路径而不是文件内容。因为我们的协议是一帧一个 JSON，把整个报告塞进 JSON 会撑爆单帧。这一点在协议文档里有约定。

**⑥ 大屏完全不走这套传输** —— `dataviz/index.html:294`

```javascript
const r = await fetch(`data/snapshot.json?t=${Date.now()}`, {cache:'no-store'});
```

> 浏览器连不上我们自定义的 TCP 协议。如果为大屏单开一个 HTTP 服务，服务端就不再是纯 Socket 了，会破坏说明书的考核点。所以我让 Python 只读导出 JSON 快照，大屏轮询这个静态文件，两边彻底解耦。

### Q7 · 数据库是如何操作的？

**这题分值最高，准备两套代码：C++ 侧和 Python 侧。**

#### ① C++ 服务端：线程独立连接 + 参数绑定 + 事务

**取连接** —— `ext_08_carbon_service.cpp:591`

```cpp
QSqlDatabase db = threadDb();   // 按线程 id 生成独立连接，禁止跨线程共享
```

> 这是全组的硬性规则：服务端是 pthread 线程池，`QSqlDatabase` 跨线程共享会崩，统一加大锁又会把并发退化成串行，所以每个线程持有自己的连接。

**查询：一律 prepare + 参数绑定，绝不拼字符串** —— `ext_08_carbon_service.cpp:176`

```cpp
QSqlQuery sel(db);
sel.prepare(QStringLiteral(
    "SELECT station_id, kwh_x100, start_time, end_time"
    " FROM t_order WHERE status = ? AND date(settle_time) = ?"));
sel.addBindValue(ORDER_SETTLED);   // 只计已结算，与营收统计同源
sel.addBindValue(date);
if (!sel.exec()) {
    LOG_E(QStringLiteral("读取 %1 订单失败: %2").arg(date, sel.lastError().text()));
    return ERR_INTERNAL;
}
while (sel.next()) {
    const int    stationId = sel.value(0).toInt();
    const qint64 kwh       = sel.value(1).toLongLong();
    // ...
}
```

**写入：事务包住，任一步失败整体回滚** —— `ext_08_carbon_service.cpp:220`

```cpp
if (!db.transaction()) {
    LOG_E(QStringLiteral("开启事务失败: %1").arg(db.lastError().text()));
    return ERR_INTERNAL;
}

QSqlQuery del(db);
del.prepare(QStringLiteral(
    "DELETE FROM t_carbon_daily"
    " WHERE stat_date = ? AND factor_version = ? AND algo_version = ?"));
// ... 失败则 db.rollback()

for (auto it = byStation.constBegin(); it != byStation.constEnd(); ++it) {
    QSqlQuery ins(db);
    ins.prepare(QStringLiteral(
        "INSERT INTO t_carbon_daily"
        " (station_id, stat_date, total_kwh_x100, ... , update_time)"
        " VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    ins.addBindValue(it.key());
    // ... 17 个字段逐个绑定
    if (!ins.exec()) { db.rollback(); return ERR_INTERNAL; }
}

if (!db.commit()) { db.rollback(); return ERR_INTERNAL; }
```

**三点可以主动补充**：

- **先删后插 + `UNIQUE` 约束 = 幂等重算**。同一天重算多少次结果都一样，不会产生重复行。
- **不持长事务**。按天分批处理，一天一个短事务，不会长时间锁住整个库。
- **权限边界**：我这个模块只读 `t_order` / `t_station`，只写 `t_carbon_*` 三张表。整个文件 grep 不到对用户表、钱包表的任何写操作。

#### ② Python 侧：用 SQLite authorizer 做结构性只读保护

这是我比较得意的一处，**建议主动讲** —— `ml/predict.py:88`

```python
def only_forecast_writable(action, arg1, arg2, dbname, source):
    """SQLite authorizer：只放行对 t_load_forecast 的写，其余写操作与所有 DDL 一律拒绝。"""
    if action in (sqlite3.SQLITE_INSERT, sqlite3.SQLITE_UPDATE, sqlite3.SQLITE_DELETE):
        return sqlite3.SQLITE_OK if arg1 in WRITABLE else sqlite3.SQLITE_DENY
    if action in (sqlite3.SQLITE_CREATE_TABLE, sqlite3.SQLITE_DROP_TABLE,
                  sqlite3.SQLITE_ALTER_TABLE, sqlite3.SQLITE_CREATE_INDEX,
                  sqlite3.SQLITE_DROP_INDEX):
        return sqlite3.SQLITE_DENY
    return sqlite3.SQLITE_OK

# :319  挂到连接上
con.set_authorizer(only_forecast_writable)
```

> 组内规定数据侧脚本对业务表只读，只能写自己那张预测表。但"靠代码自觉"一次手滑就破了。挂 authorizer 之后这就变成连接级的结构性保证——即便脚本里写错了表名，SQLite 自己会拒绝这次写入。

---

## 3. 可能的追问与回答

| 追问 | 回答要点 |
| --- | --- |
| **为什么金额和电量都用整数？** | 浮点累加有误差。八千多笔订单聚合后，营收和排放总量必须能和数据库直算对上账。电量用 `kwh_x100`（度 × 100）、排放用整数克，全程 `qint64`。实测全量电量合计 3663 万，乘因子约 2.1×10¹⁰，**32 位一定溢出**，所以必须 64 位。 |
| **峰平谷怎么保证加起来等于总量？** | 最大余数法。先整除得到三段基数，余下的 0~2 个单位按余数从大到小依次补，并列时固定「峰 > 平 > 谷」的顺序保证结果可复现。代码在 `ext_08_carbon_calc.cpp:117`，5000 例模糊测试验证守恒。 |
| **跨午夜的订单怎么算？** | 日归属按结算时间整单归一天，与营收统计同源；峰平谷按真实钟点切分，可以跨两天。实测有 553 单（8.5%）跨午夜，这个口径写进了设计文档，否则会被当成 bug。 |
| **模型怎么评估的，指标可信吗？** | 三条：**严格按时间切分**训练测试集，绝不随机切（相邻小时高度相关，随机切会把未来样本混进训练集）；超参在训练段内部再切验证集来选，**不碰测试集**；用了两个基线，其中一个是分工作日/周末的同小时均值。1 小时负荷比基线好 9.2%，并发会话数好 31.1%。 |
| **为什么不给空闲桩数单独建模？** | 空闲桩数 = 总桩数 − 预测并发数，由负荷预测派生。单独训分类器会出现"预测负荷很低但被标成高峰"这种自相矛盾的结果。 |
| **你怎么保证服务端算的数是对的？** | 做了一层独立对拍：`ml/carbon_crosscheck.py` 用 Python 从订单表**完全独立地重算一遍**，与服务端写进数据库的结果逐格比对，任一格不等就退出码 2。三百多行数据零不一致。故意改坏一行能被抓出来。 |
| **排放因子改了，历史数据会变吗？** | 不会被悄悄改。因子版本一经发布不可修改；新因子以"接续"方式发布，自动闭合上一版本；受影响的旧报告会被标记为过期（STALE），数字一字不改，需要重新生成才会更新。 |
| **大屏为什么不直接连服务端？** | 浏览器连不上自定义 TCP 协议。为大屏单开 HTTP 会让服务端不再是纯 Socket，破坏考核点。所以走只读 JSON 快照。这是有意的架构决策，写进了设计文档。 |
| **你的模块会不会影响主流程？** | 不会。扩展模块由 `t_sys_config` 里的功能开关控制，关掉就不注册 handler，客户端调用得到"未知命令字"，页面显示"该功能未启用"。核心闭环没有任何回归。 |
| **你写了别人目录里的代码？** | 是，经属主口头授权。碳模块的服务端在 `server/biz/`（L2 目录）、管理端页在 `admin-client/`（L3 目录）。所有跨目录改动做成**独立提交**，便于属主事后追认或单独回退，L3 和 L2 都已追认。 |

---

## 4. 现场演示路径（如果让你打开代码）

按这个顺序翻，**三分钟能覆盖全部提问点**：

1. **`ml/predict.py`** → 拉到第 88 行，讲 authorizer 只写保护（Python 侧数据库操作 + 权限边界）
2. **`server/biz/ext_08_carbon_service.cpp`** → 第 1626 行看命令字注册（传输），往上翻到第 584 行看权限校验（登录之后的鉴权），再到第 176 行和 220 行看查询与事务（数据库操作）
3. **`server/biz/ext_08_carbon_calc.cpp`** → 第 117 行 `splitOrder`，讲最大余数法与守恒（核心算法）
4. **`admin-client/ext_08_carbon_page.cpp`** → 第 583 行发请求、第 844 行收响应（客户端传输 + 信号槽）
5. **`dataviz/index.html`** → 第 294 行轮询快照（大屏取数）

> 提前把这五个文件在编辑器里开成五个标签页，按顺序排好。

---

## 5. 一句话收尾

> 概括一下：我这条线技术栈跨得比较开——Python 做数据和模型，C++ 做服务端计算，Qt 做管理端界面，前端做大屏。贯穿始终的一条原则是**数字要能对得住**：整数运算不用浮点，分摊保证守恒，服务端算完还用 Python 独立重算一遍对拍。

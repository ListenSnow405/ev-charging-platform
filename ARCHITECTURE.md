# 系统架构与责任归属

> 电动汽车充电桩管理平台 · 课程设计
> 按 2026-09-14 仓库实测核对。**44 个命令字**（核心 35 + 扩展 9），核心 11 张数据表 + 扩展 3 张，代码约 **2.2 万行**。
>
> 第 1–7 节描述**第一阶段 Qt 业务平台**。**第二阶段大数据子系统见第 8 节**——它与前者并存、
> 技术栈不重叠，Qt 侧一行不改。任务与验收见 [PHASE2-PLAN.md](PHASE2-PLAN.md)。

## 1. 人员分工

| 工作线 | 负责人 | 负责范围 | 代码规模 |
| --- | --- | --- | --- |
| **L1** 服务端通信与并发 | （姓名） | `common/`、`server/net/`、`tools/` | 2,089 行 |
| **L2** 数据存储与业务服务 | （姓名） | `server/biz/`、`server/dao/`、`docs/db-schema.sql` | 2,042 行 |
| **L3** PC 服务器管理端 | （姓名） | `admin-client/`、`scripts/` | 4,839 行 |
| **L4** 充电用户端 | （姓名） | `user-client/`、充电流程服务 | 2,488 行 |
| **L5** 数据可视化与机器学习 | （姓名） | `ml/`、`dataviz/`、碳排放扩展模块；**二阶段 `bigdata/**`** | 8,657 行（+ 二阶段 4,472 行） |

> 代码规模为 **2026-09-09** 实测，按**责任归属**而非目录统计（充电流程服务计入 L4，碳排放全栈的三个目录一并计入 L5）。09-10 之后的修复使总量增至约 2.2 万行，分线明细未重测。

每条线除本职模块外另挂一项团队职责：L1 契约维护、L2 评审组织、L3 集成与构建、L4 测试与回归、L5 配置管理与文档归档。

## 2. 架构图

```mermaid
flowchart TB
    subgraph CLIENT["客户端层"]
        direction LR
        A["PC 管理端<br/>admin-client/<br/>L3"]
        U["充电用户端<br/>user-client/<br/>L4"]
        P["电桩模拟器<br/>tools/pile-simulator/<br/>L1"]
    end

    T["TCP 自定义帧协议 &nbsp; L1<br/>4 字节大端长度头 + UTF-8 JSON<br/>common/frame.h"]

    subgraph SERVER["ecp-server · 多线程业务服务端"]
        direction TB
        N["网络层 &nbsp; server/net/ &nbsp; L1<br/>epoll reactor · pthread 线程池<br/>会话表 · 分发器 · 推送注册表"]
        B["业务层 &nbsp; server/biz/ &nbsp; L2<br/>九个核心服务 · 充电流程 L4 · 碳排放扩展 L5"]
        D["DAO 层 &nbsp; server/dao/ &nbsp; L2<br/>threadDb() 按线程 id 生成独立连接"]
    end

    DB[("charging.db · QSQLite 单一主库 &nbsp; L2<br/>11 张表 · docs/db-schema.sql 冻结")]

    subgraph DATA["数据侧（不接入服务端）"]
        direction TB
        ML["ml/ &nbsp; L5<br/>特征工程 · 时序建模<br/>1h / 6h / 24h 负荷预测"]
        VIZ["dataviz/ &nbsp; L5<br/>index.html 运营大屏<br/>carbon.html 碳排放屏"]
    end

    A -->|2xxx| T
    U -->|1xxx| T
    P -->|9xxx| T
    T --> N
    N -->|投递完整报文| B
    B --> D
    D --> DB
    DB -.->|只读取数| ML
    ML -->|写回 t_load_forecast| DB
    ML -->|导出 snapshot.json| VIZ

    classDef l1 fill:#e8f1fb,stroke:#2a78d6,stroke-width:2px,color:#131722
    classDef l2 fill:#fdeee7,stroke:#eb6834,stroke-width:2px,color:#131722
    classDef l3 fill:#e6f7f1,stroke:#1baf7a,stroke-width:2px,color:#131722
    classDef l4 fill:#fdf3dd,stroke:#eda100,stroke-width:2px,color:#131722
    classDef l5 fill:#fceef3,stroke:#e87ba4,stroke-width:2px,color:#131722

    class T,N,P l1
    class B,D,DB l2
    class A l3
    class U l4
    class ML,VIZ l5
```

> 该图在 GitHub 上可直接渲染。若渲染环境不支持 Mermaid，可参照下一节的分层说明理解结构。

## 3. 分层说明

一次请求的完整路径：**客户端 → TCP 帧 → 网络层切帧 → 线程池跑业务 → DAO → SQLite**。

### 客户端层

| 模块 | 归属 | 内容 |
| --- | --- | --- |
| `admin-client/` | L3 | PC 管理端，六个功能页：数据总览、电站管理、电桩管理、用户管理、订单管理、负荷预测 |
| `user-client/` | L4 | 充电用户端，五个页签：附近电桩、导航、充电、我的、登录 |
| `tools/pile-simulator/` | L1 | 电桩模拟器，模拟设备注册、状态与电量上报、接收下发指令 |

### 传输层

自定义 TCP 帧协议，**4 字节大端长度头 + UTF-8 JSON 体**。TCP 是字节流没有消息边界，收发一律走 `common/frame.h` 的编解码器，粘包与半包在网络层处理干净，业务代码只看到完整的一帧。

### 服务端 `ecp-server`

| 层 | 目录 | 归属 | 内容 |
| --- | --- | --- | --- |
| 网络层 | `server/net/` | L1 | 主线程 Qt 事件循环负责 accept；独立 epoll IO 线程统一收发与切帧；pthread 线程池执行业务；另有会话表、命令字分发器、设备与用户推送注册表 |
| 业务层 | `server/biz/` | L2 | Dispatcher 注册全部业务 handler，覆盖用户、钱包、电站、电桩、预约、订单、计费、统计等 |
| DAO 层 | `server/dao/` | L2 | `threadDb()` 按线程 id 生成独立 SQLite 连接，禁止跨线程共享 |

### 数据层

`charging.db`，QSQLite 单一主库，11 张表，图片只存文件路径。表结构 `docs/db-schema.sql` 为冻结契约，非属主只读。

### 数据侧

`ml/` 只读数据库，完成特征工程与时序建模，预测未来 1h / 6h / 24h 的充电负荷、空闲桩数与高峰时段，结果写回 `t_load_forecast`，并导出 JSON 快照供 `dataviz/` 大屏轮询。

## 4. 目录归属明细

| 目录 | 属主 | 内容 | 规模 |
| --- | --- | --- | --- |
| `common/` | L1 | 冻结契约：命令字表、帧编解码、错误码、日志、时间工具 | 8 文件 · 539 行 |
| `server/net/` | L1 | epoll reactor、线程池、会话表、分发器、推送注册表、设备服务 | 19 文件 · 1,468 行 |
| `tools/` | L1 | 电桩模拟器 | 1 文件 · 82 行 |
| `server/biz/` | L2 | 业务服务集合 | 14 文件 · 4,686 行 |
| `server/dao/` | L2 | 线程安全的数据库连接分配 | 2 文件 · 77 行 |
| `docs/db-schema.sql` | L2 | 冻结的数据库表结构 | 476 行 |
| `admin-client/` | L3 | PC 管理端六个功能页 | 23 文件 · 5,251 行 |
| `scripts/` | L3 | 一键构建、协议冒烟测试、隔离式集成测试 | 7 文件 · 2,018 行 |
| `user-client/` | L4 | 充电用户端 | 7 文件 · 2,167 行 |
| `ml/` | L5 | 历史数据生成、特征工程、时序建模、预测回写、独立对拍 | 9 文件 · 3,234 行 |
| `dataviz/` | L5 | 运营大屏与碳排放大屏 | 2 文件 · 593 行 |
| `bigdata/` | L5 | **第二阶段**：ODS 导出、Spark 清洗与分析、MLlib 建模、Flask 只读 API、Vue3+DataV 大屏 | 26 文件 · 3,568 行 |
| `scripts/*phase2*`　`*hadoop*`　`hdfs-ctl.sh` | L5 | **第二阶段**：环境安装与自检、Hadoop 起停、ODS 推 HDFS、接口与大屏冒烟 | 8 文件 · 904 行 |

## 5. 跨目录协作

目录归属与实际作者并非完全重合。以下三处为经属主授权的跨目录实现，均做成**独立提交**，便于属主追认或单独回退。

| 文件 | 所在目录（属主） | 实际作者 | 规模 | 说明 |
| --- | --- | --- | --- | --- |
| `server/biz/order_flow_service.cpp` | `server/biz/`（L2） | L4 | 321 行 | 充电起停与结算服务，由用户端作者实现，与前端同步联调 |
| `server/biz/ext_08_carbon_*.cpp` | `server/biz/`（L2） | L5 | 2,400 行 | 碳排放扩展模块的计算核心与 handler，含手算夹具 |
| `admin-client/ext_08_carbon_page.*` | `admin-client/`（L3） | L5 | 1,493 行 | 碳排放报告页与因子对话框，L3 侧仅新增导航登记 |

> 看架构图时的口径：**按颜色认作者，按目录认属主。**

## 6. 命令字分段

核心 35 个命令字分段由 L1 维护在 `common/protocol.h`；扩展 08 的 9 个在 `common/protocol_ext.h`，段内自治。

| 段 | 功能 | 实现方 |
| --- | --- | --- |
| `1001–1006` | 用户登录、资料修改、钱包充值与流水 | L2 |
| `1101–1102` | 附近电站、站内电桩查询 | L2 |
| `1201–1202` · `1206–1207` | 未完成订单校验、预约、取消、订单列表 | L2 |
| `1203–1205` | 开始充电、结束充电、结算 | L4 |
| `1208` | 充电状态主动推送 | L1 |
| `2001` | 管理员登录 | L2 |
| `2101–2103` | 电站列表、新增、详情 | L2 |
| `2111–2112` | 电桩列表、远程重启（下发 9003） | L2 |
| `2201–2202` | 用户模糊搜索、冻结解冻 | L2 |
| `2301–2305` | 营收三指标、趋势、电桩状态分布、订单列表、负荷预测 | L2 |
| `3740–3748` | 碳排放指标、排放因子、报告生成与导出、因子撤销与彻底删除（扩展 08） | L5 |
| `9001–9006` | 电桩注册、状态上报、心跳、下发重启与起停充电 | L1 |

核心 11 张表见 `docs/db-schema.sql`；扩展 08 另建 `t_carbon_factor` / `t_carbon_daily` / `t_carbon_report`（`docs/db-schema-ext-08.sql`，幂等，随构建自动执行）。

错误码集中在 `common/error_code.h`，扩展模块错误码另置于 `common/error_code_ext.h`，通过函数指针挂钩注入，冻结文件不反向依赖扩展。

## 7. 关键设计决策

| 决策 | 理由 |
| --- | --- |
| **Qt 服务端始终不提供 HTTP** —— 一阶段大屏只读导出 JSON 快照、浏览器轮询静态文件；二阶段大屏改由独立的 Flask 层供数 | 浏览器连不上自定义 TCP 协议。若为大屏给 Qt 服务端加 HTTP，它就不再是纯 Socket，会破坏说明书 1.6 的考核点。一阶段用静态快照解耦；二阶段的 Flask **不挂在 Qt 服务端上**，而是大数据子系统自己的一层，只读 MySQL 分析结果，Qt 侧仍然一行不改 |
| **网络层用 POSIX 原生 socket + epoll**，不用 QTcpServer | 说明书正文明确要求 Socket 编程；epoll reactor 使单线程即可管理全部连接读写 |
| **并发用 pthread 线程池**，不用 QThread | 说明书 1.6 考核点 |
| **每线程独立数据库连接** | `QSqlDatabase` 跨线程共享会崩；统一加锁又会把并发退化为串行 |
| **金额一律 `qint64` 整数「分」** | 浮点累加会产生误差，八千余笔订单的营收统计必须能对上账；仅显示层除以 100 |
| **协议、错误码、表结构冻结** | 跨模块契约由单一属主维护，非属主只读，变更走流程，避免五条线并行时接口反复漂移 |
| **SQL 一律 `prepare` + 参数绑定** | 防注入，同时避免手工拼接字符串带来的类型错误 |

## 8. 第二阶段 · 大数据子系统

**与第一阶段并存，不是替代。** Qt 业务平台一行不改（仍是纯 Socket），
本子系统只共用同一份数据源 `charging.db`，用另一套技术栈把同类分析重做一遍。
两阶段技术栈基本不重叠，不要把一边的结论套到另一边。

```mermaid
flowchart TB
    DB[("charging.db · QSQLite<br/>一阶段业务库（只读取数）")]

    subgraph ODS["ODS 原始层 · 只读"]
        O["bigdata/ods/ 或 hdfs:///ecp/ods<br/>14 表 10524 行 CSV<br/>444 只读 · _manifest.json 记血缘"]
    end

    subgraph COMPUTE["Spark 计算 &nbsp; bigdata/spark · mllib &nbsp; L5"]
        direction TB
        C["清洗 DWD<br/>SOP 六阶段 · R001–R011"]
        AN["多维分析<br/>13 维度 + 3 组对比"]
        MLB["Spark MLlib<br/>6 个 GBT · 1h/6h/24h"]
    end

    MY[("MySQL · 分析结果层<br/>只存结果，不存明细")]
    API["Flask 只读 API &nbsp; bigdata/api &nbsp; L5<br/>4 个接口 · 只监听 127.0.0.1"]
    WEB["Vue3 + DataV 大屏 &nbsp; bigdata/web &nbsp; L5<br/>5 页 18 面板 · 9 类图表"]

    DB -->|export_ods.py&nbsp;mode=ro| O
    O --> C
    C --> AN
    C --> MLB
    AN -->|回写| MY
    MLB -->|回写| MY
    MY -->|SQL| API
    API -->|HTTP/JSON| WEB

    classDef l5 fill:#fceef3,stroke:#e87ba4,stroke-width:2px,color:#131722
    classDef ro fill:#eef1f5,stroke:#6b7785,stroke-width:2px,color:#131722
    class C,AN,MLB,API,WEB l5
    class O,DB ro
```

### 8.1 分层与职责边界

| 层 | 目录 | 职责 | 硬边界 |
| --- | --- | --- | --- |
| ODS 原始层 | `bigdata/ods/` 或 HDFS | `charging.db` 的只读快照 | **任何清洗都不在此层做**，原始快照一个字节不改 |
| DWD 加工层 | `bigdata/dwd/` | 清洗、类型转换、派生字段 | 删除/修正/填充**必须留痕**，无法判定的进待核清单 |
| 分析与建模 | `bigdata/spark/`　`mllib/` | 多维分析、MLlib 训练与预测 | **不得直连 `charging.db`**，一律走 ODS |
| 结果层 | MySQL | 只存分析结果 | 不存明细，否则 Flask 查询压力不可控 |
| Web 层 | `bigdata/api/` | 只读接口 | 不写任何业务库，不现场触发 Spark job |

五条边界对应 [CLAUDE.md 第 5.2 节](CLAUDE.md)，逐条都有对应的实现手段，不是口头约定。

### 8.2 第二阶段的关键设计决策

| 决策 | 理由 |
| --- | --- |
| **ODS 的介质可换，代码只认路径** —— 本地目录与 HDFS 共用同一条代码路径，靠 `ECP_ODS_ROOT` 切换 | 本地开发期不必背着 Hadoop 调试，答辩时换成 `hdfs://` 前缀即可。**但这不会自动成立**：`pathlib` 会折叠 `//`、Hadoop 的 `FileInputFormat` 会过滤 `_` 开头的文件，都得专门处理（见 PHASE2-PLAN 第 13.5 节） |
| **ODS 导出与推送 HDFS 分成两个脚本** | `export_ods.py` 是纯 Python 文件 I/O，对 HDFS 不可用；让它直写 HDFS 就要引入 Python 的 HDFS 客户端，去做 `hdfs dfs -put` 已经能做的事。现在它产出**本地权威快照**，`ods-to-hdfs.sh` 负责搬运与逐文件 MD5 校验 |
| **HDFS 上的只读靠独立身份，不只靠 444** | HDFS 的超级用户（启动 NameNode 的账号）**绕过一切权限检查**，照搬本地的 `chmod 444` 会让「ODS 只读」在 HDFS 上变成一句自觉。ODS 属主设为 `ecp_ods`，分析侧固定以 `ecp_analyst` 连，上传脚本末尾以分析身份**实际试写一次并期待失败** |
| **只部署 HDFS，不起 YARN** | Spark 跑 `local[*]`，用不上资源调度；多两个守护进程只是多两处会崩的地方 |
| **统一的 `SparkSession` 入口** | worker 与 driver 的 Python 解释器、`JAVA_HOME`、HDFS 访问身份都在这里钉死。这些都是「shell 里 export 也能修，但总有人会忘」的东西 |
| **金额仍是整数「分」，电量仍是 `kwh_x100`** | 与第一阶段同源。Spark 聚合一律在整数上做，只在展示层除以 100；八千余笔订单的营收必须能逐分对上账 |
| **机器学习改用 Spark MLlib**，不沿用一阶段的 scikit-learn | 第二阶段任务书明文要求。方法论沿用一阶段：按时间切分、双基线对照、验证段选超参 |

### 8.3 一条贯穿全程的硬校验

营收 **53,936,279 分**与电量 **36,638,035**（×100 度）这两个数，在清洗校验、
7 个含营收的分析维度、MySQL 落库后、MLlib 特征面板守恒自检**逐环节对账一致**，
电量还与第一阶段碳排放对拍脚本独立算出的数字相同。

口径在环节间漂移是这类项目最常见的暗伤——第一阶段就踩过（大屏与管理端把同一个指标显示成两个形状，
见 [conventions.md](docs/conventions.md) 3.1 的 2026-09-04 记录），所以每一环都显式做了对账。

---

*本文档由仓库实测数据生成：`git shortlog`、目录行数统计、`registerHandler` 调用清单、SQLite 表结构抽查。人员一栏请填入真实姓名后使用。*
*运行方式见 [docs/RUNBOOK.md](docs/RUNBOOK.md)。*

# 运行手册 `docs/RUNBOOK.md`

> **全项目唯一的「怎么跑起来」出处。** 各模块文档不再重复启动命令，一律链到本文。
> 契约见 [protocol.md](protocol.md) / [db-schema.sql](db-schema.sql)，架构见 [../ARCHITECTURE.md](../ARCHITECTURE.md)，
> 规范与变更记录见 [conventions.md](conventions.md)，答辩大屏动线见 [../dataviz/DEMO.md](../dataviz/DEMO.md)。
> **第 1–6 节是第一阶段 Qt 业务平台，第 7 节是第二阶段大数据子系统**，两套技术栈互不依赖。

## 1. 首次准备

```bash
bash scripts/check-env.sh          # 环境自检；也可只查一条线：check-env.sh L3
bash scripts/build-all.sh          # 构建 + 建库 + ext 建表 + 生成 config/app.ini
```

`check-env.sh` 对 QtCharts / QtWebEngineWidgets **实际试编译**（只看包名会误判：运行库与 `-dev` 开发包是两个 deb），
未通过时直接打印该装哪些包，退出码等于未通过项数。

`build-all.sh` 的行为：`qmake6` + `make` → 产物落 `build/bin/`；`charging.db` 不存在则按 `docs/db-schema.sql` 建库；
**每次构建都按文件名顺序执行全部 `docs/db-schema-ext-*.sql`**（脚本幂等，重复跑安全）；`config/app.ini` 不存在则从 `.example` 复制。
`bash scripts/build-all.sh clean` 清理 `build/`。

Python 侧（只有 L5 的建模脚本需要）：

```bash
python3 -m venv .venv && .venv/bin/pip install -r ml/requirements.txt
```

`gen_history.py` / `export_snapshot.py` / `check_signal.py` 只用标准库，不需要 venv；
`build_features.py` / `train_forecast.py` / `predict.py` / `selftest.py` 需要。

## 2. 启动

```bash
./build/bin/ecp-server                        # 服务端，先启动（控制台程序）
./build/bin/ecp-admin                         # PC 管理端（GUI）
./build/bin/ecp-user                          # 充电用户端（GUI）
./build/bin/ecp-pile-sim SZ001-01             # 电桩模拟器，默认连 127.0.0.1:9527
./build/bin/ecp-pile-sim --port 58171 SZ002-03   # 指向其他端口的服务端实例
```

运行参数在 `config/app.ini`：`server/host`、`server/port`、`server/pool_size`（工作线程数 = 最大并发业务处理数）、
`map/key`、`dataviz/*`。业务参数（token 有效期、充值上限等）在 `t_sys_config` 表，**改表不改代码**。

管理员默认账号 `admin / 123456`；用户端手机号免密登录，不存在即自动注册。

## 3. 大屏

```bash
python3 ml/export_snapshot.py                 # 默认读 charging.db → dataviz/data/snapshot.json
python3 -m http.server 8080 -d dataviz        # 打开 http://127.0.0.1:8080
```

碳排放屏 `carbon.html` 的快照另由 `python3 ml/export_carbon_snapshot.py` 导出。

⚠ **必须经 http 访问**：直接双击 `file://` 打开会被同源策略拦下，`fetch` 全挂。
页面按快照里的 `pollIntervalSec`（默认 30 秒）轮询，但快照本身不会自己更新——演示时让导出器循环跑：

```bash
while true; do python3 ml/export_snapshot.py >/dev/null; sleep 30; done &
```

## 4. 预测流水线

日常只需刷新预测：

```bash
.venv/bin/python ml/predict.py charging.db --commit --prune   # 推理并回写 t_load_forecast
python3          ml/export_snapshot.py                        # 重新导出快照
```

`predict.py` 挂了 SQLite authorizer，对 `charging.db` **只可能写 `t_load_forecast`**，其余表的写入与所有 DDL 在连接层就被拒绝。

改了生成器或特征之后才需要重建模型，**顺序不能乱**（5 依赖 4 训出的模型与 `meta.json` 里的 is_peak 阈值，6 依赖 5 的回写）：

```bash
python3          ml/gen_history.py ml/data/dev.db --commit --reset   # 1 重播历史（仅副本）
python3          ml/check_signal.py                                  # 2 信号体检，四项须全过
.venv/bin/python ml/build_features.py charging.db                    # 3 特征面板（只读真库）
.venv/bin/python ml/train_forecast.py                                # 4 训练 + 评估报告
.venv/bin/python ml/predict.py charging.db --commit --prune          # 5 推理回写
python3          ml/export_snapshot.py                               # 6 导出快照
```

⚠ **不要对 `charging.db` 跑 `gen_history.py`**：历史数据已于 2026-09-05 正式落库，`--reset` 对该库是硬性拒绝的
（CR-002 批复第 2 条），重复执行只会撞 `order_no` 唯一约束。要重播先在副本上做。
需要回滚时用落库前的备份 `charging-bak-20260905-082255.db` 直接 `cp` 覆盖。

## 5. 测试

| 命令 | 覆盖 | 是否需要服务端 |
| --- | --- | --- |
| `python3 scripts/smoke-admin.py` | 管理端命令字协议 smoke | 是 |
| `bash scripts/test-admin-integration.sh` | 隔离式端到端集成（自建临时库与配置，不碰真实 DB） | 自带 |
| `bash scripts/test-carbon-calc.sh` | 扩展 08 计算核心手算夹具，单独编译，不连库不起服务 | 否 |
| `python3 scripts/smoke-carbon.py` | 扩展 08 七个命令字协议 smoke（只读，3745 会重写 `t_carbon_daily`） | 是 |
| `bash scripts/test-carbon-integration.sh` | 扩展 08 隔离式集成 | 自带 |
| `.venv/bin/python ml/selftest.py` | 数据端契约与不变量，41 项断言，全程临时目录 | 否 |
| `.venv/bin/python ml/selftest.py --full` | 同上 + 隔离环境完整训练（约 4 分钟） | 否 |
| `.venv/bin/python ml/carbon_crosscheck.py` | 独立重算碳排放与 `t_carbon_daily` 逐格对拍，不一致退出码 2 | 否 |

机器验不了的（布局、交互、跨模块联调、演示动线）见 [../ml/TESTING.md](../ml/TESTING.md)。

全组测试用例集见 [../第一阶段测试用例.xlsx](../第一阶段测试用例.xlsx)：9 张模块表 106 条用例 + 缺陷清单，协议层 94 条已实测，GUI 12 条标注待人工执行。**执行充电类用例前务必先拉起电桩模拟器**，否则 1203 恒返回 3005，会被误判为缺陷。

## 6. 常见故障

**GUI 程序报 `could not connect to display`。** 不是 Qt 装坏了——最后那句「Reinstalling the application may fix this」极具误导性。
原因是 SSH 会话没有 `DISPLAY`。服务端与电桩模拟器是控制台程序，不受影响。三种做法：

1. **在虚拟机桌面的终端里跑 GUI 程序**（推荐），SSH 留给服务端、模拟器、大屏这些控制台程序；
2. 留在 SSH 里借用桌面显示（窗口出现在虚拟机桌面上，不在 SSH 终端里）：
   ```bash
   export DISPLAY=:0
   export XAUTHORITY=$(ls /run/user/1000/.mutter-Xwaylandauth.* 2>/dev/null | head -1)
   ```
   认证文件名里的随机串每次重启桌面会变，**必须用 `ls` 通配，不要写死**；
3. `ssh -X` X11 转发，窗口显示在宿主机，Qt 程序走转发较卡，调界面不推荐。

**客户端报「不支持的请求类型」（`ERR_CMD_UNKNOWN` 1005）。** 该命令字的 handler 没注册，不是链路故障。
扩展模块的命令字还要看 `t_sys_config` 里对应的 `feat_*` 开关——关掉时 handler 不注册，页面显示「该功能未启用」。

**大屏图全空。** 先确认是经 http 而不是 `file://` 打开的，再确认 `dataviz/data/snapshot.json` 是否已导出。

**「今日营收」为 0。** `gen_history.py` 刻意不写当天订单——红线自检正是靠「今日无订单」分辨库里有没有真实联调数据。
服务端跑起来、现场产生订单后这个数会自己填上。

## 7. 第二阶段 · 大数据子系统

> 与上面六节**完全独立**：技术栈不重叠，跑第二阶段不需要启动 `ecp-server`，
> 反之亦然。两者只共用同一份 `charging.db`。子系统导览见 [../bigdata/README.md](../bigdata/README.md)，
> 任务与验收见 [../PHASE2-PLAN.md](../PHASE2-PLAN.md)。

### 7.1 首次准备

```bash
bash scripts/install-phase2-env.sh      # 需 root：JDK 17 / Python 3.11 / MySQL
bash scripts/init-mysql-phase2.sh       # 需 root：建库建账号 → config/phase2.ini
python3.11 -m venv .venv-phase2 && .venv-phase2/bin/pip install -r bigdata/requirements.txt
bash scripts/install-hadoop-phase2.sh   # Hadoop 3.3.6 伪分布式，免 root，装到 ~/opt
bash scripts/check-env-phase2.sh        # 自检，应全绿
```

⚠ **`.venv-phase2` 与一阶段的 `.venv` 是两个环境，不要混用。** 前者 Python 3.11 装 PySpark，
后者 Python 3.10 装 scikit-learn。`check-env-phase2.sh` 会**实际建一次 `SparkSession`、
实际连一次 NameNode**，不是只看 `pip list`。

⚠ **境内装 PySpark 务必加镜像**，官方源下 318MB 的 sdist 实测会断流：

```bash
.venv-phase2/bin/pip install -r bigdata/requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple
```

Hadoop 安装包走阿里云（实测 13MB/s；清华的 apache 线只有 0.3MB/s，与 pypi 那条线的快慢相反）。

### 7.2 HDFS 起停

**HDFS 不会开机自启**，每次重启机器后都要拉一次：

```bash
bash scripts/hdfs-ctl.sh start          # 起 NameNode + DataNode，等退出安全模式
bash scripts/hdfs-ctl.sh status         # 看两个守护进程在不在
bash scripts/hdfs-ctl.sh report         # dfsadmin -report
bash scripts/hdfs-ctl.sh stop
```

NameNode Web UI：http://localhost:9870　数据落在 `~/opt/hadoop-data`，**不在 `/tmp`**，重启不丢。

### 7.3 数据源：本地目录 ↔ HDFS

分析代码**只认路径不认介质**，切换靠一个环境变量：

```bash
set -a; . config/phase2-hdfs.env; set +a     # 切到 HDFS
unset ECP_ODS_ROOT ECP_HDFS_USER             # 切回本地目录
```

每个 job 启动时打印本次数据源，事后能分辨某份结果是哪一边跑的：

```text
[ODS] HDFS　hdfs://localhost:9000/ecp/ods　身份 ecp_analyst
[ODS] 本地　/home/bit/projects/ev-charging-platform/bigdata/ods
```

### 7.4 全链路

顺序不能乱，前一步的产出是后一步的输入：

```bash
.venv-phase2/bin/python bigdata/spark/export_ods.py     # ODS 本地权威快照（源库 mode=ro）
bash scripts/ods-to-hdfs.sh                             # 推 HDFS + 逐文件 MD5 + 落只读 + 试写验证
set -a; . config/phase2-hdfs.env; set +a                # 下游切到 HDFS

.venv-phase2/bin/python bigdata/spark/profiling.py      # 清洗阶段 1+2
.venv-phase2/bin/python bigdata/spark/cleaning.py       # 清洗阶段 4
.venv-phase2/bin/python bigdata/spark/validation.py     # 清洗阶段 5，20/20 必须全过
.venv-phase2/bin/python bigdata/spark/quality_report.py # 清洗阶段 6
.venv-phase2/bin/python bigdata/spark/analysis.py       # 13 维度 + 3 组对比 → MySQL
```

建模（可与分析并行，训练约 80 分钟）：

```bash
.venv-phase2/bin/python bigdata/mllib/features.py       # 特征面板，电量守恒自检须 0.0000
.venv-phase2/bin/python bigdata/mllib/train.py          # 6 个 GBTRegressor
.venv-phase2/bin/python bigdata/mllib/report.py         # 评估报告（由 eval.json 生成，不手写）
.venv-phase2/bin/python bigdata/mllib/predict.py        # 预测 → MySQL
```

⚠ `export_ods.py` **只写本地目录**。`ECP_ODS_ROOT` 指向 HDFS 时它会直接报错退出（退出码 2），
推 HDFS 是 `ods-to-hdfs.sh` 的事——理由见 PHASE2-PLAN 第 13.5 节。

### 7.5 API 与大屏

```bash
.venv-phase2/bin/python bigdata/api/app.py &            # Flask 只读 API :5000
cd bigdata/web && npm install && npm run dev            # 大屏 :5173
```

Flask **只监听 `127.0.0.1`**：本服务无鉴权，不应暴露到局域网。

冒烟：

| 命令 | 覆盖 | 是否需要服务 |
| --- | --- | --- |
| `.venv-phase2/bin/python bigdata/spark/smoke_spark.py` | 最小 SparkSession，装完环境先跑它 | 否 |
| `python3 scripts/smoke-api-phase2.py` | 4 个接口 14 项，含一条 SQL 注入尝试 | API |
| `python3 scripts/smoke-screen-phase2.py` | 大屏逐页渲染断言，出真实截图 | API + 大屏 |

> 两个 `smoke-*-phase2.py` **只用标准库**，用系统 `python3` 即可；
> 只有需要 PySpark 的脚本才必须走 `.venv-phase2/bin/python`。

第二阶段测试用例集见 [../第二阶段测试用例.xlsx](../第二阶段测试用例.xlsx)：9 张模块表 **130 条用例** + 缺陷清单 8 条，
覆盖环境基线、ODS 与血缘、HDFS 部署与只读、清洗六阶段、多维分析、Flask API、DataV 大屏、MLlib 建模。
表格由 `scripts/gen-testcases-phase2.py` 生成，**不手工维护**——改用例改脚本再重跑：

```bash
.venv-phase2/bin/python scripts/gen-testcases-phase2.py
```

⚠ **跑接口与大屏用例前要先起服务**（API :5000 + 生产预览 :4173），否则冒烟会整段失败；
大屏冒烟默认打的是 `4173` 的**生产构建**，不是 `5173` 的 dev server。

### 7.6 第二阶段常见故障

**`PYTHON_VERSION_MISMATCH`（worker 3.10 / driver 3.11）。** 用系统 `python3` 跑了 job。
一律用 `.venv-phase2/bin/python`；从 `spark_session.build_spark()` 取 session 时会自动钉死两端解释器。

**Spark 报 `Path does not exist: hdfs:/localhost:9000/...`（注意只有一个斜杠）。**
路径被 `pathlib` 折叠了。ODS 路径必须按字符串处理，用 `spark_session.ods_file()` 拼，**不要包成 `Path`**。

**Spark 读不到 `_manifest.json`，但 `hdfs dfs -cat` 读得到。**
Hadoop 的 `FileInputFormat` 默认过滤 `_` 和 `.` 开头的文件。用 `spark_session.read_ods_text()`，
它走 `FileSystem.open` 绕开这层过滤。

**图表是空的，但接口返回 200。** MySQL 的 `SUM()` 经 pymysql 返回 `Decimal`，
被序列化成字符串 `"78.11"`，前端图表库拿到字符串会静默画不出来。序列化层已统一转 `float`，
冒烟里有一条专门防回归的断言——若复现，先看接口返回里该字段有没有引号。

**训练像是「卡住了」。** 不要用 `pgrep -f "mllib/train.py"` 判断它在不在——
**那个模式会匹配到发起检查的进程自己**，结果恒为真。看日志或用 PID 文件。

**`java.lang.StackOverflowError`（GBT 训练）。** `checkpointInterval` 不设检查点目录会静默不生效，
血缘线性增长到一定轮数必崩。`train.py` 已 `setCheckpointDir`，改训练参数时别把它去掉。

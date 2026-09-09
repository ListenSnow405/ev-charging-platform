# 运行手册 `docs/RUNBOOK.md`

> **全项目唯一的「怎么跑起来」出处。** 各模块文档不再重复启动命令，一律链到本文。
> 契约见 [protocol.md](protocol.md) / [db-schema.sql](db-schema.sql)，架构见 [../ARCHITECTURE.md](../ARCHITECTURE.md)，
> 规范与变更记录见 [conventions.md](conventions.md)，答辩大屏动线见 [../dataviz/DEMO.md](../dataviz/DEMO.md)。

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
./build/bin/ecp-pile-sim SZ002-03 127.0.0.1 9527
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

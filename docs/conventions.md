# 规范与变更记录

> 属主 **SCML（L5）**。编码规范见 [CLAUDE.md](../CLAUDE.md) 第 7 节，本文件不重复；只承载**冻结跟踪、变更记录、环境与配置基线、成果物归档**。
> `[说明书]` 1.7：配置负责人「按照已定义的规范对成员的开发流程及成果物进行跟踪，并对过程成果物进行配置」——本文件即该职责载体。

## 1. 冻结状态

| 契约 | 属主 | 状态 | 冻结日期 | 版本 |
| --- | --- | --- | --- | --- |
| `docs/protocol.md` | L1 | ✅ 已冻结 | 2026-09-02 | v1.0 |
| `docs/db-schema.sql` | L2 | ✅ 已冻结 | 2026-09-02 | v1.0 |
| `common/**` | L1 | ✅ 已冻结 | 2026-09-02 | v1.1 |
| `CLAUDE.md` / `AGENTS.md` | 全组 | ✅ 已冻结 | 2026-09-02 | v1.0 |

**冻结** = 成果物定稿后进入受控状态，不再由个人随手改。判定三条同时成立：① 已合入 `main`　② 全组评审通过并知晓　③ 此后仅属主可改。
**冻结不等于永不修改**——协议会补命令字，schema 会加字段。它管的是「怎么改」，见下节。

## 2. 变更流程

① 群里提出（改什么 / 为什么 / 影响哪几条线）→ ② 属主与受影响担当评审，PRL 把关 → ③ **由属主一人修改** → ④ 群里通知，所有人**重新同步各自 agent 的上下文** → ⑤ SCML 在第 3.1 节记一条。**提出但尚未评审的申请，先挂在第 3.2 节。**

> 第 ④ 步是 agent 协作特有的，也最容易漏：**人知道了不等于 agent 知道了**，旧会话缓存的是过期协议。Codex 用户建议重开会话。

## 3. 变更记录

### 3.1 已生效变更

| 日期 | 契约 | 变更 | 评审 | 已通知 |
| --- | --- | --- | --- | --- |
| 2026-09-02 | 全部四项 | 初版编写完成，提交评审 | — | — |
| 2026-09-02 | 全部四项 | **全组评审通过，正式冻结 v1.0** | 全组 | ✅ |
| 2026-09-02 | `CLAUDE.md` | Qt 版本写死为 **6.2.4**（全组虚拟机一致） | 全组 | ✅ |
| 2026-09-02 | 工程骨架 | 建立四个 C++ 子工程与顶层 qmake 工程，编译验证通过 | — | ✅ |
| 2026-09-02 | `common/logger.h` | 修正编译错误：`pthread_t → quintptr` 的 `reinterpret_cast` 在 64 位 Linux 非法，改 `static_cast<quint64>` | PRL | ✅ |
| 2026-09-02 | `common/app_path.h` | 新增 `resPath()`：从 Qt Creator 运行时工作目录是构建目录，原先按相对路径找 `config/app.ini` 与 `charging.db` 会失败。改为可执行文件上溯两级定位项目根 | L1 | ✅ |
| 2026-09-02 | 全部文档 | 消除跨文件重复，按「每份内容只留一处」重排，总量减少约六成 | 全组 | ✅ |
| 2026-09-02 | `config/app.ini` | 线程池大小改为可配置项 `pool_size`，原先在 `main.cpp` 写死；每条连接占一个线程直到断开，该值即最大并发连接数 | L1 | ✅ |
| 2026-09-02 | `.gitignore` | `config/` 规则收紧为默认全挡、仅放行 `*.example`；原 `config/*.ini` 挡不住其他后缀的凭据文件 | SCML | ✅ |
| 2026-09-02 | `server/biz/` | 实现 1001 手机号免密登录（含自动注册、冻结拦截）、1002 用户信息、2001 管理员登录，作为 L2 的样板实现 | L2 | ✅ |
| 2026-09-03 | `server/biz/` | L2 服务批量合入（PR #3）：1003–1006 / 1101–1102 / 1201–1202 / 1206–1207 / 2101–2103 / 2111 / 2201–2202 / 2304，共 20 个命令字注册 | L2 | ✅ |
| 2026-09-03 | `admin-client/` | L3 六个界面 mock 完成，充电站管理与用户管理已切真实接口 | L3 | ✅ |
| 2026-09-03 | `docs/protocol.md`(→v1.1) / `server/net/session.*` / `server/biz/user_management_service.cpp` | **越权代改**：L1、L2 暂时无法操作，经用户明确指示由 L5 代为落地 CR-001+CR-003，非正常流程，详见第 3.2 节对应 CR 的越权记录 | 用户指示 | ⬜ 待 L1/L2 复核 |
| 2026-09-02 | `docs/conventions.md` | 第 3 节拆为 3.1 已生效 / **3.2 待评审变更申请**；原先只有「改完之后」的记录，没有「提出到批准之间」的落点，CR 只能停在群聊里翻不到 | SCML | ⬜ |
| 2026-09-02 | `.gitignore` | 补 `*.db-shm` / `*.db-wal`；原规则只挡 `*.db` 和 `*.db-journal`，SQLite 走 WAL 模式时这两个边车文件会漏进仓库 | SCML | ⬜ |
| 2026-09-02 | `ml/CLAUDE.md` | 数据库权限改为「运行期只读 / 离线播种可写」两条并列规则，并挂 CR-002 未批前禁止 `--commit` | SCML | ⬜ |

### 3.2 待评审变更申请（CR）

> 一条 CR 走完第 2 节五步后，从本表移入第 3.1 节。**属主未改之前，任何人不得先按新协议写代码。**

| 编号 | 契约 | 摘要 | 提出人 | 属主 | 状态 |
| --- | --- | --- | --- | --- | --- |
| CR-001 | `docs/protocol.md` | 增加负荷预测结果通道（1101 扩字段 + 新增 2305） | L5 | **L1** | ✅ **已应用**（L1 暂时无法操作，2026-09-03 由 L5 代为改动，待归队复核） |
| CR-002 | `ml/` 规范 + `t_order` 等业务表 | 授权 `gen_history.py` 离线写入业务表播种历史数据 | L5 | **L2** | 🟢 脚本已实现（dry-run 可跑），落库待批 |
| CR-003 | `docs/protocol.md` 第 6 节 + `server/net/session.h` | 冻结用户后会话立即失效（`invalidateSessions`） | L2 | **L1** | ✅ **已应用**（L1/L2 均暂时无法操作，2026-09-03 由 L5 代为改动并编译通过，待二人归队复核） |

---

#### CR-001 · 协议增加负荷预测结果通道

**提出人** L5(SCML)　**属主** L1　**日期** 2026-09-02　**影响** L1 定稿 · L2 取数实现 · L3 负荷预警界面 · L4 推荐排序 · L5 数据回写

**为什么要改**

`[说明书]` 1.4 机器学习子系统有两句明文要求：

> 「系统根据预测结果，在**用户端优先推荐低拥堵、高空闲率的充电站**，减少用户排队等待时间；同时为**运营端提供负荷预警**，辅助工作人员提前做好电力调配与运维值守安排。」

而 `docs/protocol.md` v1.0 第 4 节全表**没有任何字段或命令字承载预测结果**：1101 响应的 list 元素只有 `stationId / name / address / price / pileTotal / pileIdle / distance` 七个字段；管理端 2300 段只有 2301–2304 四个历史统计命令字。

**库里有、协议里没有** —— `docs/db-schema.sql` 第 10 张表 `t_load_forecast` 已冻结，`load_kw / idle_pile / is_peak / congestion` 字段齐备。预测算出来也送不到两端，说明书明文功能无法验收。

**建议改动**（三处，最终由 L1 定夺）

*改动 1 — 4.2 节 1101 响应 list 元素增加两个字段*（向后兼容，客户端可忽略）

| 字段 | 类型 | 来源 | 说明 |
| --- | --- | --- | --- |
| `congestion` | double | `t_load_forecast.congestion` | 0..1 拥堵度，取 `horizon=1` 的最新一条；**无预测数据填 `-1`** |
| `idleForecast` | int | `t_load_forecast.idle_pile` | 未来 1h 预测空闲桩数；**无预测数据填 `-1`** |

> **为什么缺省值是 -1 而不是 0**：在拥堵度语义里 `0` 表示「完全不拥堵」，是最优站点，会被排到推荐列表最前面。「没有预测数据」被当成「最好的站」是方向性错误，必须能与真实的 0 区分开。

*改动 2 — 4.2 节 1101 请求增加可选字段 `sortBy`*

| 值 | 含义 |
| --- | --- |
| `0`（缺省即 0） | 按距离升序 —— `[说明书]` 1.4「按距离由近及远展示充电站列表」 |
| `1` | 按拥堵度升序，同拥堵度再按距离 —— `[说明书]` 1.4「优先推荐低拥堵、高空闲率的充电站」 |

> 说明书这两句都是明文且并存，**不能二选一改默认值**，只能加开关。默认保持距离排序，L4 现有实现不受影响。

*改动 3 — 4.4 节管理端 2300 段新增命令字 2305*

| 命令字 | 名称 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- |
| 2305 | 站点负荷预测 / 负荷预警 `[说明书]` 1.4 | `{stationId, horizon}` | `{list:[{stationId, stationName, horizon, predictTime, loadKw, idlePile, isPeak, congestion, modelVersion}]}` |

- `stationId = 0` 表示全部站点；`horizon` 取 1 / 6 / 24，非法值返回 `ERR_PARAM`
- `loadKw` 为 double，单位 kW。**这是物理量不是金额**，不受「金额一律整数分」规则约束
- 数据来源：`t_load_forecast` 中最新一批（同一 `model_version` 下 `create_time` 最大的那批）
- **预警阈值不进协议**：`congestion ≥ 0.8` 判为预警，由管理端本地判定。阈值将来要调不必动协议

**错误码：不需要新增。** 无预测数据时返回 `code = 0` + 空 `list`，管理端显示「暂无预测数据」。预测缺失是正常状态而非错误 —— 这条明确写出来，避免 L2 自行造一个 `ERR_NO_FORECAST`，那样会同时污染 `docs/protocol.md` 第 5 节和 `common/error_code.h` 两处冻结契约。

**明确不做**（防止范围膨胀）

- 不为用户端单独增加预测查询命令字，扩 1101 字段即可，少一个命令字少一处联调
- 不做预测结果服务端推送，大屏走 JSON 快照、两端按需拉取
- **不涉及 `docs/db-schema.sql`**，`t_load_forecast` 表结构不动

**L5 侧承诺**（协议批准后）

保证 `t_load_forecast` 中每个 `station_id × horizon` 至少有一条当前有效预测；`model_version` + `create_time` 可供 L2 稳定取到「最新一批」。

> ⚠ **越权记录**：本规则第 4 节「非属主只读……不要自己动手改了再说」在此处被打破。L1 2026-09-03 暂时无法操作，经用户明确指示由 L5 代为直接改动 `docs/protocol.md` 并已应用（v1.1），**非正常变更流程**（跳过了「属主亲自落笔」一步）。以下是实际已落地的内容，L1 归队后请逐条复核，如有异议可随时改回或调整——这不是最终定论，是应急处理。

已应用的补丁：

*补丁 1 — 替换 4.2 节整段*

```markdown
### 4.2 用户端 · 充电站与电桩（1100–1199）

| 命令字 | 名称 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- |
| 1101 | 附近充电站列表 `[说明书]` 1.4 | `{lng, lat, keyword, sortBy}` | `{list:[{stationId, name, address, price, pileTotal, pileIdle, distance, congestion, idleForecast}]}` |
| 1102 | 充电站内电桩详情 `[说明书]` 1.4 | `{stationId}` | `{list:[{pileId, code, type, status, power}]}` |

`price` 单位为**分/度**；`distance` 单位为**米**（显示层换算为公里）。

`sortBy`（`[本组自定]` CR-001，可选，缺省 `0`）：
- `0` 按距离升序 `[说明书]` 1.4「按距离由近及远展示充电站列表」
- `1` 按拥堵度升序，同拥堵度再按距离 `[说明书]` 1.4「优先推荐低拥堵、高空闲率的充电站」

`congestion`（double，0..1，`[本组自定]` CR-001）/ `idleForecast`（int，`[本组自定]` CR-001）：取 `t_load_forecast` 中 `horizon=1` 的最新一条。**无预测数据时两字段均填 `-1`**——`0` 在拥堵度语义中代表「最不拥堵」，不能兼职表示「没有数据」，否则新站或模型未跑到的站会被排到推荐最前面。
```

*补丁 2 — 4.4 节 2304 行之后新增一行 + 表后加一段说明*

```markdown
| 2305 | 站点负荷预测 / 负荷预警 `[说明书]` 1.4（CR-001） | `{stationId, horizon}` | `{list:[{stationId, stationName, horizon, predictTime, loadKw, idlePile, isPeak, congestion, modelVersion}]}` |
```
```markdown
`2305`：`stationId=0` 表示全部站点；`horizon` 取 `1`/`6`/`24`，非法值返回 `ERR_PARAM`。取 `t_load_forecast` 中同一 `model_version` 下 `create_time` 最大的一批。**无预测数据返回 `code=0` + 空 `list`**，不新增错误码——预测缺失是正常状态，不是错误，新增错误码要同时改协议第 5 节和 `common/error_code.h` 两处冻结契约，不值得。管理端按 `congestion ≥ 0.8` 本地判定预警，阈值不进协议，将来要调不必再走变更流程。`loadKw` 单位 kW，是物理量不是金额，不受「金额整数分」规则约束。
```

*补丁 3 — 第 8 节变更记录加一行*

```markdown
| 2026-09-03 | v1.1 | CR-001：1101 增加 congestion/idleForecast/sortBy；新增 2305 站点负荷预测 | L5 | 待评审 |
```

---

#### CR-002 · 授权 `gen_history.py` 离线写入业务表

**提出人** L5　**属主** L2（`t_order` / `t_pile_log` / `t_pile` 属主）；规范条文由 SCML 落地　**日期** 2026-09-02
**影响** L2 授权 · L5 实现 · L3/L4 演示数据 · 全组答辩

**为什么要改**

`ml/CLAUDE.md` 现规定 ml/ **只读业务表**，但历史数据生成器必须向 `t_order` 写入模拟历史。实测当前 `charging.db`：

```
t_station 6　t_pile 24　t_user 4
t_order 0　　t_pile_log 0　　t_wallet_tx 0　　t_load_forecast 0
```

后果不止 L5 一条线：大屏「营收趋势」「站点排行」两张图全 0；时序模型没有任何训练数据；管理端 `[说明书]` 1.4 要求的「近 7 / 30 日营收趋势」演示时也是一条零线。

**冲突点在哪**：「只读业务表」这条规则本身是对的 —— 防的是运行期 Python 脚本与服务端争抢、污染业务数据。**不能简单删掉**。

**建议方案**：把「运行期只读」和「离线播种可写」拆成两条**并列**规则，而不是给只读规则开一个口子。

| 项 | 约定 |
| --- | --- |
| 定位 | **离线种子脚本**，与服务端进程无关，不随服务端启动，不进任何自动流程 |
| 可写表 | 仅 `t_order`（INSERT）、`t_pile_log`（INSERT）、`t_pile` 的 `charge_count` / `charge_duration`（UPDATE，按生成订单聚合回填） |
| 禁止 | 不碰 `t_user` / `t_station` / `t_admin` / `t_wallet_tx` / `t_sys_config`；不改任何表结构 |
| 默认行为 | **默认 dry-run**，只打印将生成多少条；必须显式 `--commit` 才落库 |
| 重跑 | 提供 `--reset` 先清空 `t_order` / `t_pile_log` 再生成，仅限开发库 |
| 红线自检 | 执行前检查：若 `t_order` 中存在 `settle_time` 晚于本次生成区间上界的记录，判定为「库里已有真实联调数据」，**拒绝执行**并提示 |
| 运行期 | ml/ 其余脚本（`export_snapshot.py`、预测脚本）维持 `mode=ro` 只读；预测结果只写 `t_load_forecast` |

> **为什么 `t_pile` 的两个累计字段也要写**：不回填的话，库里有几千条历史订单，而管理端 2111 电桩列表里每个桩「累计充电次数 = 0」「累计充电时长 = 0」，`[说明书]` 1.4 明文要求的这两列一眼就是假数据，答辩必被追问。因此列入可写范围，但**严格限于这两个聚合字段**。

**请 L2 确认三点**

1. 授权范围是否就是上表这三张表，有无遗漏或需要收紧的
2. `--reset` 清空 `t_order` 是否可接受 —— 会连带清掉 L2 自测产生的订单，需约定「谁在什么时候可以跑」
3. 是否需要在 `t_sys_config` 增加一行 `data_seed_version` 记录播种批次。该表属 L2，**L5 不自行插入**，需要的话请 L2 加

---

#### CR-003 · 冻结用户后会话立即失效

**提出人** L2　**属主** L1（`docs/protocol.md` 第 6 节 + `server/net/session.h`）　**日期** 2026-09-03
**影响** L1 实现接口 · L2 调用 · **L4 客户端要处理 `ERR_TOKEN_INVALID`** · L3 无感

**问题**

2202 冻结/解冻已实现（`server/biz/user_management_service.cpp`），冻结只 `UPDATE t_user.status`。1001 登录会拦截冻结账号（`user_service.cpp:82`），但**冻结前已签发的 token 不受影响**，会话表在 `server/net/session.h`，L2 无权改，也不应该改。

**⚠ 风险比提出时描述的更严重。** L2 原话是「最长可能继续有效 2 小时」，这个说法不成立：

`SessionTable::validate()` 每次校验成功都会 `it->lastActive = now`（`session.cpp`），`docs/protocol.md` 第 6 节也明文「**每次成功请求刷新最后活跃时间**」。所以 7200 秒是**空闲超时**，不是签发后的绝对有效期。

> **结论：一个被冻结但仍在操作客户端的用户，token 永不过期，风控措施等于没生效。** 上限不是 2 小时，是无限。

实测当前已注册的 20 个命令字中，冻结用户凭旧 token 仍可调用的包括 **1005 钱包充值**（`wallet_service.cpp` 全程未查 `t_user.status`）、1003 改昵称、1004 改头像。1202 预约有复查（`reservation_service.cpp:78`），是唯一挡住的。`[说明书]` 1.4 明文「管理员可手动冻结用户账号（用于风控场景）」——现状不满足。

**建议实现**（L1 定夺）

L2 建议的 `void invalidateUserSessions(int userId)` 方向正确，建议签名调整为：

```cpp
// 踢掉指定身份的全部会话，返回被清除的会话数（供日志与审计）。
// 必须按 role 过滤：user_id 与 admin_id 是两套独立自增序列，同一个 id 值
// 在两种角色下都存在，不过滤会误踢管理员。
int invalidateSessions(int id, Role role);
```

两点理由：

1. **返回值不要 void** —— L2 提交事务后要 `LOG_I` 记一句「踢掉 N 个会话」，否则联调时无法判断到底生效没有
2. **带 `Role` 参数而非写死 ROLE_USER** —— `docs/db-schema.sql` 的 `t_admin.status` 已有 `1=停用`，管理员停用迟早要同样的能力，一次做好省得再改一遍接口

实现上是一次写锁下的全表扫描（`m_map` 量级为在线连接数，`pool_size = 8`），成本可忽略。

**协议第 6 节需同步加一条**（这才是本条必须走变更流程的原因）：

> - 管理员冻结用户（2202）后，该用户**全部在线会话立即失效**，后续请求返回 `ERR_TOKEN_INVALID`

**L4 必须知道的连带影响**

会话被踢后，客户端下一个请求收到的是 `ERR_TOKEN_INVALID` →「登录已过期，请重新登录」，**不是**「该账号已被冻结」。用户重新用手机号登录时 1001 才返回 `ERR_USER_FROZEN` 显示真实原因。

所以 **L4 收到 `ERR_TOKEN_INVALID` 必须清空本地 token 并跳转登录页**，不能静默重试——否则会拿着已失效的 token 死循环。这条不写清楚，联调周必踩。

**错误码：不新增。** 复用 `ERR_TOKEN_INVALID`，理由同 CR-001：新增错误码要同步改 `docs/protocol.md` 第 5 节和 `common/error_code.h` 两处冻结契约，为一个能靠两步流程表达清楚的语义不值得。

**附带建议（属 L2，不入本 CR）**：1005 充值补一次 `t_user.status` 复查。本 CR 落地后冻结用户已拿不到有效 token，该检查属纵深防御，但资金接口值得多一道，且能覆盖「冻结事务提交」与「踢会话」之间的瞬时窗口。

> ⚠ **越权记录**：同上，本条同样打破「非属主只读」规则。L1、L2 2026-09-03 均暂时无法操作，经用户明确指示由 L5 代为直接改动 `server/net/session.h` / `session.cpp`（L1 的文件）与 `server/biz/user_management_service.cpp`（L2 的文件）并已应用，增量编译通过（0 error 0 warning），并已用真实 `charging.db` + 实际 TCP 协议跑通完整链路（13800138002 登录取 token → 2202 冻结 → 旧 token 查 1002 返回 `ERR_TOKEN_INVALID`(1003) → 重新登录返回 `ERR_USER_FROZEN`(2003) → 解冻恢复数据库原状），7 项断言全部通过。两位属主归队后仍请复核代码本身，测试通过不代表设计取舍没有异议空间。

已应用的补丁：

*补丁 1（L1）— `server/net/session.h`，`remove()` 声明之后新增*

```cpp
    // 按角色踢除指定身份的全部会话；返回被清除的数量。
    // 必须按 role 过滤——user_id 与 admin_id 是两套独立自增序列，
    // 同一个 id 值在两种角色下都存在，不过滤会误踢管理员会话。
    int invalidateSessions(int id, Role role);
```

*补丁 2（L1）— `server/net/session.cpp`，`remove()` 实现之后新增*

```cpp
int SessionTable::invalidateSessions(int id, Role role)
{
    int n = 0;
    pthread_rwlock_wrlock(&m_lock);
    for (auto it = m_map.begin(); it != m_map.end(); ) {
        if (it->id == id && it->role == role) { it = m_map.erase(it); ++n; }
        else ++it;
    }
    pthread_rwlock_unlock(&m_lock);
    return n;
}
```

*补丁 3（L1）— `docs/protocol.md` 第 6 节末尾追加一条*

```markdown
- 管理员冻结用户（2202）后，该用户**全部在线会话立即失效**，后续请求返回 `ERR_TOKEN_INVALID`（客户端应清空本地 token 并跳转登录页，而非静默重试）
```

*补丁 4（L2）— `server/biz/user_management_service.cpp`，`handleUserStatus` 内 `db.commit()` 成功之后、`return ERR_OK` 之前插入*（`net/session.h` 的 include 方式与文件里已有的 `net/dispatcher.h` 一致，不必新加 include 路径写法）：

```cpp
    if (status == USER_FROZEN) {
        const int kicked = SessionTable::instance().invalidateSessions(static_cast<int>(userId), ROLE_USER);
        LOG_I(QStringLiteral("冻结用户已踢下线: userId=%1 会话数=%2").arg(userId).arg(kicked));
    }
```

需要在文件顶部 `#include "net/dispatcher.h"` 之后加一行 `#include "net/session.h"`。**L1 的补丁 1/2 落地之后 L2 补丁 4 才能编译通过**，两人需要协调落地顺序（L1 先合，L2 再合，或 L2 先在本地叠加 L1 的分支自测）。

---

## 4. 评审记录

| 日期 | 对象 | 结论 |
| --- | --- | --- |
| 2026-09-02 | 四项冻结契约 v1.0 | **通过**。会上定案：Qt 统一 6.2.4；运行参数维持原设定（线程池 8 / token 7200s / 端口 9527 / 充值上限 100000 分，均在 `t_sys_config` 表，改表不改代码）；地图 Key 每人各申请，见第 6 节 |

## 5. 环境基线

`[说明书]` 1.5：Ubuntu 22.04+，Qt Creator 6.2+。

| 项 | 版本 |
| --- | --- |
| 操作系统 | Ubuntu 22.04 LTS（VMware 17）`[说明书]` |
| Qt | **6.2.4**，全组一致 `[本组自定]` |
| 编译器 | g++，C++17 |
| 数据库 | QSQLite（Qt 自带驱动，无需额外装）`[说明书]` |
| Python | 3.10（L5） |

### 自检

**全员 W1 第一天跑一次，把「结论」一行贴群里。**

```bash
bash scripts/check-env.sh        # 全部
bash scripts/check-env.sh L3     # 只查自己这条线
```

脚本检查基础工具、Qt 版本、模块头文件、QSQLite 驱动，并对 QtCharts / QtWebEngineWidgets **实际试编译**；未通过时直接打印该装哪些包。退出码等于未通过项数。

> **为什么试编译而不只看包名**：这两个模块各有「运行库」与「开发包」两个 deb。`libqt6charts6` 是运行库，`libqt6charts6-dev` 才带头文件。若因其他软件依赖装上了运行库，`dpkg -l | grep charts` 能看到东西，容易误判已装好，但编译仍报找不到头文件。**试编译是唯一可靠判定。**

L3 需要 QtCharts（`[说明书]` 1.4 营收趋势用 QChart），L4 需要 QtWebEngineWidgets（`[说明书]` 1.4 一键导航用 QWebEngineView），两者都不在 `qt6-base-dev` 里。这是 agent 完全帮不上的环境问题，拖到联调周会连累全组。

### SSH 登录时 GUI 程序起不来

从宿主机 SSH 进虚拟机跑 `ecp-admin` / `ecp-user` 会报：

```
qt.qpa.xcb: could not connect to display
This application failed to start because no Qt platform plugin could be initialized.
Reinstalling the application may fix this problem.
```

**不是 Qt 装坏了**，最后那句提示极具误导性。原因是 SSH 会话没有 `DISPLAY`，Qt 不知道往哪儿画窗口。服务端与电桩模拟器是控制台程序，不受影响。

三种做法：

1. **在虚拟机桌面的终端里跑 GUI 程序**（推荐）。SSH 留给服务端、模拟器、大屏这些控制台程序，分工正好。
2. 想留在 SSH 里跑，借用桌面的显示（窗口出现在虚拟机桌面上，不在 SSH 终端里）：

   ```bash
   export DISPLAY=:0
   export XAUTHORITY=$(ls /run/user/1000/.mutter-Xwaylandauth.* 2>/dev/null | head -1)
   ```

   认证文件名里的随机串每次重启桌面会变，**必须用 `ls` 通配，不要写死**。可加进 `~/.bashrc`。
3. `ssh -X` X11 转发，窗口显示在宿主机。Qt 程序走转发较卡，调界面不推荐。

## 6. 配置与密钥

`[说明书]` 2.2：合理考虑数据安全问题。

**凭据不进仓库。** `config/` 默认整个被 `.gitignore` 挡下，只放行 `*.example` 模板——这样以后往里放任何凭据文件（`map_key.txt`、`secrets.json` 等）都不会误提交。仓库只留模板：

```bash
cp config/app.ini.example config/app.ini   # 首次克隆后执行，填入自己的值
```

模板改了要提交，实际配置永不提交。已验证 `git add config/` 只会加入 `.example`。

`app.ini` 可配置项：`server/host`、`server/port`、`server/pool_size`（= 最大并发连接数）、`map/key`、`dataviz/*`。

### 腾讯地图 Key

`[说明书]` 1.4 两处功能依赖：地址转经纬度、路线规划。Key 是腾讯位置服务发给开发者账号的调用凭据，绑定微信/QQ，**按 Key 计算免费额度**（地理编码约 1 万次/日）。

**每人各申请一个**，三个理由：

1. **额度共享**——五人共用一个，联调时容易同一天跑满；如果那天正好答辩，导航当场演示不了
2. **进了 git 撤不干净**——后来删掉仍留在提交历史里，仓库交老师、推 GitHub、答辩投屏 `git log` 都会暴露，别人能消耗你的额度，腾讯检测到异常还可能封禁
3. **是「数据安全」的具体落点**——答辩被问时，除密码 SHA-256、SQL 参数绑定外的第三条

**申请**：<https://lbs.qq.com/> 注册实名 → 应用管理创建应用 → 添加 Key，勾选 **WebServiceAPI** 与 **JavaScript API** → IP 白名单本机调试可留空，**答辩机出口 IP 要提前加** → 填入 `config/app.ini` 的 `[map] key=`。

代码从 `QSettings` 读，禁止硬编码进 `.cpp` / `.h` / `.pro` / 提交信息，也不要编造假 Key 占位。

## 7. 分支与提交

- `main` 为集成分支；各线在 `feat/L1-xxx` 分支开发，自测通过后合入
- 合入前确认只改了自己归属目录下的文件
- 小步提交，禁止一次上千行。信息写人话：

  ```
  ✅ feat(L2): 订单结算扣减钱包余额，余额不足返回 ERR_BALANCE_NOT_ENOUGH
  ❌ 完成管理端 / update
  ```
- 禁止提交 `config/app.ini`、`*.db`、构建产物、`.pro.user`（均已在 `.gitignore`）
- 推送由本人确认后手动执行，不要让 agent 跑 `git push`

## 8. 过程成果物归档

| 阶段 | 成果物 | 责任人 | 状态 |
| --- | --- | --- | --- |
| 需求 | 需求理解与工作线拆解 | 全组 | ✅ |
| 设计 | 通信协议 | L1 | ✅ [protocol.md](protocol.md) |
| 设计 | 数据库设计 | L2 | ✅ [db-schema.sql](db-schema.sql) |
| 设计 | 技术选型说明（含推论项理由） | 全组 | ✅ CLAUDE.md 第 2 节 |
| 开发 | 各模块源码与模块级 CLAUDE.md | 各线 | ⬜ 进行中 |
| 测试 | 测试用例集与回归清单 | L4 | ⬜ 待建 |
| 测试 | 交叉试读评审记录 | L2 | ⬜ 每周一次，记入第 4 节 |
| 发布 | 部署与运行说明 | L3 | ⬜ 随一键启动脚本提交 |
| 发布 | 答辩演示动线 | 全组 | ⬜ W4 |

## 9. 待办

- [x] 四项契约评审冻结　- [x] Qt 版本写死 6.2.4　- [x] 运行参数定案　- [x] 地图 Key 方案
- [x] 模块级 `CLAUDE.md` / `AGENTS.md`（六个模块）　- [x] 工程骨架与一键构建
- [ ] **全员 W1 第一天跑 `scripts/check-env.sh`，结论贴群**
- [ ] **L3 装 `libqt6charts6-dev`，L4 装 `qt6-webengine-dev qt6-webengine-dev-tools`**
- [ ] L4 申请腾讯地图 Key 并同步申请步骤
- [ ] W1 结束各线在骨架上交出本线「能跑的空壳」，L3 汇总验证
- [ ] **⚠ L1、L2 归队后必读**：`docs/protocol.md`(v1.1)、`server/net/session.h/.cpp`、`server/biz/user_management_service.cpp` 在二人不在时被 L5 代为直接改动（CR-001 + CR-003，第 3.2 节有完整越权记录），已编译通过但**不代表内容一定对**，必须逐条复核
- [ ] CR-001 待 L1 复核——协议缺预测通道，已代为应用 v1.1，1101/2305 的**实际取数逻辑**（`server/biz/station_service.cpp` 读 `t_load_forecast`）还没人写，不在本次代改范围内
- [ ] CR-002 待 L2 定夺——`gen_history.py` 写业务表授权，脚本已实现测试通过，**落库动作本身未越权执行**，仍在等 L2 拍板
- [ ] CR-003 待 L1/L2 复核——冻结后会话立即失效，已代为应用；实测冻结后旧 token 立即返回 `ERR_TOKEN_INVALID`，测试后已解冻恢复数据库原状（`t_admin_oplog` 留了一对 FREEZE/UNFREEZE 记录，这是功能本身该留的审计痕迹，未清理）

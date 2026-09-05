# 通信协议 `docs/protocol.md`

> **冻结契约** · 属主 **L1**，其他人只读。变更走 [CLAUDE.md](../CLAUDE.md) 第 3 节流程。
> 标记约定见 CLAUDE.md 第 0 节：`[说明书]` 不得擅自更改，`[本组自定]` 可评审后调整。

## 1. 依据与自定范围

- `[说明书]` 1.6：**网络通信由 Socket 编程实现其功能**
- `[说明书]` 1.6：程序的主框架应该是一个**多线程**结构（本项目为 pthread 线程池）
- `[说明书]` 2.2：通信数据结构设计，能够**安全稳定**地实现数据传输；合理考虑数据安全问题
- `[说明书]` 2.3：需设计完整的**错误处理**机制

说明书**只规定了「用 Socket」和「要设计数据结构」，没有规定具体报文格式**。以下第 2 节起的帧格式、命令字编号、错误码编号、会话机制全部为 `[本组自定]`。

## 2. 帧格式 `[本组自定]`

```
 0        4                          4+N
 +--------+---------------------------+
 | length |   payload (UTF-8 JSON)    |
 | uint32 |         N bytes           |
 +--------+---------------------------+
```

- `length`：**网络字节序（大端）uint32**，值为 payload 字节数，**不含自身 4 字节**
- `payload`：UTF-8 编码的 JSON 文本，不以 `\0` 结尾
- 单帧 payload 上限 **1 MB**，超出直接断开连接并记日志（防御性上限，头像等大数据不走此通道）

> **⚠ TCP 是字节流，没有消息边界。** 必须循环读满 4 字节长度头、再循环读满 N 字节体，才算收到一帧。禁止假设一次 `read()` / `readyRead()` 就对应一个完整包——这是本项目 [五条硬性规则](../CLAUDE.md#5-五条硬性规则agent-最常踩的坑) 第 1 条。统一使用 `common/frame.h` 的 `FrameCodec`，不要自己写解析。

## 3. 报文信封 `[本组自定]`

### 请求

```json
{
  "cmd":   1001,
  "seq":   12,
  "token": "登录后下发，未登录接口填空串",
  "data":  { }
}
```

### 响应

```json
{
  "cmd":  1001,
  "seq":  12,
  "code": 0,
  "msg":  "ok",
  "data": { }
}
```

| 字段 | 类型 | 说明 |
| --- | --- | --- |
| `cmd` | int | 命令字，见第 4 节。响应原样回填请求的 `cmd` |
| `seq` | int | 客户端自增序号，服务端原样回填，用于匹配请求与响应 |
| `token` | string | 登录后由服务端下发；除登录/注册类接口外均需携带 |
| `code` | int | 错误码，`0` 表示成功，见第 5 节 |
| `msg` | string | 错误描述，成功时为 `"ok"` |
| `data` | object | 业务数据；无数据时为 `{}`，不要用 `null` |

**服务端推送**（无对应请求，如电桩状态变化）：`seq` 固定为 `0`，`code` 固定为 `0`。

## 4. 命令字 `[本组自定]`

编号分段便于扩展：新增功能先在本表占号，再写代码。

### 4.1 用户端 · 用户与鉴权（1000–1099）

| 命令字 | 常量名 | 说明 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- | --- |
| 1001 | `CMD_USER_LOGIN` | 手机号免密登录 / 首次自动注册 `[说明书]` 1.4 | `{phone}` | `{token, userId, phone, nickname, avatar, balance, status}` |
| 1002 | `CMD_USER_INFO` | 获取用户信息 | `{}` | 同上（不含 token） |
| 1003 | `CMD_USER_SET_NICKNAME` | 修改昵称 | `{nickname}` | `{}` |
| 1004 | `CMD_USER_SET_AVATAR` | 修改头像 | `{avatarPath}` | `{}` |
| 1005 | `CMD_USER_RECHARGE` | 钱包充值（模拟支付）`[说明书]` 1.4 | `{amount}` 单位**分** | `{balance}` |
| 1006 | `CMD_WALLET_TX_LIST` | 查询钱包流水 | `{page, size}` | `{total, list[]}` |

### 4.2 用户端 · 充电站与电桩（1100–1199）

| 命令字 | 常量名 | 说明 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- | --- |
| 1101 | `CMD_STATION_NEARBY` | 附近充电站列表 `[说明书]` 1.4 | `{lng, lat, keyword, sortBy}` | `{list:[{stationId, name, address, price, pileTotal, pileIdle, distance, congestion, idleForecast}]}` |
| 1102 | `CMD_STATION_PILES` | 充电站内电桩详情 `[说明书]` 1.4 | `{stationId}` | `{list:[{pileId, code, type, status, power}]}` |

`price` 单位为**分/度**；`distance` 单位为**米**（显示层换算为公里）。

`sortBy`（`[本组自定]` v1.1，可选，缺省 `0`）：
- `0` 按距离升序 `[说明书]` 1.4「按距离由近及远展示充电站列表」
- `1` 按拥堵度升序，同拥堵度再按距离 `[说明书]` 1.4「优先推荐低拥堵、高空闲率的充电站」

`congestion`（double，0..1，`[本组自定]` v1.1）/ `idleForecast`（int，`[本组自定]` v1.1）：取 `t_load_forecast` 中 `horizon=1` 的最新一条。**无预测数据时两字段均填 `-1`**——`0` 在拥堵度语义中代表「最不拥堵」，不能兼职表示「没有数据」，否则新站或模型未跑到的站会被排到推荐最前面。

### 4.3 用户端 · 充电与订单（1200–1299）

| 命令字 | 常量名 | 说明 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- | --- |
| 1201 | `CMD_ORDER_UNFINISHED` | **查询未完成订单** `[说明书]` 1.4 进入充电页必调 | `{}` | `{hasUnfinished, order}` |
| 1202 | `CMD_ORDER_RESERVE` | 预约电桩 | `{pileId}` | `{orderId}` |
| 1203 | `CMD_ORDER_START` | 开始充电 | `{orderId}` | `{startTime}` |
| 1204 | `CMD_ORDER_STOP` | 结束充电（计费） | `{orderId}` | `{endTime, kwh, amount}` |
| 1205 | `CMD_ORDER_SETTLE` | 订单结算（扣钱包余额） | `{orderId}` | `{amount, balance}` |
| 1206 | `CMD_ORDER_CANCEL` | 取消预约 | `{orderId}` | `{}` |
| 1207 | `CMD_ORDER_LIST` | 我的订单列表 | `{page, size, status}` | `{total, list[]}` |
| 1208 | `CMD_ORDER_PUSH` | 充电中实时数据推送 → | 服务端推送 | `{orderId, kwh, amount, duration}` |

### 4.4 管理端（2000–2399）

| 命令字 | 常量名 | 说明 | 请求 `data` | 响应 `data` |
| --- | --- | --- | --- | --- |
| 2001 | `CMD_ADMIN_LOGIN` | 管理员登录 `[说明书]` 1.4 默认 admin/123456 | `{account, password}` | `{token, adminId, account}` |
| 2101 | `CMD_STATION_LIST` | 充电站列表 `[说明书]` 1.4 | `{page, size}` | `{total, list:[{stationId, name, address, lng, lat, pileTotal, onlineRate}]}` |
| 2102 | `CMD_STATION_ADD` | 新增充电站 `[说明书]` 1.4 | `{name, address, lng, lat, price, pileCount}` | `{stationId}` |
| 2103 | `CMD_STATION_DETAIL` | 站内电桩明细 `[说明书]` 1.4 | `{stationId}` | `{list[]}` |
| 2111 | `CMD_PILE_LIST` | 电桩列表 `[说明书]` 1.4 | `{page, size, stationId, status}` | `{total, list:[{pileId, code, stationName, type, power, status, chargeCount, chargeDuration}]}` |
| 2112 | `CMD_PILE_REBOOT` | **远程重启电桩** `[说明书]` 1.4 | `{pileId}` | `{}` |
| 2201 | `CMD_ADMIN_USER_LIST` | 用户列表 + 手机号模糊搜索 `[说明书]` 1.4 | `{page, size, phoneLike}` | `{total, list:[{userId, phone, nickname, balance, createTime, status}]}` |
| 2202 | `CMD_ADMIN_USER_STATUS` | **冻结 / 解冻用户** `[说明书]` 1.4 | `{userId, status}` | `{}` |
| 2301 | `CMD_STAT_REVENUE` | 营收概览 `[说明书]` 1.4 今日/本月/总营收 | `{}` | `{today, month, total}` 单位**分** |
| 2302 | `CMD_STAT_REVENUE_TREND` | 营收趋势 `[说明书]` 1.4 近 7 / 30 日 | `{days}` 取 7 或 30 | `{list:[{date, amount}]}` |
| 2303 | `CMD_STAT_PILE_STATUS` | 电桩状态分布 `[说明书]` 1.4 在用/闲置/故障 | `{}` | `{inUse, idle, fault, total}` |
| 2304 | `CMD_ADMIN_ORDER_LIST` | 订单列表 | `{page, size, status, dateFrom, dateTo}` | `{total, list[]}` |
| 2305 | `CMD_STAT_LOAD_FORECAST` | 站点负荷预测 / 负荷预警 `[说明书]` 1.4（v1.1） | `{stationId, horizon}` | `{list:[{stationId, stationName, horizon, predictTime, loadKw, idlePile, isPeak, congestion, modelVersion}]}` |

`2305`：`stationId=0` 表示全部站点；`horizon` 取 `1`/`6`/`24`，非法值返回 `ERR_PARAM`。取 `t_load_forecast` 中同一 `model_version` 下 `create_time` 最大的一批。**无预测数据返回 `code=0` + 空 `list`**，不新增错误码——预测缺失是正常状态，不是错误。管理端按 `congestion ≥ 0.8` 本地判定预警，阈值不进协议。`loadKw` 单位 kW，是物理量不是金额，不受「金额整数分」规则约束。

### 4.5 设备侧 · 电桩模拟器（9000–9099）

| 命令字 | 常量名 | 说明 | 方向 | `data` |
| --- | --- | --- | --- | --- |
| 9001 | `CMD_DEV_REGISTER` | 电桩注册上线 | 设备 → 服务端 | `{pileCode}` |
| 9002 | `CMD_DEV_REPORT` | 状态与电量上报 | 设备 → 服务端 | `{pileCode, status, kwh, power}` |
| 9003 | `CMD_DEV_REBOOT` | 重启指令下发 `[说明书]` 1.4 | 服务端 → 设备 | `{pileCode}` |
| 9004 | `CMD_DEV_HEARTBEAT` | 心跳 | 设备 → 服务端 | `{pileCode}` |

## 5. 错误码 `[本组自定]`

对应 `[说明书]` 2.3「需设计完整的错误处理机制」。所有对外接口必须返回本表中的码，**不得返回未定义的错误码**。

| 码 | 常量 | 含义 |
| --- | --- | --- |
| 0 | `ERR_OK` | 成功 |
| **1000 段 · 通用** | | |
| 1001 | `ERR_PARAM` | 参数缺失或格式错误 |
| 1002 | `ERR_NOT_LOGIN` | 未登录 / token 为空 |
| 1003 | `ERR_TOKEN_INVALID` | token 无效或已过期 |
| 1004 | `ERR_NO_PERMISSION` | 无权限（用户 token 调管理端接口等） |
| 1005 | `ERR_CMD_UNKNOWN` | 未知命令字 |
| 1006 | `ERR_FRAME` | 报文格式错误（JSON 解析失败、长度越界） |
| 1099 | `ERR_INTERNAL` | 服务端内部错误（DB 异常等） |
| **2000 段 · 用户** | | |
| 2001 | `ERR_PHONE_FORMAT` | 手机号格式错误（需 11 位）`[说明书]` 1.4 |
| 2002 | `ERR_USER_NOT_FOUND` | 用户不存在 |
| 2003 | `ERR_USER_FROZEN` | 账号已冻结 `[说明书]` 1.4 |
| 2004 | `ERR_BALANCE_NOT_ENOUGH` | 钱包余额不足 |
| 2005 | `ERR_AMOUNT_INVALID` | 金额非法（≤0 或超上限） |
| **3000 段 · 电站与电桩** | | |
| 3001 | `ERR_STATION_NOT_FOUND` | 充电站不存在 |
| 3002 | `ERR_PILE_NOT_FOUND` | 电桩不存在 |
| 3003 | `ERR_PILE_BUSY` | 电桩已被占用（非闲置） |
| 3004 | `ERR_PILE_FAULT` | 电桩故障 `[说明书]` 1.4 |
| 3005 | `ERR_PILE_OFFLINE` | 电桩未上线，指令无法下发 |
| **4000 段 · 订单** | | |
| 4001 | `ERR_ORDER_NOT_FOUND` | 订单不存在 |
| 4002 | `ERR_ORDER_UNFINISHED` | **存在未结算订单，请先结算** `[说明书]` 1.4 |
| 4003 | `ERR_ORDER_STATUS` | 订单状态不允许此操作 |
| 4004 | `ERR_ORDER_SETTLED` | 订单已结算，不可重复结算 |
| **5000 段 · 管理员** | | |
| 5001 | `ERR_ADMIN_AUTH` | 账号或密码错误 `[说明书]` 1.4 |

错误码常量定义见 `common/error_code.h`，**协议表与头文件必须一致**，改一处必须同步另一处。

## 6. 会话与安全 `[本组自定]`

对应 `[说明书]` 2.2「合理考虑数据安全问题」：

- 登录成功后服务端生成 **32 位十六进制 token**，存于内存会话表（`token → {userId | adminId, role, 最后活跃时间}`）
- token **有效期 2 小时**，每次成功请求刷新最后活跃时间；过期返回 `ERR_TOKEN_INVALID`，客户端需重新登录
- 用户 token 与管理员 token **分属不同角色**，用户 token 调用 2000 段命令返回 `ERR_NO_PERMISSION`
- 管理员密码在库中存 **SHA-256 摘要**，不存明文（初始 `admin/123456` 的摘要见 `docs/db-schema.sql`）
- 服务端对所有入参做长度与类型校验；SQL 一律使用 `QSqlQuery::prepare` + `bindValue` **参数绑定**，禁止字符串拼接 SQL
- 管理员冻结用户（2202）后，该用户**全部在线会话立即失效**，后续请求返回 `ERR_TOKEN_INVALID`（客户端应清空本地 token 并跳转登录页，而非静默重试）`[本组自定]` v1.1

> 本项目为实训环境，**不做 TLS 加密**，属于已知取舍，需在设计文档中写明。

## 7. 变更记录

| 日期 | 版本 | 变更 | 提出人 | 评审 |
| --- | --- | --- | --- | --- |
| — | v1.0 | 初版冻结 | L1 | 待全组评审 |
| 2026-09-03 | v1.1 | CR-001：1101 增加 congestion/idleForecast/sortBy；新增 2305 站点负荷预测。CR-003：第 6 节增加冻结用户会话失效说明 | L5（代 L1 落地，L1/L2 暂时无法操作，详见 docs/conventions.md 第 3.2 节） | L1：已复核通过 |
| 2026-09-05 | v1.2 | 文档整理：命令字表增加「常量名」列；§7 线程模型迁至 server/net/README.md | L1 | 待评审 |

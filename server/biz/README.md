# server/biz/ —— 业务服务层

归属 **L2**。业务 handler 在此实现，通过 `Dispatcher::registerHandler()` 注册命令字，
并在 `server/main.cpp` 的 `registerAllServices()` 中调用注册函数。

## 服务清单（对应 docs/protocol.md 第 4 节）

| 文件 | 命令字 | 说明书依据 |
| --- | --- | --- |
| `user_service.cpp` | 1001–1004 | 1.4 手机号免密登录（含自动注册、冻结拦截）、信息维护 |
| `wallet_service.cpp` | 1005–1006 | 1.4 余额充值（模拟支付）、钱包流水 |
| `station_service.cpp` | 1101 · 2101–2103 | 1.4 附近充电站（含 `sortBy` 拥堵度排序）、电站管理 |
| `pile_service.cpp` | 1102 · 2111–2112 | 1.4 电桩详情、电桩列表、远程重启（下发 9003） |
| `reservation_service.cpp` | 1202 · 1206 | 1.4 预约充电、取消预约 |
| `order_service.cpp` | 1201 · 1207 · 2304 | 1.4 未结算订单校验、订单列表 |
| `order_flow_service.cpp` | 1203–1205 | 1.4 开始充电、结束充电、计费结算（**实现方 L4**，见 ARCHITECTURE 第 5 节）|
| `user_management_service.cpp` | 2201–2202 | 1.4 手机号模糊搜索、冻结/解冻（冻结即踢会话，CR-003）|
| `statistics_service.cpp` | 2301–2303 · 2305 | 1.4 营收三指标、近 7/30 日趋势、电桩状态分布、负荷预测 |
| `admin_service.cpp` | 2001 | 1.4 管理员登录 |
| `ext_08_carbon_calc.{h,cpp}` `ext_08_carbon_service.cpp` | 3740–3747 | 扩展模块 08 碳减排与能源报告（**实现方 L5**）|

设备侧 9001–9006 在 `server/net/`（归属 L1），见 [../net/README.md](../net/README.md)。

## 写法范式

`user_service.cpp` 与 `admin_service.cpp` 是最短的样板，新 handler 照抄它们的结构最快：

```cpp
Dispatcher::instance().registerHandler(CMD_USER_LOGIN,
    [](const Request &req, QJsonObject &out) -> int {
        const QString phone = req.data.value("phone").toString();
        if (phone.size() != 11) return ERR_PHONE_FORMAT;   // [说明书] 1.4 11 位手机号
        // ... 查库、不存在则自动注册
        return ERR_OK;
    }, false);   // 第三个参数 = 是否需要 token，登录类传 false
```

## 硬性要求

- 金额一律 `qint64` 整数「分」，禁止 `double`（根 CLAUDE.md 规则 3）
- 取数据库连接一律用 `ecp::threadDb()`，禁止自己 `addDatabase`（规则 2）
- SQL 一律 `prepare` + `bindValue` 参数绑定，禁止字符串拼接
- 返回值只能是 `common/error_code.h`（扩展模块 `common/error_code_ext.h`）里定义的错误码
- 鉴权接口只信 `req.session.id`，不信客户端传来的 id

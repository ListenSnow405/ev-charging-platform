# server/biz/ —— 业务服务层

归属 **L2**。九个业务服务在此实现，通过 `Dispatcher::registerHandler()` 注册命令字。

## 待实现（对应 docs/protocol.md 第 4 节）

| 服务 | 命令字 | 说明书依据 |
| --- | --- | --- |
| 用户服务 | 1001–1004 | 1.4 手机号免密登录、信息维护 |
| 钱包服务 | 1005–1006 | 1.4 余额充值（模拟支付） |
| 电站服务 | 1101, 2101–2103 | 1.4 附近充电站、电站管理 |
| 电桩服务 | 1102, 2111–2112 | 1.4 电桩详情、远程重启 |
| 预约服务 | 1202, 1206 | 1.4 预约充电 |
| 订单服务 | 1201, 1207, 2304 | 1.4 未结算订单校验、订单列表 |
| 计费结算服务 | 1203–1205 | 1.4 开始充电、计费、结算 |
| 用户管理服务 | 2201–2202 | 1.4 模糊搜索、冻结/解冻 |
| 统计服务 | 2301–2303 | 1.4 营收指标、趋势、电桩状态分布 |

## 注册方式

```cpp
#include "dispatcher.h"
#include "protocol.h"

void registerUserService()
{
    // 登录不需要 token，第三个参数传 false
    Dispatcher::instance().registerHandler(CMD_USER_LOGIN,
        [](const Request &req, QJsonObject &out) -> int {
            const QString phone = req.data.value("phone").toString();
            if (phone.size() != 11) return ERR_PHONE_FORMAT;   // [说明书] 1.4 11 位手机号
            // ... 查库、不存在则自动注册
            return ERR_OK;
        }, false);
}
```

在 `server/main.cpp` 的 `registerAllServices()` 中调用。

## 硬性要求

- 金额一律 `qint64` 整数「分」，禁止 `double`（CLAUDE.md 规则 3）
- 取数据库连接一律用 `ecp::threadDb()`，禁止自己 `addDatabase`（CLAUDE.md 规则 2）
- SQL 一律 `prepare` + `bindValue` 参数绑定，禁止字符串拼接（docs/protocol.md 第 6 节）
- 返回值只能是 `common/error_code.h` 里定义的错误码

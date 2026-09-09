# user-client/ —— 充电用户端

> **归属 L4**。只写本模块规则；全组共识见根 [CLAUDE.md](../CLAUDE.md)，冲突时以根文件为准。
> `docs/` 与 `common/` 是只读冻结契约。

## 界面清单（[说明书] 1.4）

五个页签全部接真实链路，`mock/` 已不再使用。

| 界面 | 命令字 |
| --- | --- |
| 手机号免密登录 / 首次自动注册（含 11 位本地校验） | 1001 |
| 附近充电站：按距离升序，可切换按拥堵度推荐（`sortBy=1`） | 1101 / 1102 |
| 一键导航：QWebEngineView 加载腾讯地图，驾车/步行路线 | — |
| 个人中心：头像 / 昵称 / 充值 / 钱包流水 | 1002–1006 |
| 充电全流程：未结算拦截 → 预约 → 开始 → 结束 → 结算，充电中接 1208 推送 | 1201–1208 |

## 本模块特有规则

1. **QtWebEngine 需单独安装**（约 400MB，`bash scripts/check-env.sh L4` 会给出命令）。
   `.pro` 用 `qtHaveModule(webenginewidgets)` 守卫，未装也能编译。
   **编译期有模块 ≠ 运行期能用**：辅助进程 `QtWebEngineProcess` 由 `libqt6webenginecore6-bin`
   单独提供，缺它时 `new QWebEngineView` 直接 `qFatal` 中止进程（登录后进主窗口即崩，无法 try/catch）。
   因此建 WebEngine 控件前一律先过 `ecp::webEngineAvailable()`（`webengine_env.h`），
   返回 false 就走占位页。缺失时补装：`sudo apt install libqt6webenginecore6-bin`。
2. **进入充电页前必须先发 1201 查未结算订单**——[说明书] 1.4 明确要求：有未完成订单则弹窗提示
   「您有未完成的充电订单，请先结算」并**强制跳转结算页**。这是硬性业务规则，不能只做提示不做跳转。
3. **收到 `ERR_TOKEN_INVALID` 必须清空本地 token 并跳转登录页**，不能静默重试——否则会拿着失效 token 死循环。
   管理员冻结用户（2202）后会话立即失效（CR-003），走的就是这条路径；真实原因要等重新登录时的 `ERR_USER_FROZEN` 才显示。
4. **地图 Key 从 `config/app.ini` 的 `[map] key` 读取**，禁止硬编码进源码、`.pro` 或提交信息。
   每人各申请一个，申请步骤见 [docs/map_key.md](../docs/map_key.md)。
5. **金额显示用 `ecp::fenToYuan()`**；充值输入用 `ecp::yuanToFen()` 转换，返回 -1 表示非法。

## 第二顶帽子

**测试与回归主责**：维护全组测试用例集，重点是 agent 容易漏的**异常路径**——余额不足、账号冻结、
断网重连、重复提交、未结算拦截。测试入口见 [docs/RUNBOOK.md 第 5 节](../docs/RUNBOOK.md)。

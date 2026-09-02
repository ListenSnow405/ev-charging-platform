# common/ —— 全项目共享基座

> **冻结契约** · 属主 **L1**，其他人只读。变更走 [CLAUDE.md](../CLAUDE.md) 第 3 节流程。

五条工作线共用同一份协议定义、错误码、日志格式与时间格式。**不要在自己的模块里重新发明一套**——那正是五个 agent 各写各的、最后拼不到一起的起点。

| 文件 | 内容 | 对应契约 |
| --- | --- | --- |
| `protocol.h` | 命令字、业务枚举、报文信封的构造与解析 | `docs/protocol.md` 第 3、4 节 |
| `error_code.h` | 全部错误码与中文提示语 | `docs/protocol.md` 第 5 节 |
| `frame.h` | 4 字节长度头 + JSON 体的编解码，**处理粘包与半包** | `docs/protocol.md` 第 2 节 |
| `logger.h` | `LOG_I / LOG_W / LOG_E` 统一日志宏 | CLAUDE.md 第 7 节 |
| `time_util.h` | 时间字符串格式、分↔元换算 | `docs/db-schema.sql` 表头说明 |
| `common.pri` | qmake 引入片段 | — |

## 接入方式

在各子工程的 `.pro` 中：

```pro
include(../common/common.pri)
```

然后直接 `#include "protocol.h"`。

## 收发报文的唯一正确写法

```cpp
#include "frame.h"
#include "protocol.h"

// ---- 发送 ----
QByteArray payload = ecp::buildRequest(ecp::CMD_USER_LOGIN, ++m_seq, "",
                                       QJsonObject{{"phone", phone}});
socket->write(ecp::encodeFrame(payload));

// ---- 接收（成员变量：ecp::FrameParser m_parser;）----
m_parser.append(socket->readAll());   // 有多少喂多少：半包、整包、多包都可以
QByteArray one;
while (m_parser.next(one)) {
    QJsonObject env;
    if (!ecp::parseEnvelope(one, env)) { /* 回 ERR_FRAME */ continue; }
    // 按 env["cmd"] 分发
}
if (m_parser.overflow()) { /* 长度头越界：记日志并关闭连接 */ }
```

> ⚠️ **禁止**把一次 `read()` / `readAll()` 的结果直接当成一个完整报文去 `fromJson()`。TCP 是字节流，没有消息边界。这是 CLAUDE.md 硬性规则第 1 条。

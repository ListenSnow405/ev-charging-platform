# 数据质量报告

> T3 阶段 6 产出，对应《数据清洗基本流程-操作SOP》第 8 节。
> **本文件由 `bigdata/spark/quality_report.py` 从阶段 1–5 的产物自动生成，请勿手改。**
> 生成时间 2026-09-15 15:19:37

## 1. 数据概况

- 数据源：`charging.db`（SQLite，一阶段业务库）
- 源库指纹：`2eff8f6c1537b4eb`
- ODS 导出时间：2026-09-14 12:37:20
- 订单时间范围：2026-07-07 00:16:00 ~ 2026-09-04 23:44:00

| 表 | 行数 | 列数 | 完全重复 |
| --- | ---: | ---: | ---: |
| `t_order` | 8292 | 13 | 0 |
| `t_pile` | 24 | 11 | 0 |
| `t_station` | 6 | 8 | 0 |
| `t_user` | 5 | 8 | 0 |
| `t_pile_log` | 1800 | 8 | 0 |
| `t_load_forecast` | 18 | 10 | 0 |
| `t_carbon_daily` | 360 | 18 | 0 |
| `t_carbon_factor` | 2 | 9 | 0 |
| `t_carbon_report` | 5 | 22 | 0 |
| `t_wallet_tx` | 1 | 8 | 0 |
| `t_station_review` | 0 | 7 | 0 |
| `t_sys_config` | 7 | 4 | 0 |
| `t_admin` | 1 | 7 | 0 |
| `t_admin_oplog` | 3 | 6 | 0 |
| **合计** | **10524** | | **0** |

## 2. 六维度质量评估

| 维度 | 问题数 | 结论 |
| --- | ---: | --- |
| 完整性 | 8 | 关键字段无异常缺失；9 个高缺失字段已登记为合理缺失（见 3.1）；`t_pile_log` 的 `old_status`、`new_status` 近乎全空已剔除 |
| 准确性 | 0 | 6477 笔已结算订单金额与单价×电量逐笔吻合（容差 1 分） |
| 一致性 | 1 | 冗余字段 `station_id` 与 `t_pile` 逐行一致；冻结用户订单属正常业务 |
| 时效性 | 1 | 最新结算 2026-09-04 23:59:59，滞后 10 天 |
| 唯一性 | 0 | 完全重复行 0 行，`order_no` 无重复 |
| 有效性 | 1 | 状态枚举、金额非负、时间顺序全部合规；1 条手机号不合规 |

## 3. 问题清单

| 编号 | 维度 | 对象 | 问题 | 影响 | 严重度 | 处理 |
| --- | --- | --- | --- | ---: | --- | --- |
| Q001 | 有效性 | `t_user`.phone | 手机号不符合规则 | 1 (20.00%) | 中 | 标记待核；[说明书] 要求 11 位手机号，疑为测试残留 |
| Q002 | 一致性 | `t_order / t_user`.user_id | 冻结用户存在订单 | 4136 (49.88%) | 低 | 保留。冻结是后置管理动作，历史订单本就该在；仅作口径说明 |
| Q003 | 时效性 | `t_order`.settle_time | 数据滞后 | 8292 (100.00%) | 中 | 离线合成数据集，滞后属预期；答辩时说明口径 |
| Q004 | 完整性 | `t_pile_log`.old_status | 字段缺失率 100.0%（>50%） | 1800 (100.00%) | 高 | 该字段近乎全空，承载不了分析；剔除或改用其他字段 |
| Q005 | 完整性 | `t_pile_log`.new_status | 字段缺失率 98.67%（>50%） | 1776 (98.67%) | 高 | 该字段近乎全空，承载不了分析；剔除或改用其他字段 |
| Q006 | 完整性 | `t_pile_log`.event | 事件类型无任何记录 | 0 (0.00%) | 高 | 依赖这些事件的分析维度不成立，须在报告与维度设计中删除 |
| Q007 | 完整性 | `t_load_forecast`.(整表) | 事实表数据量偏少，限制可做的分析 | 18 (100.00%) | 低 | 可做快照类展示，不足以支撑趋势/分布类维度 |
| Q008 | 完整性 | `t_carbon_report`.(整表) | 事实表数据量偏少，限制可做的分析 | 5 (100.00%) | 低 | 可做快照类展示，不足以支撑趋势/分布类维度 |
| Q009 | 完整性 | `t_wallet_tx`.(整表) | 事实表数据量严重不足 | 1 (100.00%) | 中 | 不纳入分析维度，在报告中说明 |
| Q010 | 完整性 | `t_station_review`.(整表) | 空表 | 0 (—) | 高 | 不纳入分析维度，在质量报告中说明 |
| Q011 | 完整性 | `t_admin_oplog`.(整表) | 事实表数据量严重不足 | 3 (100.00%) | 中 | 不纳入分析维度，在报告中说明 |

## 3.1 合理缺失登记

下列字段的缺失**经判定属业务正常**，不计入问题清单、不做任何填充。登记按实测缺失率生成，字段消失或已无缺失会自动标注为「登记已过期」。

| 表 | 字段 | 缺失数 | 缺失率 | 超过 SOP 50% 阈值 | 判定 | 理由 |
| --- | --- | ---: | ---: | :---: | --- | --- |
| `t_admin_oplog` | `detail` | 3 | 100.0% | 是 | 合理缺失，不计为问题 | detail 是可选备注字段 |
| `t_carbon_factor` | `effect_to` | 1 | 50.0% | 否 | 合理缺失，不计为问题 | 当前生效的因子没有截止时刻，开口区间是设计如此 |
| `t_carbon_report` | `output_path` | 2 | 40.0% | 否 | 合理缺失，不计为问题 | 尚未导出的报告没有文件路径 |
| `t_order` | `end_time` | 1815 | 21.89% | 否 | 合理缺失，不计为问题 | 同上 |
| `t_order` | `settle_time` | 1815 | 21.89% | 否 | 合理缺失，不计为问题 | 同上 |
| `t_order` | `start_time` | 1815 | 21.89% | 否 | 合理缺失，不计为问题 | 已取消订单从未开始充电，1815 笔缺失 = 取消单数，恒等 |
| `t_pile` | `last_heartbeat` | 23 | 95.83% | 是 | 合理缺失，不计为问题 | 离线电桩本就没有心跳时间 |
| `t_user` | `avatar` | 5 | 100.0% | 是 | 合理缺失，不计为问题 | [说明书] 规定默认灰色头像，空值即默认，非异常 |
| `t_wallet_tx` | `order_id` | 1 | 100.0% | 是 | 合理缺失，不计为问题 | 充值类流水不关联订单 |

## 4. 处理动作

规则全文见 [03_rules.md](03_rules.md)，执行日志见 [04_clean_log.json](04_clean_log.json)。

| 规则 | 动作 | 结果 |
| --- | --- | --- |
| R001 | 伪缺失归一 | 命中=0 |
| R002 | 整数列类型转换 | 表=t_order　列数=8　失败行=0 |
| R003 | 时间列转 timestamp | 表=t_order　列数=4　失败行=0 |
| R004 | 金额/电量保持整数 | 断言=amount·price·kwh_x100 均为 bigint |
| R005 | 电桩类型/状态码表归一 | 电桩=24　站点=6 |
| R010 | 派生分析字段 | 新增列=7　负时长=0 |
| R011 | 外键完整性 | 孤儿订单=0 |
| R007 | 冻结用户历史订单：保留不动 | 涉及行=4136　删除=0 |
| 写出 | dwd_order.parquet | 输入=8292　输出=8292 |
| R006 | 非法手机号标记待核（保留原值） | 标记=1　删除=0　表行数=5 |
| R002/R003·t_user | 用户表类型转换（余额转整数分） | 失败行=0 |
| R002/R003·t_pile | 维表类型转换（失败进待核） | 列数=10　失败行=0　表行数=24 |
| R002/R003·t_station | 维表类型转换（失败进待核） | 列数=5　失败行=0　表行数=6 |
| R002/R003·t_pile_log | 事件表类型转换（失败进待核） | 列数=3　失败行=0 |
| R008 | 剔除近乎全空的状态列 | 剔除列=['old_status', 'new_status']　保留行=1800　有效事件=37 |
| R009 | 低量/空表仍导入，不删数据 | 表=['t_carbon_daily(360)', 't_load_forecast(18)', 't_carbon_report(5)', 't_wallet_tx(1)', 't_station_review(0)', 't_carbon_factor(2)'] |
| R004·全表 | 金额与电量列一律整数 | 覆盖=11 张 DWD 表 132 列按契约断言 |

**本轮汇总：删除 0 行，修改值 0 处，标记待核 1 条，剔除列 ['old_status', 'new_status']，订单 8292 → 8292（不变）。**

**类型转换失败对账（R002/R003）**：`t_carbon_daily` 0 行、`t_carbon_factor` 0 行、`t_carbon_report` 0 行、`t_load_forecast` 0 行、`t_order` 0 行、`t_pile` 0 行、`t_pile_log` 0 行、`t_station` 0 行、`t_station_review` 0 行、`t_user` 0 行、`t_wallet_tx` 0 行，合计 0 行。四张表各有一份待核清单，**空清单也落文件**——「查过且为空」与「没查」必须能区分。

> 全程**零删除**。SOP 第 5 节把删除列为最后手段，本轮发现的问题要么可标记（R006）、要么属正常业务（R007）、要么是分析范围问题而非数据错误（R009），都不需要动行。

## 5. 清洗前后对比与校验

阶段 5 共 82 项检查，失败 0 项。

| 检查项 | 结论 | 详情 |
| --- | --- | --- |
| 订单行数对账 | **PASS** | ODS 8292 → DWD 8292（本轮无删除，应相等） |
| 业务主键非空且唯一 | **PASS** | order_no 空值 0，重复 0 |
| 金额/电量为整数型（R004） | **PASS** | amount=bigint price=bigint kwh_x100=bigint |
| 时间列为 timestamp（R003） | **PASS** | reserve_time=timestamp |
| 时间列非空数无漂移 | **PASS** | 四列逐列一致 |
| dwd_order 符合 Schema 契约（列序+类型） | **PASS** | 20 列全部一致 |
| dwd_order 非空约束 | **PASS** | 14 个非空列均无 NULL |
| dwd_order 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；12 个转换列非空数逐列一致 |
| dwd_user 符合 Schema 契约（列序+类型） | **PASS** | 10 列全部一致 |
| dwd_user 非空约束 | **PASS** | 9 个非空列均无 NULL |
| dwd_user 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；5 个转换列非空数逐列一致 |
| dwd_pile 符合 Schema 契约（列序+类型） | **PASS** | 13 列全部一致 |
| dwd_pile 非空约束 | **PASS** | 12 个非空列均无 NULL |
| dwd_pile 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；10 个转换列非空数逐列一致 |
| dwd_station 符合 Schema 契约（列序+类型） | **PASS** | 8 列全部一致 |
| dwd_station 非空约束 | **PASS** | 8 个非空列均无 NULL |
| dwd_station 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；6 个转换列非空数逐列一致 |
| dwd_pile_log 符合 Schema 契约（列序+类型） | **PASS** | 7 列全部一致 |
| dwd_pile_log 非空约束 | **PASS** | 6 个非空列均无 NULL |
| dwd_pile_log 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；4 个转换列非空数逐列一致 |
| dwd_carbon_daily 符合 Schema 契约（列序+类型） | **PASS** | 18 列全部一致 |
| dwd_carbon_daily 非空约束 | **PASS** | 18 个非空列均无 NULL |
| dwd_carbon_daily 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；15 个转换列非空数逐列一致 |
| dwd_carbon_factor 符合 Schema 契约（列序+类型） | **PASS** | 9 列全部一致 |
| dwd_carbon_factor 非空约束 | **PASS** | 8 个非空列均无 NULL |
| dwd_carbon_factor 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；6 个转换列非空数逐列一致 |
| dwd_carbon_report 符合 Schema 契约（列序+类型） | **PASS** | 22 列全部一致 |
| dwd_carbon_report 非空约束 | **PASS** | 16 个非空列均无 NULL |
| dwd_carbon_report 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；14 个转换列非空数逐列一致 |
| dwd_load_forecast 符合 Schema 契约（列序+类型） | **PASS** | 10 列全部一致 |
| dwd_load_forecast 非空约束 | **PASS** | 9 个非空列均无 NULL |
| dwd_load_forecast 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；9 个转换列非空数逐列一致 |
| dwd_wallet_tx 符合 Schema 契约（列序+类型） | **PASS** | 8 列全部一致 |
| dwd_wallet_tx 非空约束 | **PASS** | 6 个非空列均无 NULL |
| dwd_wallet_tx 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；7 个转换列非空数逐列一致 |
| dwd_station_review 符合 Schema 契约（列序+类型） | **PASS** | 7 列全部一致 |
| dwd_station_review 非空约束 | **PASS** | 5 个非空列均无 NULL |
| dwd_station_review 转换无静默置空（R002/R003） | **PASS** | 待核清单 0 行；6 个转换列非空数逐列一致 |
| dwd_order 主键非空且唯一 | **PASS** | 8292 行；order_id + order_no 均无空值与重复 |
| dwd_user 主键非空且唯一 | **PASS** | 5 行；user_id + phone 均无空值与重复 |
| dwd_pile 主键非空且唯一 | **PASS** | 24 行；pile_id + pile_code 均无空值与重复 |
| dwd_station 主键非空且唯一 | **PASS** | 6 行；station_id + name 均无空值与重复 |
| dwd_pile_log 主键非空且唯一 | **PASS** | 1800 行；log_id 均无空值与重复 |
| dwd_carbon_daily 主键非空且唯一 | **PASS** | 360 行；daily_id 均无空值与重复 |
| dwd_carbon_factor 主键非空且唯一 | **PASS** | 2 行；factor_id 均无空值与重复 |
| dwd_carbon_report 主键非空且唯一 | **PASS** | 5 行；report_id 均无空值与重复 |
| dwd_load_forecast 主键非空且唯一 | **PASS** | 18 行；forecast_id 均无空值与重复 |
| dwd_wallet_tx 主键非空且唯一 | **PASS** | 1 行；tx_id 均无空值与重复 |
| dwd_station_review 主键非空且唯一 | **PASS** | 0 行；review_id 均无空值与重复 |
| dwd_order 外键完整性（3 条） | **PASS** | 全部命中维表 |
| dwd_pile 外键完整性（1 条） | **PASS** | 全部命中维表 |
| dwd_pile_log 外键完整性（1 条） | **PASS** | 全部命中维表 |
| dwd_carbon_daily 外键完整性（1 条） | **PASS** | 全部命中维表 |
| dwd_load_forecast 外键完整性（1 条） | **PASS** | 全部命中维表 |
| dwd_wallet_tx 外键完整性（2 条） | **PASS** | 全部命中维表 |
| dwd_station_review 外键完整性（3 条） | **PASS** | 全部命中维表 |
| dwd_order 取值范围（9 条规则） | **PASS** | 9 条规则全部满足 |
| dwd_user 取值范围（3 条规则） | **PASS** | 3 条规则全部满足 |
| dwd_pile 取值范围（6 条规则） | **PASS** | 6 条规则全部满足 |
| dwd_station 取值范围（4 条规则） | **PASS** | 4 条规则全部满足 |
| dwd_pile_log 取值范围（1 条规则） | **PASS** | 1 条规则全部满足 |
| dwd_carbon_daily 取值范围（6 条规则） | **PASS** | 6 条规则全部满足 |
| dwd_carbon_factor 取值范围（3 条规则） | **PASS** | 3 条规则全部满足 |
| dwd_carbon_report 取值范围（5 条规则） | **PASS** | 5 条规则全部满足 |
| dwd_load_forecast 取值范围（4 条规则） | **PASS** | 4 条规则全部满足 |
| dwd_wallet_tx 取值范围（3 条规则） | **PASS** | 3 条规则全部满足 |
| dwd_station_review 取值范围（1 条规则） | **PASS** | 1 条规则全部满足 |
| dwd_order 时间连续性（每日应有订单） | **PASS** | 2026-07-07 ~ 2026-09-04 共 60 天 × 1 组 = 60 点，实际 60 点，无缺口 |
| dwd_carbon_daily 时间连续性（每站每日一条） | **PASS** | 2026-07-07 ~ 2026-09-04 共 60 天 × 6 组 = 360 点，实际 360 点，无缺口 |
| amount 分布前后一致 | **PASS** | 计数/合计/极值/四分位逐项相等（中位数 6499） |
| kwh_x100 分布前后一致 | **PASS** | 计数/合计/极值/四分位逐项相等（中位数 4400） |
| price 分布前后一致 | **PASS** | 计数/合计/极值/四分位逐项相等（中位数 145） |
| 订单状态分布前后一致 | **PASS** | 2 类状态计数逐一相等：{'3': 6477, '4': 1815}（历史单只落终态，0/1/2 本就不该出现） |
| 已结算营收逐分对账 | **PASS** | ODS 53936279 分 vs DWD 53936279 分 |
| 已结算电量对账 | **PASS** | 36638035 == 36638035（×100 度） |
| 码表归一无未知值（R005） | **PASS** | status_label 无「未知」 |
| 时间顺序 end >= start | **PASS** | 倒置 0 行 |
| 派生时长非负（R010） | **PASS** | 负值 0 行 |
| 派生单价与快照单价一致 | **PASS** | 偏差超 1 分的 0 行 |
| 取消单时长保持为空（合理缺失未被填充） | **PASS** | 被误填 0 行 |
| 非法手机号只标记不删除（R006） | **PASS** | t_user 行数 ODS 5 → DWD 5，其中 phone_valid=false 1 行（标记保留，未删） |
| 近乎全空列已剔除（R008） | **PASS** | dwd_pile_log 列：7 个，有效事件 37 条 |

> 营收与电量的**逐分/逐度对账**是最硬的一条：清洗前后完全相等（营收 53,936,279 分，电量 36,638,035 ×100 度），说明标准化与派生没有引入任何数值漂移。电量合计与第一阶段碳排放对拍脚本独立算出的数字一致，构成一次跨阶段交叉验证。

### 5.1 时间连续性与关键分布前后对比

| 表 | 区间 | 期望点数 | 实际点数 | 缺口 |
| --- | --- | ---: | ---: | ---: |
| `dwd_order` | 2026-07-07 ~ 2026-09-04 | 60 | 60 | 0 |
| `dwd_carbon_daily` | 2026-07-07 ~ 2026-09-04 | 360 | 360 | 0 |

| 字段 | 计数 | 合计 | 最小 | 最大 | Q1 / 中位 / Q3 | 前后 |
| --- | ---: | ---: | ---: | ---: | --- | :---: |
| `amount` | 8292 | 5.39363e+07 | 0 | 18232 | 2390 / 6499 / 10301 | 一致 |
| `kwh_x100` | 8292 | 3.6638e+07 | 0 | 11395 | 1627 / 4400 / 7006 | 一致 |
| `price` | 8292 | 1.221e+06 | 138 | 160 | 140 / 145 / 152 | 一致 |

> 表中为 DWD 侧数值；「前后」一列标明与 ODS 直算结果是否**逐项完全相等**。本轮零删除零改值，所以标准定到最严：有一项不等即判失败。

## 5.1 DWD Schema 契约（数据字典）

共 11 张表 132 列。类型口径以 `docs/db-schema.sql`（冻结契约）为准；`[派生]` 列由清洗规则生成。**列序也是契约的一部分**——转换会把列挪位，不声明就会随实现细节漂移。机器可读版见 [09_dwd_schema.json](09_dwd_schema.json)。

<details><summary><code>dwd_order</code>　20 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `order_id` | bigint | 否 | 源库自增主键 |
| 2 | `order_no` | string | 否 | 业务主键：ORD + yyyyMMddHHmmss + 4 位随机 |
| 3 | `user_id` | bigint | 否 | 外键 → t_user |
| 4 | `pile_id` | bigint | 否 | 外键 → t_pile |
| 5 | `station_id` | bigint | 否 | 外键 → t_station（源库冗余字段，便于按站统计） |
| 6 | `status` | bigint | 否 | 0=已预约 1=充电中 2=待结算 3=已结算 4=已取消 |
| 7 | `price` | bigint | 否 | 下单时单价快照，单位**分/度** |
| 8 | `kwh_x100` | bigint | 否 | 充电量 × 100，整数避免浮点 |
| 9 | `amount` | bigint | 否 | 应付金额，单位**分** |
| 10 | `reserve_time` | timestamp | 否 | 预约时刻 |
| 11 | `start_time` | timestamp | 是 | 开始充电时刻；取消单为空（合理缺失） |
| 12 | `end_time` | timestamp | 是 | 结束充电时刻；同上 |
| 13 | `settle_time` | timestamp | 是 | 结算时刻；同上 |
| 14 | `status_label` | string | 否 | [派生] R005 码表归一，原码值保留在 status |
| 15 | `charge_minutes` | bigint | 是 | [派生] R010 end−start，整数分钟；取消单为空 |
| 16 | `wait_minutes` | bigint | 是 | [派生] R010 start−reserve，整数分钟；取消单为空 |
| 17 | `unit_price_fen` | bigint | 是 | [派生] R010 amount×100÷kwh_x100，整数分/度；电量为 0 时空 |
| 18 | `order_date` | date | 否 | [派生] R010 预约日期，供按日聚合 |
| 19 | `order_hour` | bigint | 否 | [派生] R010 预约小时 0–23，供负荷分析 |
| 20 | `is_weekend` | boolean | 否 | [派生] R010 是否周末，供工作日/周末对比 |

</details>

<details><summary><code>dwd_user</code>　10 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `user_id` | bigint | 否 | 源库自增主键 |
| 2 | `phone` | string | 否 | 11 位手机号，用户端免密登录主键 |
| 3 | `nickname` | string | 否 | 默认「用户」+ 后 4 位 |
| 4 | `avatar` | string | 是 | 图片路径。源库 NOT NULL DEFAULT ''，**空串=默认灰色头像**；空串经 R001 归一为 NULL，语义仍是「用默认头像」而非「未知」 |
| 5 | `balance` | bigint | 否 | 钱包余额，单位**分** |
| 6 | `status` | bigint | 否 | 0=正常 1=冻结 |
| 7 | `create_time` | timestamp | 否 | 注册时间 |
| 8 | `update_time` | timestamp | 否 | 更新时间 |
| 9 | `phone_valid` | boolean | 否 | [派生] R006 手机号是否合规；false 者进待核，不删不改 |
| 10 | `status_label` | string | 否 | [派生] R005 正常 / 冻结 |

</details>

<details><summary><code>dwd_pile</code>　13 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `pile_id` | bigint | 否 | 源库自增主键 |
| 2 | `pile_code` | string | 否 | 电桩编号，如 SZ001-03 |
| 3 | `station_id` | bigint | 否 | 外键 → t_station |
| 4 | `type` | bigint | 否 | 0=快充 1=慢充 |
| 5 | `power` | double | 否 | 额定功率，单位 kW |
| 6 | `status` | bigint | 否 | 0=在用 1=闲置 2=故障 |
| 7 | `charge_count` | bigint | 否 | 累计充电次数 |
| 8 | `charge_duration` | bigint | 否 | 累计充电时长，单位**秒** |
| 9 | `online` | bigint | 否 | 0=离线 1=在线 |
| 10 | `last_heartbeat` | timestamp | 是 | 最近心跳；离线桩本就没有（合理缺失） |
| 11 | `create_time` | timestamp | 否 | 建档时间 |
| 12 | `type_label` | string | 否 | [派生] R005 快充 / 慢充 |
| 13 | `status_label` | string | 否 | [派生] R005 在用 / 闲置 / 故障 |

</details>

<details><summary><code>dwd_station</code>　8 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `station_id` | bigint | 否 | 源库自增主键 |
| 2 | `name` | string | 否 | 站名 |
| 3 | `address` | string | 否 | 详细地址 |
| 4 | `lng` | double | 否 | 经度 |
| 5 | `lat` | double | 否 | 纬度 |
| 6 | `price` | bigint | 否 | 充电价格，单位**分/度** |
| 7 | `status` | bigint | 否 | 0=营业 1=停业 |
| 8 | `create_time` | timestamp | 否 | 建站时间 |

</details>

<details><summary><code>dwd_pile_log</code>　7 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `log_id` | bigint | 否 | 源库自增主键 |
| 2 | `pile_id` | bigint | 否 | 外键 → t_pile |
| 3 | `event` | bigint | 否 | 0=上线 1=离线 2=状态变更 3=远程重启 4=故障上报 |
| 4 | `operator` | string | 否 | 管理员账号或 system |
| 5 | `detail` | string | 是 | 事件详情。源库 NOT NULL DEFAULT ''，空串经 R001 归一为 NULL |
| 6 | `create_time` | timestamp | 否 | 事件时刻 |
| 7 | `event_label` | string | 否 | [派生] R005 事件中文标签 |

</details>

<details><summary><code>dwd_carbon_daily</code>　18 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `daily_id` | bigint | 否 | 源库自增主键 |
| 2 | `station_id` | bigint | 否 | 外键 → t_station |
| 3 | `stat_date` | date | 否 | 统计日期 |
| 4 | `total_kwh_x100` | bigint | 否 | 当日总电量 × 100 |
| 5 | `peak_kwh_x100` | bigint | 否 | 峰段电量 × 100 |
| 6 | `flat_kwh_x100` | bigint | 否 | 平段电量 × 100 |
| 7 | `valley_kwh_x100` | bigint | 否 | 谷段电量 × 100 |
| 8 | `unalloc_kwh_x100` | bigint | 否 | 未分配到峰平谷的电量 × 100 |
| 9 | `order_cnt` | bigint | 否 | 当日订单数 |
| 10 | `emission_g` | bigint | 否 | 碳排放量，单位**克** |
| 11 | `intensity_g_per_kwh` | bigint | 否 | 碳强度 克/度；-1 表示不可计算 |
| 12 | `completeness` | bigint | 否 | 数据完整度 0–100；-1 表示未知 |
| 13 | `estimated_pct` | bigint | 否 | 估算占比 0–100；-1 表示未知 |
| 14 | `quality` | string | 否 | METER_INTERVAL / UNIFORM_ESTIMATE / UNAVAILABLE |
| 15 | `factor_version` | string | 否 | 所用排放因子版本 |
| 16 | `algo_version` | string | 否 | 算法版本 |
| 17 | `cutoff_time` | timestamp | 否 | 数据截止时刻 |
| 18 | `update_time` | timestamp | 否 | 更新时刻 |

</details>

<details><summary><code>dwd_carbon_factor</code>　9 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `factor_id` | bigint | 否 | 源库自增主键 |
| 2 | `region` | string | 否 | 区域 |
| 3 | `version` | string | 否 | 因子版本 |
| 4 | `effect_from` | timestamp | 否 | 生效起 |
| 5 | `effect_to` | timestamp | 是 | 生效止；**当前生效的因子为空（开口区间，设计如此）** |
| 6 | `factor_g_per_kwh` | bigint | 否 | 排放因子 克/度 |
| 7 | `source` | string | 否 | 因子来源 |
| 8 | `enabled` | bigint | 否 | 0=停用 1=启用 |
| 9 | `create_time` | timestamp | 否 | 创建时间 |

</details>

<details><summary><code>dwd_carbon_report</code>　22 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `report_id` | bigint | 否 | 源库自增主键 |
| 2 | `scope` | string | 否 | STATION / ALL |
| 3 | `station_id` | bigint | 否 | 0 表示全站范围 |
| 4 | `date_from` | date | 否 | 报告起始日 |
| 5 | `date_to` | date | 否 | 报告截止日 |
| 6 | `version` | bigint | 否 | 报告版本，≥ 1 |
| 7 | `status` | string | 否 | GENERATING / READY / FAILED / STALE |
| 8 | `total_kwh_x100` | bigint | 否 | 总电量 × 100 |
| 9 | `peak_kwh_x100` | bigint | 否 | 峰段电量 × 100 |
| 10 | `flat_kwh_x100` | bigint | 否 | 平段电量 × 100 |
| 11 | `valley_kwh_x100` | bigint | 否 | 谷段电量 × 100 |
| 12 | `unalloc_kwh_x100` | bigint | 否 | 未分配电量 × 100 |
| 13 | `emission_g` | bigint | 否 | 碳排放量，单位克 |
| 14 | `intensity_g_per_kwh` | bigint | 否 | 碳强度；-1 表示不可计算 |
| 15 | `completeness` | bigint | 否 | 完整度 0–100；-1 表示未知 |
| 16 | `factor_version` | string | 是 | 因子版本。源库 DEFAULT ''，空串经 R001 归一为 NULL |
| 17 | `algo_version` | string | 是 | 算法版本。源库 DEFAULT ''，空串经 R001 归一为 NULL |
| 18 | `tariff_mode` | string | 是 | 峰平谷划分模式。源库 DEFAULT ''，空串经 R001 归一为 NULL |
| 19 | `cutoff_time` | string | 是 | 数据截止时刻。按字符串保留；源库 DEFAULT ''，空串经 R001 归一为 NULL |
| 20 | `run_time` | timestamp | 否 | 生成时刻 |
| 21 | `output_path` | string | 是 | 导出文件路径。**未导出的报告本就没有路径**（合理缺失，见质量报告 3.1）；源库 DEFAULT ''，空串经 R001 归一为 NULL |
| 22 | `req_id` | string | 是 | 请求 id，可空 |

</details>

<details><summary><code>dwd_load_forecast</code>　10 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `forecast_id` | bigint | 否 | 源库自增主键 |
| 2 | `station_id` | bigint | 否 | 外键 → t_station |
| 3 | `horizon` | bigint | 否 | 预测时长：1 / 6 / 24 小时 |
| 4 | `predict_time` | timestamp | 否 | 预测目标时刻 |
| 5 | `load_kw` | double | 否 | 预测充电负荷 kW |
| 6 | `idle_pile` | bigint | 否 | 预测空闲桩数 |
| 7 | `is_peak` | bigint | 否 | 0=否 1=是高峰时段 |
| 8 | `congestion` | double | 否 | 拥堵度 0–1 |
| 9 | `model_version` | string | 是 | 模型版本。源库 DEFAULT ''，空串经 R001 归一为 NULL |
| 10 | `create_time` | timestamp | 否 | 生成时刻 |

</details>

<details><summary><code>dwd_wallet_tx</code>　8 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `tx_id` | bigint | 否 | 源库自增主键 |
| 2 | `user_id` | bigint | 否 | 外键 → t_user |
| 3 | `type` | bigint | 否 | 0=充值 1=充电扣费 2=退款 |
| 4 | `amount` | bigint | 否 | 变动金额，单位**分**，正数表示绝对值 |
| 5 | `balance_after` | bigint | 否 | 变动后余额，单位**分** |
| 6 | `order_id` | bigint | 是 | 关联订单；**充值类流水不关联订单（合理缺失）** |
| 7 | `remark` | string | 是 | 备注。源库 NOT NULL DEFAULT ''，空串经 R001 归一为 NULL |
| 8 | `create_time` | timestamp | 否 | 发生时刻 |

</details>

<details><summary><code>dwd_station_review</code>　7 列</summary>

| # | 列 | 类型 | 可空 | 说明 |
| ---: | --- | --- | :---: | --- |
| 1 | `review_id` | bigint | 否 | 源库自增主键 |
| 2 | `station_id` | bigint | 否 | 外键 → t_station |
| 3 | `user_id` | bigint | 否 | 外键 → t_user |
| 4 | `order_id` | bigint | 是 | 关联订单，可空 |
| 5 | `score` | bigint | 否 | 评分 1–5 |
| 6 | `content` | string | 是 | 评价内容。源库 NOT NULL DEFAULT ''，空串经 R001 归一为 NULL |
| 7 | `create_time` | timestamp | 否 | 评价时刻 |

</details>

## 6. 遗留问题与风险

| # | 遗留 | 影响 | 建议 |
| --- | --- | --- | --- |
| 1 | `t_pile_log` 的 1763 条「状态变更」无前后状态（`old_status`、`new_status` 已剔除），且缺少事件类型：故障上报 | 依赖状态流转与故障事件的维度不成立，1800 行中仅 37 条有效事件 | 维度改为「设备事件类型与时序分布」，或放弃该维度另补 |
| 2 | `t_user` 仅 5 个用户、4 个有订单 | 用户维度分析（分层/复购/画像）在数据层面为空 | 已决定暂不扩充，分析维度全部绕开用户粒度 |
| 3 | 数据量不足的表：`t_load_forecast`(18 行)、`t_carbon_report`(5 行)、`t_wallet_tx`(1 行)、`t_station_review`(0 行)、`t_admin_oplog`(3 行) | 相关维度不可做（评价、钱包、审计等） | 报告中说明，不纳入维度设计；数据不足是分析范围问题，不是数据错误 |
| 4 | 数据滞后 10 天 | 合成数据集的固有属性 | 无规则可施；答辩时主动说明数据为 `ml/gen_history.py` 合成 |
| 5 | `invalid_phone` 待核 1 条（规则 R006）：user_id=6、phone=12345678901、nickname=用户8901 | 已标记，保留原值未做删改 | 人工复核后决定；本轮不猜测、不改写 |

## 7. 版本信息与数据血缘

| 项 | 值 |
| --- | --- |
| 源库指纹 | `2eff8f6c1537b4eb` |
| ODS 导出 | 2026-09-14 12:37:20 |
| 探查执行 | 2026-09-15 14:59:18 |
| 清洗执行 | 2026-09-15 15:12:09 |
| 校验执行 | 2026-09-15 15:19:29 |
| 代码版本 | `489758e` |

**血缘链**：`charging.db` → `bigdata/ods/*.csv`（444 只读） → `bigdata/dwd/*.parquet` → （下一步）Spark 分析 → MySQL。

**归档清单**（SOP 第 8 节要求一并归档）

| 类别 | 位置 |
| --- | --- |
| 原始只读快照 | `bigdata/ods/` + `_manifest.json` |
| 规则清单 | `bigdata/quality/03_rules.md` |
| 清洗脚本 | `bigdata/spark/{export_ods,profiling,cleaning,validation,quality_report}.py` |
| 执行日志 | `bigdata/quality/04_clean_log.json` |
| 待核记录 | `bigdata/quality/pending/` |
| 清洗结果 | `bigdata/dwd/` |
| 质量报告 | 本文件 |

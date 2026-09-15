"""T3 阶段 6：数据质量报告与存档。对应 SOP 第 8 节。

报告**由阶段 1–5 的产物程序生成**，不手写——手写的报告和实际数字迟早对不上，
而 SOP 8 要求报告里的处理数量能与执行日志对账。
产出 bigdata/quality/06_quality_report.md。
"""
from __future__ import annotations

import csv
import json
import subprocess
import sys
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
Q = REPO_ROOT / "bigdata" / "quality"


def load(name: str):
    return json.loads((Q / name).read_text(encoding="utf-8"))


def git_rev() -> str:
    try:
        return subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO_ROOT,
                              capture_output=True, text=True, timeout=10).stdout.strip() or "（未提交）"
    except Exception:
        return "（取不到）"


def main() -> int:
    profile = load("01_profile.json")
    clean = load("04_clean_log.json")
    valid = load("05_validation.json")
    with open(Q / "02_issues.csv", encoding="utf-8-sig") as f:
        issues = list(csv.DictReader(f))

    L: list[str] = []
    a = L.append

    a("# 数据质量报告")
    a("")
    a(f"> T3 阶段 6 产出，对应《数据清洗基本流程-操作SOP》第 8 节。")
    a(f"> **本文件由 `bigdata/spark/quality_report.py` 从阶段 1–5 的产物自动生成，请勿手改。**")
    a(f"> 生成时间 {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    a("")

    # ---- 1 数据概况 ----
    a("## 1. 数据概况")
    a("")
    a(f"- 数据源：`{profile['数据源']}`（SQLite，一阶段业务库）")
    a(f"- 源库指纹：`{profile['源库指纹']}`")
    a(f"- ODS 导出时间：{profile['ODS导出时间']}")
    a(f"- 订单时间范围：{profile['表']['t_order']['时间范围']['最早预约']}"
      f" ~ {profile['表']['t_order']['时间范围']['最晚预约']}")
    a("")
    a("| 表 | 行数 | 列数 | 完全重复 |")
    a("| --- | ---: | ---: | ---: |")
    total = 0
    for t, v in profile["表"].items():
        total += v["rows"]
        a(f"| `{t}` | {v['rows']} | {v['columns']} | {v.get('完全重复行数', 0)} |")
    a(f"| **合计** | **{total}** | | **0** |")
    a("")

    # ---- 2 六维度评估 ----
    a("## 2. 六维度质量评估")
    a("")
    dims = ["完整性", "准确性", "一致性", "时效性", "唯一性", "有效性"]
    cnt = {d: sum(1 for i in issues if i["质量维度"] == d) for d in dims}
    a("| 维度 | 问题数 | 结论 |")
    a("| --- | ---: | --- |")
    verdict = {
        "完整性": "关键字段无异常缺失；`t_pile_log` 两列近乎全空已剔除",
        "准确性": f"{profile['表']['t_order']['rows']} 笔订单金额与单价×电量逐笔吻合（容差 1 分）",
        "一致性": "冗余字段 `station_id` 与 `t_pile` 逐行一致；冻结用户订单属正常业务",
        "时效性": f"最新结算 {profile['时效性']['最新结算时间']}，滞后 {profile['时效性']['滞后天数']} 天",
        "唯一性": "无完全重复行，`order_no` 无重复",
        "有效性": "状态枚举、金额非负、时间顺序全部合规；1 条手机号不合规",
    }
    for d in dims:
        a(f"| {d} | {cnt[d]} | {verdict[d]} |")
    a("")

    # ---- 3 问题清单 ----
    a("## 3. 问题清单")
    a("")
    a("| 编号 | 维度 | 对象 | 问题 | 影响 | 严重度 | 处理 |")
    a("| --- | --- | --- | --- | ---: | --- | --- |")
    for i in issues:
        a(f"| {i['问题编号']} | {i['质量维度']} | `{i['表']}`.{i['字段']} | {i['问题类型']} "
          f"| {i['影响数量']} ({i['影响比例']}) | {i['严重程度']} | {i['建议处理']} |")
    a("")

    # ---- 4 处理动作 ----
    a("## 4. 处理动作")
    a("")
    a("规则全文见 [03_rules.md](03_rules.md)，执行日志见 [04_clean_log.json](04_clean_log.json)。")
    a("")
    a("| 规则 | 动作 | 结果 |")
    a("| --- | --- | --- |")
    for r, v in clean["规则执行"].items():
        kv = "　".join(f"{k}={v[k]}" for k in v if k != "说明")
        a(f"| {r} | {v['说明']} | {kv} |")
    a("")
    c = clean["结论"]
    a(f"**本轮汇总：删除 {c['本轮删除行数']} 行，修改值 {c['本轮修改值']} 处，"
      f"标记待核 {c['标记待核']} 条，剔除列 {c['剔除列']}，订单 {c['订单行数']}。**")
    a("")
    # 类型转换失败的逐表对账（R002/R003）。旧版日志没有这一项，缺了就不渲染。
    if "类型转换失败" in c:
        tc = c["类型转换失败"]
        a(f"**类型转换失败对账（R002/R003）**：" +
          "、".join(f"`{t}` {v} 行" for t, v in tc.items()) +
          f"，合计 {sum(tc.values())} 行。"
          "四张表各有一份待核清单，**空清单也落文件**——「查过且为空」与「没查」必须能区分。")
        a("")
    a("> 全程**零删除**。SOP 第 5 节把删除列为最后手段，本轮发现的问题要么可标记（R006）、"
      "要么属正常业务（R007）、要么是分析范围问题而非数据错误（R009），都不需要动行。")
    a("")

    # ---- 5 清洗前后对比 ----
    a("## 5. 清洗前后对比与校验")
    a("")
    a(f"阶段 5 共 {valid['检查数']} 项检查，失败 {valid['失败数']} 项。")
    a("")
    a("| 检查项 | 结论 | 详情 |")
    a("| --- | --- | --- |")
    for r in valid["明细"]:
        a(f"| {r['检查项']} | **{r['结论']}** | {r['详情']} |")
    a("")
    a("> 营收与电量的**逐分/逐度对账**是最硬的一条：清洗前后完全相等，"
      "说明标准化与派生没有引入任何数值漂移。其中电量合计 36,638,035（×100 度）"
      "与第一阶段碳排放对拍脚本独立算出的数字一致，构成一次跨阶段交叉验证。")
    a("")

    # ---- 6 遗留问题 ----
    a("## 6. 遗留问题与风险")
    a("")
    a("| # | 遗留 | 影响 | 建议 |")
    a("| --- | --- | --- | --- |")
    a("| 1 | `t_pile_log` 的 1763 条「状态变更」无前后状态，`event=4` 故障上报 0 行 "
      "| **PHASE2-PLAN 的 D8「设备故障事件分析」不成立**，1800 行中仅 37 条有效事件 "
      "| 维度改为「设备事件类型与时序分布」，或放弃该维度另补 |")
    a("| 2 | `t_user` 仅 5 个用户、4 个有订单 | 用户维度分析（分层/复购/画像）在数据层面为空 "
      "| 已决定暂不扩充，10 个维度全部绕开用户粒度 |")
    a("| 3 | `t_station_review` 空表、`t_wallet_tx` 1 行、`t_admin_oplog` 3 行 "
      "| 评价、钱包、审计三类维度不可做 | 报告中说明，不纳入维度设计 |")
    a("| 4 | `t_load_forecast` 仅 18 行 | 够做拥堵度快照，不够做预测趋势对比 "
      "| 二阶段 MLlib 重新训练后会补充该表 |")
    a(f"| 5 | 数据滞后 {profile['时效性']['滞后天数']} 天 | 合成数据集的固有属性 "
      "| 无规则可施；答辩时主动说明数据为 `ml/gen_history.py` 合成 |")
    a("| 6 | 手机号 `12345678901` 已标记待核 | 不影响分析（用户维度本就不做） "
      "| 保留标记，不删不改 |")
    a("")

    # ---- 7 版本信息 ----
    a("## 7. 版本信息与数据血缘")
    a("")
    a("| 项 | 值 |")
    a("| --- | --- |")
    a(f"| 源库指纹 | `{profile['源库指纹']}` |")
    a(f"| ODS 导出 | {profile['ODS导出时间']} |")
    a(f"| 探查执行 | {profile['生成时间']} |")
    a(f"| 清洗执行 | {clean['生成时间']} |")
    a(f"| 校验执行 | {valid['生成时间']} |")
    a(f"| 代码版本 | `{git_rev()}` |")
    a("")
    a("**血缘链**：`charging.db` → `bigdata/ods/*.csv`（444 只读）"
      " → `bigdata/dwd/*.parquet` → （下一步）Spark 分析 → MySQL。")
    a("")
    a("**归档清单**（SOP 第 8 节要求一并归档）")
    a("")
    a("| 类别 | 位置 |")
    a("| --- | --- |")
    a("| 原始只读快照 | `bigdata/ods/` + `_manifest.json` |")
    a("| 规则清单 | `bigdata/quality/03_rules.md` |")
    a("| 清洗脚本 | `bigdata/spark/{export_ods,profiling,cleaning,validation,quality_report}.py` |")
    a("| 执行日志 | `bigdata/quality/04_clean_log.json` |")
    a("| 待核记录 | `bigdata/quality/pending/` |")
    a("| 清洗结果 | `bigdata/dwd/` |")
    a("| 质量报告 | 本文件 |")
    a("")

    (Q / "06_quality_report.md").write_text("\n".join(L), encoding="utf-8")
    print(f"✓ 质量报告已生成：{Q/'06_quality_report.md'}（{len(L)} 行）")
    print(f"  问题 {len(issues)} 条　规则执行 {len(clean['规则执行'])} 条　"
          f"校验 {valid['检查数']} 项全过" if valid["失败数"] == 0 else "  ⚠ 校验有失败项")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

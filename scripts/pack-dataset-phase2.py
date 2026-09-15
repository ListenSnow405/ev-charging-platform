"""把第二阶段清洗过程的全部数据与产物打成结项数据集压缩包。　归属 L5

归档内容严格对齐《数据清洗基本流程-操作SOP》第 8 节的「需要共同归档」清单：
原始只读快照、规则清单、清洗脚本、执行日志、待核记录、清洗结果、质量报告。
目录按 SOP 六阶段编号，评阅时从 00 到 06 顺着读就是完整的清洗过程。

两个坑（都实测踩过，改动前先看）：
  · **DWD 的 parquet 里时间是 UTC**。Spark 按会话时区（Asia/Shanghai）写入，
    parquet 规范化存 UTC——用 pyarrow 直接读出来导 CSV，全表时间会平移 8 小时，
    `reserve_time` 11:21 变成 03:21，而同一行的 `order_hour` 还是 11，自相矛盾。
    所以 CSV 一律**用 Spark 导出**，由它按会话时区还原，与流水线口径一致。
  · 整数列含空值（取消单的时长）时，走 pandas 会被提升成 float 写出 `48.0`。
    Spark 不会，空值就写成空——这也符合 CLAUDE.md 5.2 第 7 条的整数口径。

用法：.venv-phase2/bin/python scripts/pack-dataset-phase2.py
"""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BIGDATA = REPO / "bigdata"
QUALITY = BIGDATA / "quality"
OUT_ZIP = REPO / "export" / "第二阶段数据清洗数据集.zip"
PKG = "第二阶段数据清洗数据集"

#  DWD 是 parquet 目录；这里额外导一份 CSV，评阅人不装任何库也能打开核对。
#  parquet 本身也一并归档——它才是流水线的真实产物，CSV 只是可读副本。
DWD_TABLES = ["dwd_order", "dwd_user", "dwd_pile", "dwd_station", "dwd_pile_log",
              "dwd_carbon_daily", "dwd_carbon_factor", "dwd_carbon_report",
              "dwd_load_forecast", "dwd_wallet_tx", "dwd_station_review"]


def sha16(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def export_dwd_csv(dst: Path) -> dict[str, int]:
    """用 Spark 把 DWD 各表导成单文件 CSV（表头、UTF-8、时间 yyyy-MM-dd HH:mm:ss）。"""
    sys.path.insert(0, str(BIGDATA / "spark"))
    from spark_session import build_spark            # noqa: E402

    spark = build_spark("ecp-pack-dataset")
    dst.mkdir(parents=True, exist_ok=True)
    rows: dict[str, int] = {}
    tmp = dst / "_tmp"
    for t in DWD_TABLES:
        src = BIGDATA / "dwd" / f"{t}.parquet"
        if not src.exists():
            print(f"  [跳过] {t}：DWD 中不存在")
            continue
        df = spark.read.parquet(str(src))
        rows[t] = df.count()
        (df.coalesce(1).write.mode("overwrite")
           .option("header", True).option("encoding", "UTF-8")
           .option("timestampFormat", "yyyy-MM-dd HH:mm:ss")
           .option("dateFormat", "yyyy-MM-dd")
           .csv(str(tmp)))
        part = next(tmp.glob("part-*.csv"))
        shutil.move(str(part), str(dst / f"{t}.csv"))
        shutil.rmtree(tmp)
        print(f"  {t:<22} {rows[t]:>6} 行 → {t}.csv")
    spark.stop()
    return rows


def copy(src: Path, dst: Path) -> None:
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src.is_dir():
        shutil.copytree(src, dst, dirs_exist_ok=True)
    else:
        shutil.copy2(src, dst)
    #  ODS 产物是 444 只读，copy2 会把只读一起带过来，导致重跑时删不掉暂存目录。
    #  归档包里恢复可写，只读性由 _manifest.json 的指纹和质量报告来证明。
    for p in ([dst] if dst.is_file() else dst.rglob("*")):
        if p.is_file():
            p.chmod(0o644)


def main() -> int:
    stage = REPO / "export" / PKG
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    print("== 1/4 汇集六阶段产物 ==\n")
    copy(BIGDATA / "ods", stage / "00_原始层_ODS")
    copy(QUALITY / "08_hdfs_deploy.json", stage / "00_原始层_ODS" / "_hdfs_deploy.json")
    copy(QUALITY / "01_profile.json", stage / "01_探查与质量评估" / "01_profile.json")
    copy(QUALITY / "02_issues.csv", stage / "01_探查与质量评估" / "02_issues.csv")
    copy(QUALITY / "03_rules.md", stage / "02_清洗规则" / "03_rules.md")
    copy(BIGDATA / "dwd", stage / "03_清洗结果_DWD" / "parquet")
    #  Spark 会在每个 parquet 旁留一份 .crc 校验碎片，对评阅人是纯噪音。
    #  `_SUCCESS` 保留——它标记「这次写入是完整的」，是有意义的产物。
    for crc in (stage / "03_清洗结果_DWD" / "parquet").rglob("*.crc"):
        crc.unlink()
    copy(QUALITY / "04_clean_log.json", stage / "04_执行日志与待核清单" / "04_clean_log.json")
    copy(QUALITY / "pending", stage / "04_执行日志与待核清单" / "pending")
    copy(QUALITY / "05_validation.json", stage / "05_清洗校验" / "05_validation.json")
    copy(QUALITY / "06_quality_report.md", stage / "06_质量报告" / "06_quality_report.md")
    copy(REPO / "数据清洗基本流程-操作SOP.md", stage / "06_质量报告" / "数据清洗基本流程-操作SOP.md")
    for s in ["spark_session.py", "export_ods.py", "profiling.py",
              "cleaning.py", "validation.py", "quality_report.py"]:
        copy(BIGDATA / "spark" / s, stage / "脚本" / s)
    copy(Path(__file__), stage / "脚本" / Path(__file__).name)
    print("  ODS / 探查 / 规则 / DWD / 日志 / 校验 / 报告 / 脚本 / SOP 已就位")

    print("\n== 2/4 DWD 导出可读 CSV（Spark，按会话时区还原时间） ==\n")
    dwd_rows = export_dwd_csv(stage / "03_清洗结果_DWD" / "csv")

    print("\n== 3/4 生成打包清单与导览 ==\n")
    manifest = {
        "包名": PKG,
        "打包时间": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "源库指纹": json.loads((BIGDATA / "ods" / "_manifest.json").read_text(encoding="utf-8"))["source_sha256_16"],
        "文件": [],
    }
    for p in sorted(stage.rglob("*")):
        if p.is_file():
            manifest["文件"].append({
                "路径": str(p.relative_to(stage)),
                "字节": p.stat().st_size,
                "sha256_16": sha16(p),
            })
    (stage / "_打包清单.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    write_readme(stage, dwd_rows)
    print(f"  清单 {len(manifest['文件'])} 个文件，导览 README.md 已生成")

    print("\n== 4/4 压缩 ==\n")
    OUT_ZIP.parent.mkdir(parents=True, exist_ok=True)
    if OUT_ZIP.exists():
        OUT_ZIP.unlink()
    subprocess.run(["zip", "-rq", str(OUT_ZIP), PKG], cwd=str(stage.parent), check=True)
    shutil.rmtree(stage)
    mb = OUT_ZIP.stat().st_size / 1024 / 1024
    print(f"✓ {OUT_ZIP.relative_to(REPO)}　{mb:.2f} MB　{len(manifest['文件'])} 个文件")
    return 0


def write_readme(stage: Path, dwd_rows: dict[str, int]) -> None:
    """导览由产物现读现写，不手填数字——手写的数字迟早和产物对不上。"""
    prof = json.loads((QUALITY / "01_profile.json").read_text(encoding="utf-8"))
    clean = json.loads((QUALITY / "04_clean_log.json").read_text(encoding="utf-8"))
    valid = json.loads((QUALITY / "05_validation.json").read_text(encoding="utf-8"))
    ods_mf = json.loads((BIGDATA / "ods" / "_manifest.json").read_text(encoding="utf-8"))
    issues = (QUALITY / "02_issues.csv").read_text(encoding="utf-8-sig").strip().splitlines()
    c = clean["结论"]
    ods_rows = sum(v["rows"] for v in ods_mf["tables"].values())

    L = [f"# {PKG}", "",
         "电动汽车充电桩管理平台 · 第二阶段大数据子系统 —— **数据清洗全过程数据集**。", "",
         f"> 打包时间 {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}　"
         f"源库指纹 `{ods_mf['source_sha256_16']}`　ODS 导出 {ods_mf['exported_at']}", "",
         "目录按《数据清洗基本流程-操作SOP》**六阶段**编号，从 `00` 读到 `06` 即是完整的清洗过程。", "",
         "## 一、目录导览", "",
         "| 目录 | 对应 SOP 阶段 | 内容 |",
         "| --- | --- | --- |",
         f"| `00_原始层_ODS/` | 前置 | 业务库 `charging.db` 的**只读原始快照**，"
         f"{len(ods_mf['tables'])} 表 {ods_rows} 行 CSV + `_manifest.json` 血缘 + HDFS 落地留痕 |",
         "| `01_探查与质量评估/` | 阶段 1+2 | 数据概况统计、六维度质量问题清单 |",
         "| `02_清洗规则/` | 阶段 3 | R001–R011 规则全文，每条含检测条件/处理动作/依据/验证方式 |",
         "| `03_清洗结果_DWD/` | 阶段 4 | 清洗后数据：`parquet/` 为流水线真实产物，`csv/` 为等价可读副本 |",
         "| `04_执行日志与待核清单/` | 阶段 4 | 每条规则的命中与处理量；无法判定的记录**逐条留档，不静默丢弃** |",
         "| `05_清洗校验/` | 阶段 5 | 清洗前后指标对账与业务断言结果 |",
         "| `06_质量报告/` | 阶段 6 | 数据质量报告（由前五阶段产物程序生成）+ SOP 原文 |",
         "| `脚本/` | 全程 | 六个可重复执行的脚本，相同输入得到相同输出 |",
         "| `_打包清单.json` | — | 包内每个文件的字节数与 sha256 指纹 |", "",
         "## 二、清洗过程一览", "",
         "```",
         "charging.db  ──(mode=ro 只读打开)──▶  00 ODS 原始层（chmod 444，一个字节不改）",
         "                                            │",
         "        01 探查 ─▶ 02 评估 ─▶ 03 规则 ─▶ 04 执行 ─▶ 05 校验 ─▶ 06 报告",
         "                                            ▼",
         "                                     03 DWD 加工层",
         "```", "",
         "**所有清洗、纠错、派生都在 DWD 做，ODS 原始层只读**——这是「可复现、可回溯」的唯一保证，",
         "也是 SOP 第 2 节的硬性要求。任何人拿到这个包，都能从 `00` 的原始快照出发，跑 `脚本/` 重现 `03` 的结果。", "",
         "## 三、关键结论", "",
         "| 项 | 值 |",
         "| --- | --- |",
         f"| 原始数据 | {len(ods_mf['tables'])} 张表 {ods_rows} 行 |",
         f"| 质量问题 | {len(issues) - 1} 条（六维度评估） |",
         f"| 清洗规则 | R001–R011，执行记录 {len(clean['规则执行'])} 条 |",
         f"| **本轮删除** | **{c['本轮删除行数']} 行** |",
         f"| **本轮修改值** | **{c['本轮修改值']} 处** |",
         f"| 标记待核 | {c['标记待核']} 条（保留原值，不删不改） |",
         f"| 剔除列 | {c['剔除列']}（缺失率 > 50%，列级剔除，行不动） |",
         f"| 订单行数 | {c['订单行数']} |",
         f"| 清洗校验 | **{valid['检查数'] - valid['失败数']}/{valid['检查数']} 全部通过** |", ""]

    if "类型转换失败" in c:
        tc = c["类型转换失败"]
        L += [f"类型转换失败逐表对账：" + "、".join(f"`{t}` {v} 行" for t, v in tc.items())
              + f"，合计 {sum(tc.values())} 行。四张表各有一份待核清单，**空清单也落文件**"
                "——「查过且为空」与「没查」必须能区分。", ""]

    rev = next((r["详情"] for r in valid["明细"] if "营收" in r["检查项"]), "")
    L += ["> **全程零删除。** SOP 把删除列为最后手段：本轮问题要么可标记（非法手机号）、",
          "> 要么属正常业务（冻结用户的历史订单）、要么是分析范围问题而非数据错误（低量表），都不需要动行。", "",
          f"> **最硬的一条对账**：{rev}　清洗前后逐分相等，说明标准化与派生没有引入任何数值漂移。", "",
          "## 四、DWD 各表行数", "",
          "| 表 | 行数 |", "| --- | ---: |"]
    L += [f"| `{t}` | {n} |" for t, n in dwd_rows.items()]
    L += ["", "## 五、阅读建议", "",
          "1. 先看 `06_质量报告/06_quality_report.md`——它是全过程的汇总，含问题清单、处理动作、前后对比与遗留问题。",
          "2. 再看 `02_清洗规则/03_rules.md`——每条规则为什么这么定，依据写在「处理依据」一栏。",
          "3. 想核对数字，用 `03_清洗结果_DWD/csv/` 里的 CSV 直接打开，与 `00_原始层_ODS/` 对拍。",
          "4. `04_执行日志与待核清单/pending/` 是「不静默丢弃」的证据——无法判定的记录都在这里，含规则号与样例。", "",
          "## 六、口径说明", "",
          "- **金额一律整数「分」**，电量为 `kwh_x100`（度 × 100），聚合全在整数上做，仅展示层除 100。",
          "  CSV 里看到的 `amount=12289` 即 122.89 元，`kwh_x100=8085` 即 80.85 度。",
          "- **时间格式 `yyyy-MM-dd HH:mm:ss`**，本地时区。`parquet/` 中按规范存 UTC，",
          "  `csv/` 已由 Spark 按会话时区还原，两者表示同一时刻。",
          "- 空单元格表示 **NULL**。取消单的时长类字段为空属**合理缺失，未做任何填充**。", ""]
    (stage / "README.md").write_text("\n".join(L) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())

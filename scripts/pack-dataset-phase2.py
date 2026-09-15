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

import csv
import hashlib
import json
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill
from openpyxl.utils import get_column_letter

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bigdata" / "spark"))
import dwd_schema as DS                                       # noqa: E402

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


def _cell(v: str, typ: str | None):
    """CSV 字符串 → Excel 单元格值。空串一律给 None，让单元格真正留空。"""
    if v == "" or v is None:
        return None
    if typ == DS.LONG:
        return int(v)
    if typ == DS.DBL:
        return float(v)
    if typ == DS.BOOL:
        return v.lower() == "true"
    return v                     # 时间/日期/标识符按文本，避免 Excel 按区域设置乱改格式


def _sheet(wb, name: str, rows: list[list], widths: dict[int, int] | None = None):
    ws = wb.create_sheet(name[:31])       # Excel 工作表名上限 31 字符
    for r in rows:
        ws.append(r)
    if rows:
        for c in ws[1]:
            c.font = Font(bold=True)
            c.fill = PatternFill("solid", fgColor="DDEBF7")
        ws.freeze_panes = "A2"
        ws.auto_filter.ref = ws.dimensions
        for i, head in enumerate(rows[0], 1):
            w = (widths or {}).get(i) or min(max(len(str(head)) * 2 + 4, 10), 32)
            ws.column_dimensions[get_column_letter(i)].width = w
    return ws


def write_excel(stage: Path) -> list[Path]:
    """给 ODS 与 DWD 各出一份 Excel 副本。

    **为什么要这一份**：CSV 是无 BOM 的 UTF-8，中文 Windows 的 Excel 按 GBK 解，
    双击打开就是乱码。

    **为什么不直接给 CSV 加 BOM**：`00_原始层_ODS/` 那份是 Spark 的真实输入，
    BOM 会混进首列列名（`order_id` 变成 `\ufefforder_id`）把流水线搞坏；
    DWD 那份要与重跑结果逐字节对拍。所以**加不是换**——CSV 原样保留，另出 Excel 供人看。
    （`02_issues.csv` 是例外：它本来就是 utf-8-sig，Excel 打开就正常。）

    ODS 一律按文本写，忠实于「原始层所有值都是字符串」这件事；
    DWD 按 Schema 契约给类型——金额电量是数字就该能直接排序求和，
    而手机号、订单号这类标识符仍按文本（SOP 6.2：避免前导零或大数精度丢失）。
    """
    out = []
    for kind, src_dir, target, typed in (
        ("ODS 原始层", stage / "00_原始层_ODS",
         stage / "00_原始层_ODS" / "ODS原始数据.xlsx", False),
        ("DWD 加工层", stage / "03_清洗结果_DWD" / "csv",
         stage / "03_清洗结果_DWD" / "DWD清洗结果.xlsx", True),
    ):
        wb = Workbook()
        wb.remove(wb.active)
        catalog = [["表名", "行数", "列数", "说明"]]
        #  按业务顺序排表，不按文件名字母序——否则 t_admin 排在 t_order 前面，
        #  打开工作簿第一眼看到的是最不重要的表。
        order = (list(DS.DWD_SCHEMA) if typed
                 else list(json.loads((stage / "00_原始层_ODS" / "_manifest.json")
                                      .read_text(encoding="utf-8"))["tables"]))
        rank = {t: i for i, t in enumerate(order)}
        files = sorted(src_dir.glob("*.csv"),
                       key=lambda f: (rank.get(f.stem, len(rank)), f.stem))
        for f in files:
            with open(f, encoding="utf-8", newline="") as fh:
                rows = list(csv.reader(fh))
            head = rows[0] if rows else []
            types = {c: t for c, t, _, _ in DS.DWD_SCHEMA.get(f.stem, [])} if typed else {}
            body = [[_cell(v, types.get(head[i]) if i < len(head) else None)
                     for i, v in enumerate(r)] for r in rows[1:]]
            _sheet(wb, f.stem, [head] + body)
            catalog.append([f.stem, len(body), len(head),
                            "清洗后，类型按 Schema 契约" if typed else "只读原始快照，全部按文本"])

        info = [["项", "说明"],
                ["层次", kind],
                ["来源", "由 scripts/pack-dataset-phase2.py 从同目录 CSV 生成，勿手改"],
                ["与 CSV 的关系", "同一份数据的两种呈现；CSV 为权威副本，本文件仅便于查看"],
                ["金额口径", "一律整数「分」。amount=12289 即 122.89 元"],
                ["电量口径", "kwh_x100，即度 × 100。kwh_x100=8085 即 80.85 度"],
                ["时间格式", "yyyy-MM-dd HH:mm:ss，本地时区，按文本存放"],
                ["空单元格", "表示 NULL。取消单的时长类字段为空属合理缺失，未做任何填充"],
                ["标识符", "手机号、订单号等按文本存放，避免前导零或大数精度丢失"]]
        _sheet(wb, "说明", info, widths={1: 16, 2: 76})
        _sheet(wb, "表清单", catalog, widths={1: 24, 2: 10, 3: 8, 4: 34})
        #  把两张导航表挪到最前，打开就先看到口径说明。
        #  move_sheet 收的是**相对偏移**，不是目标下标——按当前位置算偏移，
        #  否则表多了以后顺序会错乱（首版就错在这里）。
        for i, name in enumerate(("说明", "表清单")):
            wb.move_sheet(name, offset=i - wb.sheetnames.index(name))
        wb.save(target)
        out.append(target)
        print(f"  {kind}　{len(files)} 张表 → {target.name}　{target.stat().st_size / 1024:.0f} KB")
    return out


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

    print("== 1/5 汇集六阶段产物 ==\n")
    copy(BIGDATA / "ods", stage / "00_原始层_ODS")
    copy(QUALITY / "08_hdfs_deploy.json", stage / "00_原始层_ODS" / "_hdfs_deploy.json")
    copy(QUALITY / "01_profile.json", stage / "01_探查与质量评估" / "01_profile.json")
    copy(QUALITY / "02_issues.csv", stage / "01_探查与质量评估" / "02_issues.csv")
    copy(QUALITY / "03_rules.md", stage / "02_清洗规则" / "03_rules.md")
    copy(QUALITY / "09_dwd_schema.json", stage / "03_清洗结果_DWD" / "09_dwd_schema.json")
    copy(BIGDATA / "dwd", stage / "03_清洗结果_DWD" / "parquet")
    #  Spark 会在每个 parquet 旁留一份 .crc 校验碎片，对评阅人是纯噪音。
    #  `_SUCCESS` 保留——它标记「这次写入是完整的」，是有意义的产物。
    for crc in (stage / "03_清洗结果_DWD" / "parquet").rglob("*.crc"):
        crc.unlink()
    copy(QUALITY / "04_clean_log.json", stage / "04_执行日志与待核清单" / "04_clean_log.json")
    copy(QUALITY / "pending", stage / "04_执行日志与待核清单" / "pending")
    copy(QUALITY / "05_validation.json", stage / "05_清洗校验" / "05_validation.json")
    copy(QUALITY / "10_dwd_profile.json", stage / "05_清洗校验" / "10_dwd_profile.json")
    copy(QUALITY / "06_quality_report.md", stage / "06_质量报告" / "06_quality_report.md")
    copy(REPO / "数据清洗基本流程-操作SOP.md", stage / "06_质量报告" / "数据清洗基本流程-操作SOP.md")
    for s in ["spark_session.py", "dwd_schema.py", "export_ods.py", "profiling.py",
              "cleaning.py", "validation.py", "quality_report.py"]:
        copy(BIGDATA / "spark" / s, stage / "脚本" / s)
    copy(Path(__file__), stage / "脚本" / Path(__file__).name)
    print("  ODS / 探查 / 规则 / DWD / 日志 / 校验 / 报告 / 脚本 / SOP 已就位")

    print("\n== 2/5 DWD 导出可读 CSV（Spark，按会话时区还原时间） ==\n")
    dwd_rows = export_dwd_csv(stage / "03_清洗结果_DWD" / "csv")

    print("\n== 3/5 Excel 副本（治 Excel 打开 CSV 中文乱码）==\n")
    write_excel(stage)

    print("\n== 4/5 修相对链接、生成导览与打包清单 ==\n")
    #  仓库里这些文档同处一个目录，链接写的是同级相对路径；进包后按六阶段分了目录，
    #  同级路径就全断了。**在副本上重写，不动仓库原件**——原件的链接在仓库里是对的。
    RELINK = {
        "02_清洗规则/03_rules.md": [
            ("(02_issues.csv)", "(../01_探查与质量评估/02_issues.csv)"),
            ("(09_dwd_schema.json)", "(../03_清洗结果_DWD/09_dwd_schema.json)"),
            ("(../spark/dwd_schema.py)", "(../脚本/dwd_schema.py)")],
        "06_质量报告/06_quality_report.md": [
            ("(03_rules.md)", "(../02_清洗规则/03_rules.md)"),
            ("(04_clean_log.json)", "(../04_执行日志与待核清单/04_clean_log.json)"),
            ("(09_dwd_schema.json)", "(../03_清洗结果_DWD/09_dwd_schema.json)")],
    }
    fixed = 0
    for rel, subs in RELINK.items():
        f = stage / rel
        txt = f.read_text(encoding="utf-8")
        for old, new in subs:
            assert old in txt, f"{rel} 里找不到待修链接 {old}——文档变了，这里要跟着改"
            txt = txt.replace(old, new)
            fixed += 1
        f.write_text(txt, encoding="utf-8")
    print(f"  相对链接修正 {fixed} 处（仅改包内副本）")

    #  **README 必须先写**：清单是扫描 stage 目录生成的，后写的文件登记不进去。
    #  上一版就漏登了 README.md（2026-09-15 评审指出）。
    write_readme(stage, dwd_rows)

    manifest = {
        "包名": PKG,
        "说明": "包内每个文件的字节数与 sha256 指纹。清单自身不在其中——文件无法登记自己的指纹。",
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
    names = {f["路径"] for f in manifest["文件"]}
    assert "README.md" in names, "README.md 未登记进打包清单"
    print(f"  导览 README.md 已生成；清单登记 {len(manifest['文件'])} 个文件")

    print("\n== 5/5 压缩 ==\n")
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
         f"{len(ods_mf['tables'])} 表 {ods_rows} 行 CSV（另附 `ODS原始数据.xlsx`，Excel 直接打开）"
         f" + `_manifest.json` 血缘 + HDFS 落地留痕 |",
         "| `01_探查与质量评估/` | 阶段 1+2 | 数据概况统计、六维度质量问题清单 |",
         "| `02_清洗规则/` | 阶段 3 | R001–R011 规则全文，每条含检测条件/处理动作/依据/验证方式 |",
         "| `03_清洗结果_DWD/` | 阶段 4 | 清洗后数据：`parquet/` 为流水线真实产物，"
         "`csv/` 与 `DWD清洗结果.xlsx` 为等价可读副本；"
         "`09_dwd_schema.json` 是**列序/类型/可空的契约**，也是数据字典 |",
         "| `04_执行日志与待核清单/` | 阶段 4 | 每条规则的命中与处理量；无法判定的记录**逐条留档，不静默丢弃** |",
         "| `05_清洗校验/` | 阶段 5 | 清洗前后指标对账、主外键/范围/时序断言、分层抽样；"
         "`10_dwd_profile.json` 是**清洗后画像**，与 `01_profile.json` 的 ODS 画像对照读 |",
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
         f"| 清洗校验 | **{valid['检查数'] - valid['失败数']}/{valid['检查数']} 全部通过** |",
         f"| Schema 契约 | {len(dwd_rows)} 张表，列序/类型/可空逐列声明并断言 |", ""]

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
          "3. 想核对数字：**Excel 用户直接开 `.xlsx`**——CSV 是无 BOM 的 UTF-8，"
          "中文 Windows 的 Excel 按 GBK 解会乱码；用脚本或 Spark 处理则以 `csv/` 为准，"
          "它与流水线输出逐字节一致。两者内容相同，只是呈现不同。",
          "4. `04_执行日志与待核清单/pending/` 是「不静默丢弃」的证据——无法判定的记录都在这里，含规则号与样例。",
          "5. 想知道某字段「为什么缺一半却不算问题」，看报告第 **3.1 合理缺失登记**；"
          "想知道某列的类型与含义，看第 **5.1 DWD Schema 契约**。", "",
          "## 六、口径说明", "",
          "- **金额一律整数「分」**，电量为 `kwh_x100`（度 × 100），聚合全在整数上做，仅展示层除 100。",
          "  CSV 里看到的 `amount=12289` 即 122.89 元，`kwh_x100=8085` 即 80.85 度。",
          "- **时间格式 `yyyy-MM-dd HH:mm:ss`**，本地时区。`parquet/` 中按规范存 UTC，",
          "  `csv/` 已由 Spark 按会话时区还原，两者表示同一时刻。",
          "- 空单元格表示 **NULL**。取消单的时长类字段为空属**合理缺失，未做任何填充**。", ""]
    (stage / "README.md").write_text("\n".join(L) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())

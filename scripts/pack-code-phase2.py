"""把第二阶段大数据子系统的**源码**打成交付压缩包。　归属 L5

与 [pack-dataset-phase2.py](pack-dataset-phase2.py) 分工明确：那个打的是清洗过程的
**数据**（ODS 快照、DWD 产物、质量报告），这个只打**代码**——两包互不重叠，
按课程要求「代码包不含数据与分工文档」。

选文件的方式是**白名单 + git**：只收 `git ls-files` 里出现过的路径，再按 INCLUDE
逐条匹配。两重保险的理由——
  · 光靠 git：`bigdata/analysis/*.json`、`bigdata/quality/**` 也在版本库里，是数据不是代码；
  · 光靠白名单：本地未入库的临时脚本、`__pycache__`、截图会被 glob 误收。
末尾还有一道 `assert_no_data()` 兜底扫描：包里出现 csv/parquet/png/db 就直接报错退出，
宁可打包失败，也不能把数据混进代码包。

用法：python3 scripts/pack-code-phase2.py
"""
from __future__ import annotations

import hashlib
import json
import py_compile
import shutil
import subprocess
import sys
from datetime import datetime
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PKG = "第二阶段代码"
OUT_ZIP = REPO / "export" / f"{PKG}.zip"

#  白名单按「模块 → 收哪些」写，顺序即 README 里的呈现顺序。
#  glob 相对仓库根，`**` 递归。只写代码与随代码的说明，数据类产物一律不列。
INCLUDE: list[tuple[str, list[str]]] = [
    ("bigdata", ["bigdata/README.md", "bigdata/requirements.txt"]),
    ("bigdata/spark", ["bigdata/spark/*.py"]),
    ("bigdata/mllib", ["bigdata/mllib/*.py"]),
    ("bigdata/api", ["bigdata/api/*.py", "bigdata/api/README.md"]),
    ("bigdata/web", ["bigdata/web/src/**/*", "bigdata/web/index.html",
                     "bigdata/web/vite.config.js", "bigdata/web/package.json",
                     "bigdata/web/package-lock.json", "bigdata/web/.npmrc",
                     "bigdata/web/README.md"]),
    ("scripts", ["scripts/*phase2*", "scripts/hdfs-ctl.sh", "scripts/ods-to-hdfs.sh"]),
    ("config", ["config/phase2.ini.example", "config/phase2-hdfs.env.example"]),
]

#  兜底扫描的黑名单后缀。package-lock.json 是 json 但属依赖锁定，故按后缀而非按类型判。
DATA_SUFFIX = {".csv", ".parquet", ".db", ".xlsx", ".png", ".jpg", ".zip", ".pkl", ".crc"}
DATA_DIR = {"node_modules", "dist", "__pycache__", "analysis", "quality", "ods", "dwd", "models"}

MODULE_DESC = {
    "bigdata": "子系统总说明 `README.md` 与 Python 依赖清单 `requirements.txt`",
    "bigdata/spark": "Spark 清洗与多维分析 job：ODS 导出、探查、清洗、校验、质量报告、分析、运营建议",
    "bigdata/mllib": "Spark MLlib 负荷预测：特征工程、训练、预测、评估报告",
    "bigdata/api": "Flask 只读接口层，只查 MySQL 分析结果，不写业务库",
    "bigdata/web": "Vue3 + DataV 可视化大屏前端源码（不含 node_modules 与 dist 构建产物）",
    "scripts": "第二阶段环境安装、HDFS 落地、冒烟测试与打包脚本",
    "config": "配置模板（`.example`）。真实配置含凭据，永不入库",
}


def sha16(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def tracked() -> set[str]:
    out = subprocess.run(["git", "ls-files"], cwd=REPO, check=True,
                         capture_output=True, text=True).stdout
    return set(out.splitlines())


def collect() -> dict[str, list[Path]]:
    """按白名单挑文件。未入库的路径直接跳过并提示——多半是忘了 git add。"""
    git = tracked()
    #  本脚本自己通常还没入库（首次运行时），单独放行：交付包必须自带打包脚本，
    #  否则评阅人看到的产物无从复现。
    self_rel = str(Path(__file__).resolve().relative_to(REPO))
    picked: dict[str, list[Path]] = {}
    for module, pats in INCLUDE:
        files: set[Path] = set()
        for pat in pats:
            hits = [p for p in REPO.glob(pat) if p.is_file()]
            if not hits:
                print(f"  [警告] {pat} 没匹配到任何文件——白名单该更新了")
            for p in hits:
                rel = str(p.relative_to(REPO))
                if rel not in git and rel != self_rel:
                    print(f"  [跳过] {rel}：未入库")
                    continue
                if p.suffix in DATA_SUFFIX or set(p.relative_to(REPO).parts) & DATA_DIR:
                    continue
                files.add(p)
        picked[module] = sorted(files)
    return picked


def assert_no_data(stage: Path) -> None:
    """兜底：包里不得出现数据类文件或构建产物。命中即失败，不静默放行。"""
    bad = [str(p.relative_to(stage)) for p in stage.rglob("*")
           if p.is_file() and (p.suffix in DATA_SUFFIX
                               or set(p.relative_to(stage).parts[:-1]) & DATA_DIR)]
    assert not bad, "代码包里混进了数据/构建产物：\n  " + "\n  ".join(bad)


def check_py(stage: Path) -> int:
    """逐个 py_compile。只编译不导入，所以不需要装 pyspark 也能验语法。"""
    n = 0
    for p in sorted(stage.rglob("*.py")):
        py_compile.compile(str(p), doraise=True, cfile=str(p) + "c")
        Path(str(p) + "c").unlink()
        n += 1
    return n


def loc(p: Path) -> int:
    try:
        return len(p.read_text(encoding="utf-8").splitlines())
    except UnicodeDecodeError:
        return 0


def main() -> int:
    stage = REPO / "export" / PKG
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    print("== 1/4 按白名单挑选源码（只收 git 已跟踪的路径）==\n")
    picked = collect()
    total = 0
    for module, files in picked.items():
        lines = sum(loc(f) for f in files)
        total += len(files)
        print(f"  {module:<16} {len(files):>3} 个文件　{lines:>5} 行")
        for f in files:
            dst = stage / f.relative_to(REPO)
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(f, dst)
            dst.chmod(0o644)
    #  打包脚本自身进包，与 pack-dataset 的做法一致：产物要能被复现。
    print(f"\n  合计 {total} 个文件")

    print("\n== 2/4 校验 ==\n")
    assert_no_data(stage)
    print("  兜底扫描通过：无 csv/parquet/png/db，无 node_modules/dist/__pycache__")
    print(f"  Python 语法校验通过：{check_py(stage)} 个 .py")

    print("\n== 3/4 生成导览与打包清单 ==\n")
    write_readme(stage, picked)
    manifest = {
        "包名": PKG,
        "说明": "第二阶段大数据子系统源码包。只含代码与随代码的说明，不含数据与分工文档。",
        "打包时间": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "git": subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
                              check=True, capture_output=True, text=True).stdout.strip(),
        "文件": [],
    }
    for p in sorted(stage.rglob("*")):
        if p.is_file():
            manifest["文件"].append({"路径": str(p.relative_to(stage)),
                                     "字节": p.stat().st_size, "sha256_16": sha16(p)})
    (stage / "_打包清单.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    assert "README.md" in {f["路径"] for f in manifest["文件"]}, "README.md 未登记进打包清单"
    print(f"  README.md 已生成；清单登记 {len(manifest['文件'])} 个文件")

    print("\n== 4/4 压缩 ==\n")
    OUT_ZIP.parent.mkdir(parents=True, exist_ok=True)
    if OUT_ZIP.exists():
        OUT_ZIP.unlink()
    subprocess.run(["zip", "-rq", str(OUT_ZIP), PKG], cwd=str(stage.parent), check=True)
    shutil.rmtree(stage)
    kb = OUT_ZIP.stat().st_size / 1024
    print(f"✓ {OUT_ZIP.relative_to(REPO)}　{kb:.0f} KB　{len(manifest['文件'])} 个文件")
    return 0


def write_readme(stage: Path, picked: dict[str, list[Path]]) -> None:
    """导览的文件数与行数现读现算——手填的数字迟早和包对不上。"""
    req = [l.split("#")[0].strip() for l in
           (REPO / "bigdata" / "requirements.txt").read_text(encoding="utf-8").splitlines()]
    deps = "、".join(f"`{d}`" for d in req if d)

    L = [f"# {PKG}", "",
         "电动汽车充电桩管理平台 · **第二阶段大数据与机器学习子系统源码**。", "",
         f"> 打包时间 {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}", "",
         "**本包只含代码**：数据（ODS 原始快照、DWD 清洗结果、分析结果 JSON、模型文件）",
         "与构建产物（`node_modules/`、`dist/`）均不在其中。",
         "清洗全过程的数据另见《第二阶段数据清洗数据集》包。", "",
         "## 一、目录导览", "",
         "| 目录 | 文件 | 行数 | 内容 |",
         "| --- | ---: | ---: | --- |"]
    for module, files in picked.items():
        L.append(f"| `{module}` | {len(files)} | {sum(loc(f) for f in files)} "
                 f"| {MODULE_DESC[module]} |")
    L += ["", "## 二、数据流", "",
          "```",
          "charging.db ──▶ ODS 只读原始层（本地目录 / HDFS）──▶ Spark 清洗 ──▶ DWD 加工层",
          "                                                                      │",
          "                          ┌───────────────────────────────────────────┤",
          "                          ▼                                           ▼",
          "                   Spark 多维分析                              Spark MLlib 负荷预测",
          "                          │                                           │",
          "                          └──────────▶ MySQL 结果表 ◀─────────────────┘",
          "                                            │",
          "                                     Flask 只读接口",
          "                                            │",
          "                                   Vue3 + DataV 大屏",
          "```", "",
          "**分析不直连 `charging.db`**，一律走 ODS 原始层；该层本地是文件系统目录、",
          "答辩时是 HDFS，**代码只认路径不认介质**（`ECP_ODS_ROOT` 环境变量切换）。", "",
          "## 三、技术栈", "",
          "| 项 | 选型 |",
          "| --- | --- |",
          "| Python | 3.11 |",
          "| 文件存储 | Hadoop 3.x / HDFS（ODS 只读原始层） |",
          "| 计算引擎 | PySpark 3.5 |",
          "| 机器学习 | Spark MLlib |",
          "| 结果存储 | MySQL（只存分析结果） |",
          "| Web 层 | Flask（只读接口） |",
          "| 前端 | Node.js / Vue 3 + Vite |",
          "| 大屏 | DataV 开源 Vue3 组件库 + ECharts |", "",
          f"Python 依赖：{deps}（完整清单见 `bigdata/requirements.txt`）。", "",
          "## 四、跑起来", "",
          "```bash",
          "# 0. 环境自检（JDK / Python / PySpark / Node / MySQL）",
          "bash scripts/check-env-phase2.sh", "",
          "# 1. 装环境：顺序必须是 JDK → Python 3.11 → PySpark",
          "bash scripts/install-phase2-env.sh",
          "python3.11 -m venv .venv-phase2",
          ".venv-phase2/bin/pip install -r bigdata/requirements.txt", "",
          "# 2. 配置（凭据不入库，从模板拷一份填真实值）",
          "cp config/phase2.ini.example config/phase2.ini", "",
          "# 3. 六阶段清洗流水线，按顺序跑",
          ".venv-phase2/bin/python bigdata/spark/export_ods.py      # 业务库 → ODS 只读快照",
          ".venv-phase2/bin/python bigdata/spark/profiling.py       # 探查与质量评估",
          ".venv-phase2/bin/python bigdata/spark/cleaning.py        # 按规则清洗 → DWD",
          ".venv-phase2/bin/python bigdata/spark/validation.py      # 清洗校验",
          ".venv-phase2/bin/python bigdata/spark/quality_report.py  # 质量报告", "",
          "# 4. 多维分析与建模",
          ".venv-phase2/bin/python bigdata/spark/analysis.py",
          ".venv-phase2/bin/python bigdata/mllib/train.py",
          ".venv-phase2/bin/python bigdata/mllib/predict.py", "",
          "# 5. 接口与大屏",
          ".venv-phase2/bin/python bigdata/api/app.py               # Flask, :5000",
          "cd bigdata/web && npm install && npm run dev             # 大屏, :5173",
          "```", "",
          "各模块的详细说明见 `bigdata/README.md`、`bigdata/api/README.md`、`bigdata/web/README.md`。", "",
          "## 五、口径与硬性约束", "",
          "1. **ODS 原始层只读**，清洗、纠错、填充一律在 DWD 做，原始快照一个字节不改。",
          "2. **金额一律整数「分」，电量为 `kwh_x100`**，Spark 聚合全在整数上做，仅展示层除 100。",
          "3. **Flask 只读**：只查 MySQL 分析结果表，不写业务库，不现场触发 Spark job。",
          "4. **删除必须留痕**：清洗中的删除、修正、填充都记数量与样例，无法判定的进待核清单。",
          "5. **分析不直连业务库**，一律走 ODS 原始层，本地与 HDFS 之间可平移。", "",
          "> 第一阶段的 Qt 业务平台一行未改，两阶段并存、共用同一份数据源；",
          "> 本包不含第一阶段代码。", ""]
    (stage / "README.md").write_text("\n".join(L) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())

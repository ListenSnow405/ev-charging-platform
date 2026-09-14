"""T2：把 charging.db 导出为 ODS 只读原始层。

**这不是 Spark 作业**，是纯 Python 的一次性落地脚本——源头是 SQLite，
用 Spark 读它反而要额外挂 JDBC 驱动，得不偿失。Spark 从这里产出的 CSV 读起。

三条硬约束，都对应 CLAUDE.md 5.2：
  · 源库**只读打开**（`mode=ro`），导出过程绝不可能写到业务库；
  · 产物落地后 **chmod 444**，把「ODS 只读」从自觉变成文件权限（第 6 条）；
  · 同时写 `_manifest.json` 记录血缘：源库指纹、导出时刻、每表行数与列名。

格式选 CSV 而非 Parquet：ODS 是**原始层**，要的是可直接打开核对、不依赖任何库就能读。
列式压缩是 DWD 加工层的事。数据总量不到 2 万行，CSV 的体积劣势可以忽略。
"""
from __future__ import annotations

import csv
import hashlib
import json
import os
import sqlite3
import stat
import sys
from datetime import datetime
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DB_PATH = REPO_ROOT / "charging.db"
ODS_ROOT = Path(os.environ.get("ECP_ODS_ROOT", REPO_ROOT / "bigdata" / "ods"))

#  导出全部业务表。ODS 是原始层，**不在这里做取舍**——
#  「哪些表数据量不足、不进分析」是 T3 质量评估阶段的结论，不能提前在导出时就砍掉，
#  否则质量报告里「t_station_review 为空表」这个结论就无从谈起。
TABLES = [
    "t_order", "t_pile", "t_station", "t_user", "t_pile_log",
    "t_load_forecast", "t_carbon_daily", "t_carbon_factor", "t_carbon_report",
    "t_wallet_tx", "t_station_review", "t_sys_config", "t_admin", "t_admin_oplog",
]


def db_fingerprint(path: Path) -> str:
    """源库指纹，用于血缘追溯：换了库重跑，指纹会变。"""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def make_writable(p: Path) -> None:
    """重跑时先恢复写权限，否则第二次导出会被自己设的 444 挡住。"""
    if p.exists():
        p.chmod(p.stat().st_mode | stat.S_IWUSR)


def export() -> int:
    if not DB_PATH.exists():
        print(f"[错误] 源库不存在：{DB_PATH}", file=sys.stderr)
        return 1

    ODS_ROOT.mkdir(parents=True, exist_ok=True)
    # 只读打开——这是结构性保证，不是靠自觉。改这行前先想清楚
    con = sqlite3.connect(f"file:{DB_PATH}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row

    manifest = {
        "source": str(DB_PATH.relative_to(REPO_ROOT)),
        "source_sha256_16": db_fingerprint(DB_PATH),
        "exported_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        "ods_root": str(ODS_ROOT),
        "format": "csv (utf-8, header, RFC4180)",
        "tables": {},
    }

    total = 0
    try:
        for table in TABLES:
            cur = con.execute(f"SELECT * FROM {table}")   # 表名来自本文件常量，非外部输入
            cols = [d[0] for d in cur.description]
            out = ODS_ROOT / f"{table}.csv"
            make_writable(out)

            n = 0
            with open(out, "w", newline="", encoding="utf-8") as f:
                w = csv.writer(f)
                w.writerow(cols)
                for row in cur:
                    w.writerow([row[c] for c in cols])
                    n += 1

            out.chmod(0o444)            # ODS 只读：CLAUDE.md 5.2 第 6 条
            manifest["tables"][table] = {"rows": n, "columns": cols,
                                         "file": out.name,
                                         "bytes": out.stat().st_size}
            total += n
            print(f"  {table:<20} {n:>6} 行  → {out.name}")
    finally:
        con.close()

    mf = ODS_ROOT / "_manifest.json"
    make_writable(mf)
    mf.write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    mf.chmod(0o444)

    print(f"\n合计 {total} 行 / {len(TABLES)} 张表 → {ODS_ROOT}")
    print(f"源库指纹 {manifest['source_sha256_16']}　血缘记录 _manifest.json")
    print("产物已置为只读（444）。清洗在 DWD 层做，不要改这里的文件。")
    return 0


if __name__ == "__main__":
    raise SystemExit(export())

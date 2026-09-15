#!/usr/bin/env bash
# =============================================================================
#  scripts/ods-to-hdfs.sh  —  把 ODS 原始层推上 HDFS 并校验　归属 L5
#
#  分工（对应 CLAUDE.md 5.2 第 6 / 10 条）：
#    bigdata/spark/export_ods.py  从 charging.db 产出**本地权威快照**（源库 mode=ro）
#    本脚本                       把该快照原样搬到 HDFS，校验、落只读权限
#  两步分开是有意的：导出脚本是纯 Python 文件 I/O，对 hdfs:// 无效；
#  真要它直写 HDFS 就得引 Python 的 HDFS 客户端，多一个依赖去做 `-put` 已有的事。
#
#  搬运不做任何加工——ODS 是原始层，逐文件 MD5 必须与本地一致。
#
#  只读怎么保证：本地靠 chmod 444（属主也写不进去）。HDFS 上 444 **挡不住超级用户**
#  （启动 NameNode 的那个账号绕过一切权限检查），所以这里把 ODS 交给一个独立的
#  HDFS 身份 ecp_ods，分析侧以 ecp_analyst 连（见 spark_session._pin_hdfs_identity）。
#  脚本末尾会实际试写一次并**期待它失败**，把这条保证验出来而不是声称。
# =============================================================================
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

HADOOP_HOME="${HADOOP_HOME:-$HOME/opt/hadoop}"
export HADOOP_HOME
HDFS="$HADOOP_HOME/bin/hdfs"
LOCAL_ODS="$ROOT/bigdata/ods"
HDFS_ODS="${ECP_HDFS_ODS:-/ecp/ods}"
NN="${ECP_HDFS_NN:-hdfs://localhost:9000}"
ODS_OWNER="ecp_ods"       # ODS 属主：一个纯 HDFS 身份，不需要系统账号
ODS_GROUP="ecp"
ANALYST="ecp_analyst"     # 分析侧身份，对 ODS 只有读权限

FAIL=0
ok()  { printf "  \033[32m[ok]\033[0m   %-22s %s\n" "$1" "$2"; }
bad() { printf "  \033[31m[×]\033[0m    %-22s %s\n" "$1" "$2"; FAIL=$((FAIL+1)); }

[ -x "$HDFS" ] || { bad "Hadoop" "未安装 → bash scripts/install-hadoop-phase2.sh"; exit 1; }
[ -d "$LOCAL_ODS" ] || { bad "本地 ODS" "$LOCAL_ODS 不存在 → 先跑 export_ods.py"; exit 1; }
"$HDFS" dfs -ls / >/dev/null 2>&1 || { bad "NameNode" "连不上 $NN → bash scripts/hdfs-ctl.sh start"; exit 1; }

echo "== ODS → HDFS =="
echo "   源 $LOCAL_ODS"
echo "   目标 $NN$HDFS_ODS"
echo

# ---- 1. 上传（先清后传，保证目标是本地快照的精确副本）------------------------
#  以超级用户身份做搬运；下面第 3 步交出属主后，这个身份就不再用于分析侧。
"$HDFS" dfs -rm -r -skipTrash "$HDFS_ODS" >/dev/null 2>&1
"$HDFS" dfs -mkdir -p "$HDFS_ODS" || { bad "mkdir" "$HDFS_ODS 建失败"; exit 1; }
"$HDFS" dfs -put "$LOCAL_ODS"/* "$HDFS_ODS/" || { bad "put" "上传失败"; exit 1; }
#  期望文件数以 _manifest.json 声明的表数为准（14 张表 + manifest 自身），
#  不数本地目录里有什么——目录里还有 .gitkeep 这类 git 占位符，
#  它不是 ODS 数据，不该上 HDFS，也不该进对账。
N_TABLES=$(python3 -c "import json,sys;print(len(json.load(open(sys.argv[1]))['tables']))" \
           "$LOCAL_ODS/_manifest.json")
N_EXPECT=$((N_TABLES + 1))
N_LOCAL=$(ls -1 "$LOCAL_ODS" | wc -l)          # 与上传用的 glob 同口径，不含隐藏文件
N_HDFS=$("$HDFS" dfs -ls "$HDFS_ODS" | grep -c '^-')
if [ "$N_EXPECT" -eq "$N_HDFS" ] && [ "$N_LOCAL" -eq "$N_HDFS" ]; then
    ok "文件数" "manifest 声明 $N_TABLES 表 + 1 = $N_EXPECT，HDFS 实有 $N_HDFS"
else
    bad "文件数" "期望 $N_EXPECT（$N_TABLES 表+manifest）／本地 $N_LOCAL／HDFS $N_HDFS"
fi

# ---- 2. 逐文件 MD5 比对 -----------------------------------------------------
#  不比 HDFS 自带的 checksum：它算的是块级 CRC，与本地文件的 md5 不可直接比较。
#  这里 -cat 回来在本地算，比的是**字节内容本身**，两千行的量完全跑得起。
MISMATCH=0
N_CHECKED=0
MD5LOG=$(mktemp)
for f in "$LOCAL_ODS"/*; do
    name=$(basename "$f")
    m1=$(md5sum < "$f" | cut -d' ' -f1)
    m2=$("$HDFS" dfs -cat "$HDFS_ODS/$name" 2>/dev/null | md5sum | cut -d' ' -f1)
    [ "$m1" = "$m2" ] || { MISMATCH=$((MISMATCH+1)); echo "      内容不一致：$name"; }
    printf '%s\t%s\t%s\n' "$name" "$m1" "$m2" >> "$MD5LOG"
    N_CHECKED=$((N_CHECKED + 1))
done
[ "$MISMATCH" -eq 0 ] && ok "逐文件 MD5" "$N_CHECKED 个文件字节级一致" \
                      || bad "逐文件 MD5" "$N_CHECKED 个中 $MISMATCH 个与本地不一致"

# ---- 3. 落只读权限 ----------------------------------------------------------
#  目录 555 而非 444：少了 x 位就无法遍历，连 ls 都做不到。
"$HDFS" dfs -chown -R "$ODS_OWNER:$ODS_GROUP" "$HDFS_ODS"
"$HDFS" dfs -chmod 555 "$HDFS_ODS"
"$HDFS" dfs -chmod 444 "$HDFS_ODS"/*
PERM=$("$HDFS" dfs -ls "$HDFS_ODS" | awk '$1 ~ /^-/ {print $1}' | sort -u)
[ "$PERM" = "-r--r--r--" ] && ok "只读权限" "全部文件 444，属主 $ODS_OWNER:$ODS_GROUP" \
                           || bad "只读权限" "实际为 $PERM"

# ---- 4. 把「只读」验出来，而不是声称 -----------------------------------------
#  以分析侧身份试写一次。**期待它失败**——成功才是问题。
WRITE_OUT=$(HADOOP_USER_NAME="$ANALYST" "$HDFS" dfs -touchz "$HDFS_ODS/_should_fail" 2>&1)
if echo "$WRITE_OUT" | grep -q "AccessControlException\|Permission denied"; then
    ok "只读实测" "$ANALYST 写入被 NameNode 拒绝（符合预期）"
else
    bad "只读实测" "$ANALYST 竟然写进去了 —— 只读保证不成立"
    HADOOP_USER_NAME="$ANALYST" "$HDFS" dfs -rm -skipTrash "$HDFS_ODS/_should_fail" >/dev/null 2>&1
fi
#  读则必须通得过
HADOOP_USER_NAME="$ANALYST" "$HDFS" dfs -cat "$HDFS_ODS/_manifest.json" >/dev/null 2>&1 \
    && ok "只读实测" "$ANALYST 可正常读取" \
    || bad "只读实测" "$ANALYST 读不到，权限收得过紧"

# ---- 5. 落一份环境变量，供下游 job 取用 --------------------------------------
cat > config/phase2-hdfs.env <<EOF
#  第二阶段 ODS 数据源指向 HDFS。由 scripts/ods-to-hdfs.sh 生成，勿手改。
#  用法：set -a; . config/phase2-hdfs.env; set +a
#  改回本地目录只需 unset ECP_ODS_ROOT——代码只认路径不认介质（CLAUDE.md 5.2 第 10 条）。
export ECP_ODS_ROOT=$NN$HDFS_ODS
export ECP_HDFS_USER=$ANALYST
export HADOOP_HOME=$HADOOP_HOME
EOF
ok "环境变量" "config/phase2-hdfs.env 已生成"

# ---- 6. 部署留痕 ------------------------------------------------------------
#  ODS 只读，记录不能塞进 ODS 自己，所以落到质量产物目录，与 01~07 并列。
#  这是验收表第 2 条（Hadoop 存储）的证据文件。
python3 - "$MD5LOG" "$NN$HDFS_ODS" "$N_CHECKED" "$MISMATCH" "$ODS_OWNER:$ODS_GROUP" <<'PYEOF'
import json, sys, datetime, pathlib
md5log, uri, n, mismatch, owner = sys.argv[1:6]
files = []
for line in pathlib.Path(md5log).read_text().splitlines():
    name, local_md5, hdfs_md5 = line.split("\t")
    files.append({"文件": name, "本地MD5": local_md5, "HDFS_MD5": hdfs_md5,
                  "一致": local_md5 == hdfs_md5})
out = pathlib.Path("bigdata/quality/08_hdfs_deploy.json")
out.write_text(json.dumps({
    "部署时间": datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    "ODS_URI": uri,
    "文件数": int(n),
    "内容不一致数": int(mismatch),
    "属主": owner,
    "权限": "目录 555 / 文件 444",
    "校验方式": "逐文件 MD5（hdfs dfs -cat 回本地计算，比字节内容而非块级 CRC）",
    "分析侧身份": "ecp_analyst（非超级用户，写入应被 NameNode 拒绝）",
    "文件清单": files,
}, ensure_ascii=False, indent=2), encoding="utf-8")
print(f"  \033[32m[ok]\033[0m   {'部署留痕':<20} {out}")
PYEOF
rm -f "$MD5LOG"

echo
"$HDFS" dfs -ls "$HDFS_ODS" | head -4
echo "  ..."
if [ "$FAIL" -eq 0 ]; then
    echo
    echo "ODS 已在 HDFS 上且只读。下游切数据源："
    echo "  set -a; . config/phase2-hdfs.env; set +a"
    echo "  .venv-phase2/bin/python bigdata/spark/profiling.py"
else
    echo; echo "结论：$FAIL 项未通过"
fi
exit "$FAIL"

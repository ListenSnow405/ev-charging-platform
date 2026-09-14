#!/usr/bin/env bash
# =============================================================================
#  scripts/init-mysql-phase2.sh  —  建第二阶段的 MySQL 库与专用账号　归属 L5
#
#  需要 sudo：Ubuntu 的 MySQL 8 中 root 走 auth_socket，只有 root 身份能建库。
#  用法：bash scripts/init-mysql-phase2.sh [密码]
#  不传密码则随机生成。凭据写入 config/phase2.ini（已被 .gitignore 挡下）。
# =============================================================================
set -euo pipefail
cd "$(dirname "$0")/.."

DB=ecp_bigdata
USER=ecp
PASS="${1:-$(head -c 18 /dev/urandom | base64 | tr -d '/+=' | head -c 20)}"

echo "==> 需要 sudo 以 root 身份连接 MySQL"
sudo mysql <<SQL
CREATE DATABASE IF NOT EXISTS \`$DB\`
  DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
CREATE USER IF NOT EXISTS '$USER'@'localhost' IDENTIFIED BY '$PASS';
ALTER USER '$USER'@'localhost' IDENTIFIED BY '$PASS';
GRANT ALL PRIVILEGES ON \`$DB\`.* TO '$USER'@'localhost';
FLUSH PRIVILEGES;
SQL

cat > config/phase2.ini <<INI
; 由 scripts/init-mysql-phase2.sh 于 $(date '+%Y-%m-%d %H:%M:%S') 生成
; 本文件含凭据，已被 .gitignore 挡下，禁止提交
[mysql]
host = 127.0.0.1
port = 3306
database = $DB
user = $USER
password = $PASS
INI
chmod 600 config/phase2.ini

echo
echo "✓ 库 $DB 与账号 $USER 已就绪，凭据写入 config/phase2.ini（权限 600）"
mysql -u "$USER" -p"$PASS" -e "SELECT '连接验证通过' AS 结果;" "$DB"

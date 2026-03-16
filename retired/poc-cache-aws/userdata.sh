#!/bin/bash
set -eux

REGION="${region}"
EFS_ID="${efs_id}"
MOUNT_POINT="${mount_point}"
REPO_URL="${repo_url}"
REDIS_ENDPOINT="${redis_endpoint}"

apt-get update
apt-get install -y git python3 python3-pip nfs-common amazon-efs-utils

mkdir -p "$MOUNT_POINT"

# Use EFS mount helper (TLS by default when 'tls' option is present)
echo "$EFS_ID:/ $MOUNT_POINT efs _netdev,tls 0 0" >> /etc/fstab
mount -a

# Clone the PoC
cd /opt
if [ ! -d /opt/poc-redis-cache ]; then
  git clone "$REPO_URL"
fi
chown -R ubuntu:ubuntu /opt/poc-redis-cache || true

# Prepare a shared cache dir on EFS
mkdir -p "$MOUNT_POINT/poc-cache"

# Export env vars for all users/sessions
cat >/etc/profile.d/redis_env.sh <<EOF
export REDIS_ENDPOINT="$REDIS_ENDPOINT"
export SHARED_CACHE_DIR="$MOUNT_POINT/poc-cache"
EOF
chmod +x /etc/profile.d/redis_env.sh

# Optional: install redis-tools so you can test connectivity easily
apt-get install -y redis-tools || true

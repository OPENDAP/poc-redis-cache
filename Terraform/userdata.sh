#!/bin/bash
set -eux

REGION="${region}"
EFS_ID="${efs_id}"
MOUNT_POINT="${mount_point}"
REPO_URL="${repo_url}"
REDIS_ENDPOINT="${redis_endpoint}"

# Install basic tools (no amazon-efs-utils)
apt-get update
apt-get install -y git python3 python3-pip nfs-common redis-tools || true

# Compute EFS DNS name using *shell* vars, not template vars
EFS_DNS="$EFS_ID.efs.$REGION.amazonaws.com"

# Create mount point
mkdir -p "$MOUNT_POINT"

# Persist NFS mount in fstab and mount it
echo "$EFS_DNS:/ $MOUNT_POINT nfs4 nfsvers=4.1,_netdev 0 0" >> /etc/fstab
mount -a

# Clone the PoC repo if not already present
cd /opt
if [ ! -d /opt/poc-redis-cache ]; then
  git clone "$REPO_URL"
fi
chown -R ubuntu:ubuntu /opt/poc-redis-cache || true

# Prepare shared cache dir on EFS
mkdir -p "$MOUNT_POINT/poc-cache"

# Export env vars for all users/sessions
cat >/etc/profile.d/redis_env.sh <<EOF
export REDIS_ENDPOINT="$REDIS_ENDPOINT"
export SHARED_CACHE_DIR="$MOUNT_POINT/poc-cache"
EOF
chmod +x /etc/profile.d/redis_env.sh

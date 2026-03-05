#!/bin/bash
set -eux

REGION="${region}"
EFS_ID="${efs_id}"
MOUNT_POINT="${mount_point}"
REPO_URL="${repo_url}"
REDIS_ENDPOINT="${redis_endpoint}"

# Install basic tools (no amazon-efs-utils)
apt-get update
apt-get install -y git python3 python3-pip nfs-common redis-tools

# Compute EFS DNS name using *shell* vars, not template vars
EFS_DNS="$EFS_ID.efs.$REGION.amazonaws.com"

# Create mount point
mkdir -p "$MOUNT_POINT"

# Persist NFS mount in fstab and mount it (no spaces after commas!)
echo "$EFS_DNS:/ $MOUNT_POINT nfs4 nfsvers=4.1,_netdev,x-systemd.automount,x-systemd.mount-timeout=30s,timeo=600,retrans=2 0 0" >> /etc/fstab
# Reload systemd units
systemctl daemon-reload || true
# Retry mount in case DNS/network/EFS mount targets are not ready on boot
for i in $(seq 1 60); do
  if mountpoint -q "$MOUNT_POINT"; then
    echo "EFS mounted at $MOUNT_POINT"
    break
  fi
  echo "Attempt $i: mounting $EFS_DNS:/ to $MOUNT_POINT"
  # Force DNS check implicitly by touching the hostname
  if ! mount -t nfs4 -o nfsvers=4.1,_netdev "$EFS_DNS:/" "$MOUNT_POINT" 2>&1 | tee -a /var/log/efs-mount.log; then
    echo "Mount failed (attempt $i). resolv.conf:" | tee -a /var/log/efs-mount.log
    cat /etc/resolv.conf | tee -a /var/log/efs-mount.log
  fi
  sleep 2
done
if ! mountpoint -q "$MOUNT_POINT"; then
  echo "WARNING: EFS did not mount after retries; continuing anyway" >&2
  tail -n 50 /var/log/efs-mount.log || true
fi

# Clone the PoC repo if not already present
cd /opt
if [ ! -d /opt/poc-redis-cache ]; then
  git clone "$REPO_URL"
fi
chown -R ubuntu:ubuntu /opt/poc-redis-cache || true

# Prepare shared cache dir on EFS
mkdir -p "$MOUNT_POINT/poc-cache"
chown -R ubuntu:ubuntu "$MOUNT_POINT/poc-cache" || true

# Export env vars for all users/sessions
cat >/etc/profile.d/redis_env.sh <<EOF
export REDIS_ENDPOINT="$REDIS_ENDPOINT"
export SHARED_CACHE_DIR="$MOUNT_POINT/poc-cache"
EOF
chmod +x /etc/profile.d/redis_env.sh

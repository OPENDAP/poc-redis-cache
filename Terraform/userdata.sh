#!/bin/bash
set -uox pipefail
export DEBIAN_FRONTEND=noninteractive

# Log everything (even early failures)
exec > >(tee -a /var/log/userdata.log) 2>&1

REGION="${region}"
EFS_ID="${efs_id}"
MOUNT_POINT="${mount_point}"
REPO_URL="${repo_url}"
REDIS_ENDPOINT="${redis_endpoint}"

EFS_DNS="$EFS_ID.efs.$REGION.amazonaws.com"

echo "=== userdata start $(date -Is) ==="
echo "REGION=$REGION"
echo "EFS_ID=$EFS_ID"
echo "EFS_DNS=$EFS_DNS"
echo "MOUNT_POINT=$MOUNT_POINT"
echo "REPO_URL=$REPO_URL"
echo "REDIS_ENDPOINT=$REDIS_ENDPOINT"

# Packages (no amazon-efs-utils)
DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y git python3 python3-pip nfs-common 
apt-get install -y build-essential cmake git pkg-config 
apt-get install -y libhiredis-dev libcppunit-dev
apt-get install -y redis-tools redis-server

# Ensure mount point exists
mkdir -p "$MOUNT_POINT"

# Write fstab once (NO automount; just standard mount)
FSTAB_LINE="$EFS_DNS:/ $MOUNT_POINT nfs4 nfsvers=4.1,_netdev,noresvport,timeo=60,retrans=2 0 0"
if ! grep -qF "$EFS_DNS:/" /etc/fstab; then
  echo "$FSTAB_LINE" >> /etc/fstab
fi

# Optional: wait a bit for networking/DNS/EFS targets to be ready
sleep 60

# Keep trying the exact mount command until it works (up to ~1 minutes)
for i in $(seq 1 300); do
  if mountpoint -q "$MOUNT_POINT"; then
    echo "EFS already mounted at $MOUNT_POINT"
    break
  fi

  echo "Attempt $i: mounting $EFS_DNS:/ to $MOUNT_POINT"
  if mount -t nfs4 -o nfsvers=4.1,noresvport,timeo=60,retrans=2 "$EFS_DNS:/" "$MOUNT_POINT"; then
    echo "Mounted OK on attempt $i"
    break
  fi

  echo "Mount failed (attempt $i). resolv.conf:"
  cat /etc/resolv.conf
  sleep 2
done

if ! mountpoint -q "$MOUNT_POINT"; then
  echo "ERROR: EFS never mounted. Giving up."
  # Don't exit nonzero so instance still comes up for inspection
else
  echo "EFS mount confirmed:"
  mount | grep "$MOUNT_POINT"
  df -h | grep "$MOUNT_POINT"
fi

# Clone repo
mkdir -p /opt
cd /opt
if [ ! -d /opt/poc-redis-cache ]; then
  git clone "$REPO_URL" poc-redis-cache
fi
chown -R ubuntu:ubuntu /opt/poc-redis-cache

# Prepare shared cache dir on EFS (will be on EFS if mounted, else local)
mkdir -p "$MOUNT_POINT/poc-cache"
chown -R ubuntu:ubuntu "$MOUNT_POINT/poc-cache"

# Export env vars
cat >/etc/profile.d/redis_env.sh <<EOF
export REDIS_ENDPOINT="$REDIS_ENDPOINT"
export SHARED_CACHE_DIR="$MOUNT_POINT/poc-cache"
EOF
chmod +x /etc/profile.d/redis_env.sh

# Build the C++ cache and test program
cd /opt/poc-redis-cache/Cpp

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

# Run RedisFileCacheLRU_Simulator
./build/RedisFileCacheLRU_Simulator --duration 300 --redis-endpoint "$REDIS_ENDPOINT" --cache-dir "$MOUNT_POINT/poc-cache" --debug > /opt/simulator.log 2>&1 &

echo "=== userdata end $(date -Is) ==="

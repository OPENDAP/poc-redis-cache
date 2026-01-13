#!/bin/bash
#
# Note: This looks like a regular bash script, but it's actually a terraform template
# file that will build text passed to an EC2 instance as 'user data' Then cloud-init
# on the Ubuntu instance runs it during the instance’s first boot. The placeholders
# like ${region}, ${efs_id} in the block below get their values from main.tf at plan/apply
# time (they are not shell variables).
#
# To debug this, look at log data in /var/log/cloud-init.log and, particularly,
# /var/log/cloud-init-output.log.

set -eux

REGION="${region}"
EFS_ID="${efs_id}"
MOUNT_POINT="${mount_point}"
REPO_URL="${repo_url}"
REDIS_ENDPOINT="${redis_endpoint}"

# Install basic tools (no amazon-efs-utils)
apt-get update
apt-get install -y git python3 python3-pip nfs-common redis-tools || true

EFS_DNS="$EFS_ID.efs.$REGION.amazonaws.com"
mkdir -p "$MOUNT_POINT"

# Make sure systemd-resolved is up (sometimes it's mid-transition during boot)
systemctl is-active --quiet systemd-resolved || systemctl restart systemd-resolved || true

# Wait up to 10 minutes for DNS to resolve the EFS name
# This is, of course, a total hack. We could, instead of mounting using user-data, we could
# create a systemd mount unit for EFS that is explicitly "After=network-online.target systemd-resolved.service"
# that will keep retrying in the background. This loop is probably good enough for the PoC
# code.
DNS_OK=0
for i in $(seq 1 120); do
  if getent ahosts "$EFS_DNS" >/dev/null 2>&1; then
    DNS_OK=1
    echo "DNS ready for $EFS_DNS"
    break
  fi

  # Every 10 tries, print resolver status to the log and kick resolved
  if [ $((i % 10)) -eq 0 ]; then
    echo "Still waiting for DNS ($i/120). Resolver status:"
    resolvectl status || true
    systemctl restart systemd-resolved || true
  fi

  sleep 5
done

if [ "$DNS_OK" -ne 1 ]; then
  echo "ERROR: DNS never resolved $EFS_DNS. Aborting EFS mount."
  resolvectl status || true
  cat /etc/resolv.conf || true
  ip route || true
  exit 1
fi

# Avoid duplicate fstab lines
grep -q "$EFS_DNS:/" /etc/fstab || \
  echo "$EFS_DNS:/ $MOUNT_POINT nfs4 nfsvers=4.1,_netdev,noresvport 0 0" >> /etc/fstab

# Mount with retries (now that DNS resolves)
for i in $(seq 1 24); do
  if mount "$MOUNT_POINT" >/dev/null 2>&1; then
    echo "EFS mounted at $MOUNT_POINT"
    break
  fi
  echo "Mount failed ($i/24), retrying..."
  sleep 5
done

mountpoint -q "$MOUNT_POINT" || { echo "ERROR: EFS still not mounted"; exit 1; }

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

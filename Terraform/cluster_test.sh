#!/usr/bin/env bash

set -u

DURATION=120
PROCESSES=4
NAMESPACE="cluster-test"
MAX_BYTES=1000000
SSH_USER="ubuntu"
SSH_KEY="$HOME/.ssh/<your pem file>" #PROVIDE YOUR SSH KEY HERE
REDIS_ENDPOINT=$(terraform output -raw redis_endpoint)

# Extract the worker ips from terraform
WORKERS=$(terraform output worker_public_ips | grep -Eo '([0-9]{1,3}\.){3}[0-9]{1,3}')

printf "\n------ Starting cluster test in \"$NAMESPACE\" - Worker Duration: $DURATION - Endpoint: $REDIS_ENDPOINT -----\n"

i=0
for WORKER in $WORKERS; do
  printf "Starting worker $i: $WORKER\n"

  # ssh into each worker and run the RedisFileCacheLRU_Simulator
  ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$SSH_USER@$WORKER" "bash -s" <<EOF &

source /etc/profile.d/redis_env.sh
cd /opt/poc-redis-cache/Cpp
mkdir -p /home/ubuntu/cache-test-logs
timeout 180 ./build/RedisFileCacheLRU_Simulator \
    --duration $DURATION \
    --processes $PROCESSES \
    --cache-dir "\$SHARED_CACHE_DIR" \
    --redis-host "\$REDIS_ENDPOINT" \
    --namespace "$NAMESPACE" \
    --blocking \
    --max-bytes $MAX_BYTES \
    > /home/ubuntu/cache-test-logs/simulator.log 2>&1

STATUS=\$?
echo "Simulator exit status: \$STATUS" >> /home/ubuntu/cache-test-logs/simulator.log

EOF
  ((i++))
done

printf "RedisFileCache started on all workers, now we wait for them to complete......\n"
wait
printf "\n----- All workers have finished - cluster test complete -----\n"
printf "\nHere are the PID Results from each Worker:\n"
# ssh back into each worker and get the PID results from the test log
i=0
for WORKER in $WORKERS; do
  printf "    Worker $i: $WORKER"

  ssh -i "$SSH_KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null "$SSH_USER@$WORKER" \
    "grep 'PID ' /home/ubuntu/cache-test-logs/simulator.log | sed 's/^.*PID /PID /' || printf 'No PID results found'"

  ((i++))
done

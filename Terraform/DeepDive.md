# Terraform Deep Dive

## Overview

The `Terraform/` directory provisions a small AWS testbed for the Redis cache proof of concept. Its purpose is to create a distributed environment where multiple EC2 workers can share a filesystem and coordinate through Redis, closely matching the assumptions baked into the C++ cache code.

At a high level the stack creates:

- a VPC with two public subnets
- Internet access via an Internet Gateway and public route table
- security groups for workers, EFS, and Redis
- an encrypted EFS filesystem with mount targets in both subnets
- Redis, either as ElastiCache or a tiny EC2 instance
- worker EC2 instances that mount EFS, clone the repo, build the C++ code, and launch the simulator

## Key Files

- `main.tf`: all infrastructure resources
- `variables.tf`: input variables and defaults
- `outputs.tf`: useful outputs after apply
- `userdata.sh`: bootstrap script executed on worker instances
- `terraform.tfvars.example`: example overrides
- `README.md`: quick-start and usage notes

There are also checked-in plan/state artifacts:

- `plan.txt`, `plan2.txt`, `plan3.txt`
- `terraform.tfstate`, `terraform.tfstate.backup`

Those files look like local working artifacts, not source of truth. They should be treated carefully if this repo is used collaboratively.

## Resource Topology

### Networking

`main.tf` creates a simple public test network:

- one VPC using `var.vpc_cidr`
- one Internet Gateway
- two public subnets, one per availability zone
- one public route table with `0.0.0.0/0` via the IGW

This is intentionally easy to inspect and SSH into, but it is not production-hardened.

### Security groups

Three security groups separate the major traffic patterns:

- `workers`
  - inbound SSH on port `22` from `0.0.0.0/0`
  - all outbound traffic allowed
- `efs`
  - inbound NFS on port `2049` from the workers security group only
  - all outbound traffic allowed
- `redis`
  - inbound SSH on port `22` from `0.0.0.0/0`
  - inbound Redis on port `6379` from the workers security group only
  - all outbound traffic allowed

This means:

- workers can talk to EFS and Redis
- the Redis EC2 host can be reached over SSH for debugging in `ec2` mode
- Redis is not directly open to the public internet on port `6379`
- worker SSH is currently wide open unless you restrict it with your own changes

### Shared storage

The shared filesystem is an encrypted EFS instance:

- `aws_efs_file_system.this`
- `aws_efs_mount_target.mt` in each public subnet

The worker bootstrap mounts EFS at `/mnt/shared` and uses `/mnt/shared/poc-cache` as the shared cache directory.

### Redis modes

The stack supports two Redis backends controlled by `var.redis_mode`.

#### `elasticache`

This is the default and recommended mode.

Resources:

- `aws_elasticache_subnet_group.redis`
- `aws_elasticache_parameter_group.redis`
- `aws_elasticache_replication_group.redis`

Current configuration:

- engine: Redis `7.1`
- node type: `cache.t4g.small`
- single replication group
- no automatic failover
- encryption at rest enabled
- transit encryption enabled

#### `ec2`

This creates one Ubuntu instance and starts Redis in Docker:

- `aws_instance.redis`

Bootstrap behavior:

- `apt-get install -y docker.io`
- enable/start Docker
- `docker run --name redis-server -d -p 6379:6379 redis`

This mode is cheaper and simpler, but much less managed.

When `redis_mode = "ec2"`, the Terraform code now treats the Redis node as an
internal VPC service for worker-to-worker traffic:

- workers connect to `aws_instance.redis[0].private_ip`
- operators can still SSH to the Redis host using `aws_instance.redis[0].public_ip`

That distinction matters. The workers and the Redis EC2 instance all live in the same
VPC, so worker traffic should stay on the private network. Using the Redis instance's
public IP from the workers can fail or time out even though both instances have public
addresses.

### Worker nodes

`aws_instance.worker` launches `var.worker_count` Ubuntu 22.04 instances.

Each worker:

- gets a public IP
- uses the workers security group
- optionally attaches the configured EC2 key pair
- receives rendered `userdata.sh`
- waits for EFS mount targets before starting
- receives the Redis endpoint as:
  - ElastiCache primary endpoint in `elasticache` mode
  - Redis EC2 private IP in `ec2` mode

The AMI lookup is dynamic and uses Canonical’s Ubuntu 22.04 images.

## Bootstrap Flow (`userdata.sh`)

The worker bootstrap script is the most important operational part of the Terraform side because it bridges infra to the C++ application.

### What the script does

1. Logs all output to `/var/log/userdata.log`
2. Installs system packages:
   - Git
   - Python 3 and pip
   - `nfs-common`
   - build tools and CMake
   - `libhiredis-dev`
   - `libcppunit-dev`
   - `redis-tools`
3. Creates the EFS mount point
4. Adds an `/etc/fstab` entry for the EFS filesystem
5. Waits 60 seconds, then retries the EFS mount up to 300 times
6. Clones the repo into `/opt/poc-redis-cache`
7. Creates the shared cache directory at `/mnt/shared/poc-cache`
8. Exports environment variables for login shells:
   - `REDIS_ENDPOINT`
   - `SHARED_CACHE_DIR`
9. If using EC2-backed Redis, polls `redis-cli ... ping` until the Redis node is reachable
10. Builds the C++ code in `/opt/poc-redis-cache`
11. Launches `RedisFileCacheLRU_Simulator` in the background and redirects output to `/opt/simulator.log`

### Why this matches the C++ design

The C++ cache requires:

- a shared directory visible to multiple hosts
- a single Redis endpoint reachable by all workers

The Terraform stack provides exactly those two primitives:

- EFS for shared files
- Redis for distributed lock and index state

For the EC2-backed Redis case, "reachable by all workers" specifically means
"reachable on the Redis instance's private IP inside the VPC." That is now what
`REDIS_ENDPOINT` contains for worker startup.

## Variables

The current inputs are intentionally small:

| Variable | Default | Meaning |
| --- | --- | --- |
| `region` | `us-west-2` | AWS region |
| `project` | `poc-redis-cache` | Name prefix for resources |
| `redis_mode` | `elasticache` | Redis backend: `elasticache` or `ec2` |
| `instance_type` | `t3.small` | Worker instance type |
| `key_name` | `null` | Optional EC2 key pair name for SSH |
| `vpc_cidr` | `10.42.0.0/16` | VPC CIDR block |
| `worker_count` | `2` | Number of worker instances |

`terraform.tfvars.example` provides a small example override set and flips `redis_mode` to `ec2`.

## Outputs

`outputs.tf` publishes the values most useful for access and debugging:

- `worker_public_ips`
- `efs_id`
- `efs_dns`
- `redis_endpoint`
- `redis_public_ip`

These are enough to:

- SSH to workers
- verify EFS addressing
- point workers to Redis using the internal address
- SSH to the Redis EC2 host in `ec2` mode

## End-to-End Runtime Model

Putting the Terraform and C++ halves together:

1. Terraform provisions infra.
2. Workers boot and mount the same EFS volume.
3. Workers clone the repo and build the C++ simulator.
4. Each worker launches the simulator against the shared Redis endpoint.
5. All simulator processes coordinate through Redis while reading/writing files on shared EFS.

That makes this directory less of a generic infrastructure module and more of an application-specific reproducibility harness for the cache PoC.

## Current Caveats and Code/Infra Drift

A few details are worth calling out because they affect whether the provisioned environment behaves exactly as intended.

### Shared-run startup behavior

`userdata.sh` now launches the simulator with:

```bash
./build/Cpp/RedisFileCacheLRU_Simulator ... --redis-host "$REDIS_ENDPOINT" --redis-port 6379
```

That matches the current simulator CLI. For shared multi-node runs, the simulator should not be started with `--clean-start` on every node; instead, rely on a fresh Terraform deployment and destroy the stack after the run.

In `ec2` Redis mode, the worker bootstrap now waits for Redis to answer a `PING`
before launching the simulator. This avoids a boot-order race where workers come
up before the Redis Docker container is listening and then fail early with
connection timeouts.

### Private vs. public Redis addressing

This is the key operational rule for the EC2-backed Redis deployment:

- workers must use the Redis instance's private IP address
- humans use the Redis instance's public IP address for SSH

Terraform now reflects that split directly:

- `redis_endpoint` is the private IP in `ec2` mode
- `redis_public_ip` is exported separately for operator access

If a worker is configured to use the Redis host's public IP instead, connection
attempts may time out even though the Redis instance itself is healthy.

### ElastiCache transit encryption mismatch

The ElastiCache replication group enables `transit_encryption_enabled = true`, but the current C++ code connects with plain hiredis TCP and does not show TLS configuration. That suggests the current C++ client may not be able to talk to the managed Redis endpoint in `elasticache` mode without additional client-side TLS support or a configuration change.

By inference, the `ec2` Redis mode is the path most likely to work with the code exactly as it exists today.

### Package overlap on workers

The worker bootstrap now installs `redis-tools` but not `redis-server`, which better matches the intended architecture where workers use the external Redis endpoint rather than a local Redis instance.

### Checked-in state

`terraform.tfstate` and `terraform.tfstate.backup` are present in the repo. Keeping local state in version control is risky for team workflows and can accidentally expose infrastructure details.

## Security Posture

This stack is clearly optimized for ease of testing, not hardening.

Strengths:

- Redis is reachable only from worker instances
- EFS is reachable only from worker instances
- EFS is encrypted at rest
- ElastiCache mode enables encryption settings

Tradeoffs:

- workers have public IPs
- SSH is open to the world
- there are no private subnets, NAT gateways, or SSM-only access patterns
- there is no IAM role design for instances

The README correctly positions this as a simple testbed and sketches production-ish improvements.

## How to Read This Directory

If you want to understand it quickly, this order works best:

1. `variables.tf`
2. `main.tf`
3. `userdata.sh`
4. `outputs.tf`
5. `README.md`

That sequence shows the interface first, then the provisioned resources, then what the instances actually do once they boot.

## Summary

The Terraform directory is a purpose-built AWS harness for the C++ Redis file cache PoC. It provisions exactly the two distributed dependencies the cache needs, shared storage and Redis, and then boots workers that compile and run the simulator automatically. Its main limitations are the intentionally open test-network posture and a small amount of drift between `userdata.sh` and the current simulator/Redis client behavior.

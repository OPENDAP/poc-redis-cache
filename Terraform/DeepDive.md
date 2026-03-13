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
  - inbound Redis on port `6379` from the workers security group only
  - all outbound traffic allowed

This means:

- workers can talk to EFS and Redis
- Redis is not directly open to the public internet
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

### Worker nodes

`aws_instance.worker` launches `var.worker_count` Ubuntu 22.04 instances.

Each worker:

- gets a public IP
- uses the workers security group
- optionally attaches the configured EC2 key pair
- receives rendered `userdata.sh`
- waits for EFS mount targets before starting

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
   - `redis-server`
3. Creates the EFS mount point
4. Adds an `/etc/fstab` entry for the EFS filesystem
5. Waits 60 seconds, then retries the EFS mount up to 300 times
6. Clones the repo into `/opt/poc-redis-cache`
7. Creates the shared cache directory at `/mnt/shared/poc-cache`
8. Exports environment variables for login shells:
   - `REDIS_ENDPOINT`
   - `SHARED_CACHE_DIR`
9. Builds the C++ code in `/opt/poc-redis-cache/Cpp`
10. Launches `RedisFileCacheLRU_Simulator` in the background and redirects output to `/opt/simulator.log`

### Why this matches the C++ design

The C++ cache requires:

- a shared directory visible to multiple hosts
- a single Redis endpoint reachable by all workers

The Terraform stack provides exactly those two primitives:

- EFS for shared files
- Redis for distributed lock and index state

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

These are enough to:

- SSH to workers
- verify EFS addressing
- point clients to Redis

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

### Simulator argument mismatch

`userdata.sh` launches:

```bash
./build/RedisFileCacheLRU_Simulator ... --redis-endpoint "$REDIS_ENDPOINT"
```

But the simulator parses:

```text
--redis-host
--redis-port
--redis-db
```

There is no `--redis-endpoint` option in the current C++ simulator. As written, workers will ignore that flag and fall back to the simulator’s default Redis host unless the binary is changed or the script is corrected.

### ElastiCache transit encryption mismatch

The ElastiCache replication group enables `transit_encryption_enabled = true`, but the current C++ code connects with plain hiredis TCP and does not show TLS configuration. That suggests the current C++ client may not be able to talk to the managed Redis endpoint in `elasticache` mode without additional client-side TLS support or a configuration change.

By inference, the `ec2` Redis mode is the path most likely to work with the code exactly as it exists today.

### Package overlap on workers

The worker bootstrap installs `redis-server` locally even though the README says workers should use the external Redis endpoint rather than a local Redis instance. It is not started explicitly in `userdata.sh`, but the package is unnecessary for the intended architecture.

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

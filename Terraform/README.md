# AWS Testbed for OPeNDAP Redis Cache PoC

This is a minimal Terraform stack that'll spin up a small cluster that mirrors your PoC architecture. This version fixes HCL block formatting to work with Terraform v1.14+.:

- **Two (or more) EC2 workers** that mount a shared **EFS** volume at `/mnt/shared`
- **Redis** for cross-node locks via either:
  - **ElastiCache for Redis** (ElastiCache is default and recommended), or
  - A tiny **EC2** instance running Redis in Docker
- Workers auto-clone your repo: https://github.com/OPENDAP/poc-redis-cache

> This is meant to be a simple test with environment—simple public subnets, public IPs, and open SSH (22) from the internet by default. For production-like networking (private subnets, NAT, SSM, etc.), see the Hardening section below.

---

## Prerequisites

- Terraform >= 1.6
- AWS credentials configured (e.g., environment variables or local profile)
- (Optional) An EC2 key pair if you want to SSH into workers

## Quick start

```bash
cd poc-cache-aws

# You can optionally copy and edit the defaults
cp terraform.tfvars.example terraform.tfvars

terraform init

# Default:
terraform apply -auto-approve
# EC2 option:
terraform plan -var="redis_mode=ec2"
terraform apply -auto-approve -var="redis_mode=ec2"

# Show outputs
terraform output
```

Here's what you’ll get:
- `worker_public_ips`: Public IPs of the worker EC2 instances
- `efs_id` and `efs_dns`: EFS identifiers
- `redis_endpoint`: Hostname/IP to use as the Redis server

## Stuff that gets provisioned

- **VPC** with two public subnets (two AZs)
- **Security Groups:**
  - Workers SG (allows SSH from anywhere for testing)
  - EFS SG (allows NFS/2049 *from workers SG only*)
  - Redis SG (allows 6379 *from workers SG only*)
- **EFS** file system + mount targets in both subnets
- **Redis**: either ElastiCache (managed, encrypted) or a tiny EC2 with Dockerized Redis
- **Workers**: Ubuntu 22.04 instances that:
  - mount EFS at `/mnt/shared` using the `amazon-efs-utils` mount helper with TLS
  - create a shared cache dir at `/mnt/shared/poc-cache`
  - clone `OPENDAP/poc-redis-cache` into `/opt/poc-redis-cache`
  - export `REDIS_ENDPOINT` and `SHARED_CACHE_DIR` for all users

## Running the PoC tests

SSH into each worker (this is optional but could be useful):

```bash
ssh -i <yourkey.pem> ubuntu@<WORKER_PUBLIC_IP>
# The environment variables are available in new shells:
echo $REDIS_ENDPOINT
echo $SHARED_CACHE_DIR
```

Your repo is at `/opt/poc-redis-cache`. Adjust commands below to match the PoC’s current test harness (Python/C++/etc.). Example pattern:

```bash
cd /opt/poc-redis-cache

# If there's a Python test loop/script, for example:
# python3 -m venv .venv && source .venv/bin/activate
# pip install -r requirements.txt
# export REDIS_ENDPOINT=...  # already set via profile.d
# export SHARED_CACHE_DIR=/mnt/shared/poc-cache  # already set
# python tests/run_multi_node_demo.py
```

> Important: The PoC originally uses a local Redis Docker container for demos. In this AWS setup, **do not** start a local Redis on the workers; instead, point clients to the `redis_endpoint` output.

## Switching Redis backend

In `terraform.tfvars`:

```hcl
# default (managed)
redis_mode = "elasticache"

# or use an EC2-hosted Redis
redis_mode = "ec2"
```

Re-`apply` and Terraform will provision the chosen backend.

## Cleanup

```bash
terraform destroy
```

This tears everything down (including EFS). If you want to keep artifacts, copy them from `/mnt/shared` first.

## Hardening / Production-ish notes

- Put workers and Redis in **private subnets** (no public IP), use **SSM Session Manager** instead of SSH.
- Restrict SSH ingress to your office IP(s) or remove it entirely.
- Add IAM roles for EC2 (e.g., to pull from CodeCommit/ECR, CloudWatch logs).
- Use **Auto Scaling** for worker fleets; parameterize `user_data` to auto-run test services via systemd.
- For ElastiCache, consider **Multi-AZ** with automatic failover for resilience; tune parameter group if needed.

## Inputs

See `variables.tf` or `terraform.tfvars.example` for all variables.

## Outputs

- `worker_public_ips`
- `efs_id`
- `efs_dns`
- `redis_endpoint`

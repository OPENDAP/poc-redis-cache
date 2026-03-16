
terraform {
  required_version = ">= 1.6"
  required_providers {
    aws = { source = "hashicorp/aws", version = "~> 6.0" }
  }
}

provider "aws" { region = var.region }

# --- VPC (simple public subnets for a test environment) ---
resource "aws_vpc" "this" {
  cidr_block           = var.vpc_cidr
  enable_dns_support   = true
  enable_dns_hostnames = true
  tags = { Name = "${var.project}-vpc" }
}

resource "aws_internet_gateway" "igw" {
  vpc_id = aws_vpc.this.id
  tags   = { Name = "${var.project}-igw" }
}

# Two AZs
data "aws_availability_zones" "azs" { state = "available" }

resource "aws_subnet" "public" {
  count                   = 2
  vpc_id                  = aws_vpc.this.id
  cidr_block              = cidrsubnet(var.vpc_cidr, 8, count.index)
  availability_zone       = data.aws_availability_zones.azs.names[count.index]
  map_public_ip_on_launch = true
  tags = { Name = "${var.project}-public-${count.index}" }
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.this.id
  tags   = { Name = "${var.project}-public-rt" }
}

resource "aws_route" "public_inet" {
  route_table_id         = aws_route_table.public.id
  destination_cidr_block = "0.0.0.0/0"
  gateway_id             = aws_internet_gateway.igw.id
}

resource "aws_route_table_association" "public_assoc" {
  count          = length(aws_subnet.public)
  subnet_id      = aws_subnet.public[count.index].id
  route_table_id = aws_route_table.public.id
}

# --- Security groups ---
# Workers (EC2) SG
resource "aws_security_group" "workers" {
  name        = "${var.project}-workers-sg"
  description = "EC2 workers"
  vpc_id      = aws_vpc.this.id

  # SSH for convenience (optional)
  ingress {
    from_port   = 22
    to_port     = 22
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  # Allow all egress
  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.project}-workers-sg" }
}

# EFS allows NFS (2049) from workers
resource "aws_security_group" "efs" {
  name        = "${var.project}-efs-sg"
  description = "EFS"
  vpc_id      = aws_vpc.this.id

  ingress {
    from_port       = 2049
    to_port         = 2049
    protocol        = "tcp"
    security_groups = [aws_security_group.workers.id]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.project}-efs-sg" }
}

# Redis access SG (used by ElastiCache or Redis EC2)
resource "aws_security_group" "redis" {
  name        = "${var.project}-redis-sg"
  description = "Redis access"
  vpc_id      = aws_vpc.this.id

  # Redis TCP 6379 from workers
  ingress {
    from_port       = 6379
    to_port         = 6379
    protocol        = "tcp"
    security_groups = [aws_security_group.workers.id]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.project}-redis-sg" }
}

# --- EFS (shared disk) ---
resource "aws_efs_file_system" "this" {
  creation_token  = "${var.project}-efs"
  encrypted       = true
  throughput_mode = "bursting"
  tags            = { Name = "${var.project}-efs" }
}

# One mount target per subnet
resource "aws_efs_mount_target" "mt" {
  count           = length(aws_subnet.public)
  file_system_id  = aws_efs_file_system.this.id
  subnet_id       = aws_subnet.public[count.index].id
  security_groups = [aws_security_group.efs.id]
}

# --- AMI for EC2 instances ---
data "aws_ami" "ubuntu" {
  most_recent = true
  owners      = ["099720109477"] # Canonical
  filter {
    name   = "name"
    values = ["ubuntu/images/hvm-ssd/ubuntu-jammy-22.04-amd64-server-*"]
  }
}

# --- (Option A) Managed ElastiCache for Redis (recommended for test) ---
resource "aws_elasticache_subnet_group" "redis" {
  count      = var.redis_mode == "elasticache" ? 1 : 0
  name       = "${var.project}-redis-subnets"
  subnet_ids = aws_subnet.public[*].id
}

resource "aws_elasticache_parameter_group" "redis" {
  count  = var.redis_mode == "elasticache" ? 1 : 0
  name   = "${var.project}-redis-params"
  family = "redis7"
}


resource "aws_elasticache_replication_group" "redis" {
  count                      = var.redis_mode == "elasticache" ? 1 : 0
  replication_group_id       = "${var.project}-rg"
  description                = "PoC Redis for cache locks"
  engine                     = "redis"
  engine_version             = "7.1"
  node_type                  = "cache.t4g.small"
  automatic_failover_enabled = false
  at_rest_encryption_enabled = true
  transit_encryption_enabled = true
  auth_token                 = null
  parameter_group_name       = aws_elasticache_parameter_group.redis[0].name
  subnet_group_name          = aws_elasticache_subnet_group.redis[0].name
  security_group_ids         = [aws_security_group.redis.id]
  port                       = 6379
  apply_immediately          = true
  depends_on                 = [aws_efs_mount_target.mt]
  tags                       = { Name = "${var.project}-elasticache" }
}

# --- (Option B) Tiny EC2 running Redis (if you prefer not to use ElastiCache) ---
resource "aws_instance" "redis" {
  count                       = var.redis_mode == "ec2" ? 1 : 0
  ami                         = data.aws_ami.ubuntu.id
  instance_type               = "t3.micro"
  subnet_id                   = aws_subnet.public[0].id
  vpc_security_group_ids      = [aws_security_group.redis.id]
  associate_public_ip_address = true
  key_name                    = var.key_name
  user_data = <<-EOF
    #!/bin/bash
    set -eux
    apt-get update
    apt-get install -y docker.io
    systemctl enable --now docker
    docker run --name redis-server -d -p 6379:6379 redis
  EOF
  tags = { Name = "${var.project}-redis" }
}

# --- Worker instances (your app/test nodes) ---
resource "aws_instance" "worker" {
  count                       = var.worker_count
  ami                         = data.aws_ami.ubuntu.id
  instance_type               = var.instance_type
  subnet_id                   = aws_subnet.public[count.index % length(aws_subnet.public)].id
  vpc_security_group_ids      = [aws_security_group.workers.id]
  associate_public_ip_address = true
  key_name                    = var.key_name

  user_data = templatefile("${path.module}/userdata.sh", {
    region         = var.region
    efs_id         = aws_efs_file_system.this.id
    mount_point    = "/mnt/shared"
    repo_url       = "https://github.com/OPENDAP/poc-redis-cache.git"
    redis_endpoint = var.redis_mode == "elasticache" ? aws_elasticache_replication_group.redis[0].primary_endpoint_address : aws_instance.redis[0].public_ip
    redis_mode     = var.redis_mode
  })

  tags = { Name = "${var.project}-worker-${count.index}" }
}

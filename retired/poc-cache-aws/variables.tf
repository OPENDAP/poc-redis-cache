
variable "region" {
  type    = string
  default = "us-west-2"
}

variable "project" {
  type    = string
  default = "poc-redis-cache"
}

# Choose redis backend: "elasticache" or "ec2"
variable "redis_mode" {
  type    = string
  default = "elasticache"
}

# EC2 instance details
variable "instance_type" {
  type    = string
  default = "t3.small"
}

variable "key_name" {
  type    = string
  default = null # optional SSH key for testing
}

# VPC CIDR
variable "vpc_cidr" {
  type    = string
  default = "10.42.0.0/16"
}

# Number of workers to launch
variable "worker_count" {
  type    = number
  default = 2
}

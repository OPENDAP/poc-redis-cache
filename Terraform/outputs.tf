
output "worker_public_ips" {
  value = aws_instance.worker[*].public_ip
}

output "efs_id" {
  value = aws_efs_file_system.this.id
}

output "efs_dns" {
  value = "${aws_efs_file_system.this.id}.efs.${var.region}.amazonaws.com"
}

output "redis_endpoint" {
  value = var.redis_mode == "elasticache" ? aws_elasticache_replication_group.redis[0].primary_endpoint_address : aws_instance.redis[0].private_ip
}

output "redis_public_ip" {
  value = var.redis_mode == "ec2" ? aws_instance.redis[0].public_ip : null
}

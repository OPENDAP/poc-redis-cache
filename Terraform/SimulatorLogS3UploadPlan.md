# Plan: Upload `simulator.log` to S3

## Goal

Add an opt-in Terraform-driven feature that uploads each worker's `/opt/simulator.log`
to an S3 bucket after the simulator run finishes, without changing the current C++
simulator behavior.

## Why this belongs in `Terraform/`

Today the active AWS path is:

- `Terraform/main.tf` provisions the workers
- `Terraform/userdata.sh` builds the code and starts the simulator
- the simulator output is redirected to `/opt/simulator.log`

Because the log file is created by the worker bootstrap flow, the smallest and least
risky implementation is to add S3 upload support to the Terraform-managed worker
environment instead of modifying the simulator binary.

## Recommended design

Use an S3 bucket plus an EC2 instance profile. On each worker:

1. The worker writes simulator output to `/opt/simulator.log` as it does now.
2. A small wrapper script runs the simulator in the foreground.
3. When the simulator exits, the wrapper uploads the log to S3 with `aws s3 cp`.
4. The S3 object key includes enough metadata to distinguish workers and runs.

Recommended key shape:

```text
s3://<bucket>/<prefix>/<project>/<timestamp>/worker-<index>-<instance-id>/simulator.log
```

This keeps uploads simple, avoids changing simulator CLI/API, and fits the current
bootstrapping model.

## Why running in the foreground is simpler

The current `userdata.sh` launches the simulator in the background:

```bash
./RedisFileCacheLRU_Simulator ... > /opt/simulator.log 2>&1 &
```

That means `userdata.sh` ends immediately and cannot reliably upload the log "after the
run" unless the simulator launch is moved behind a durable wrapper or service.

If we instead run the simulator in the foreground from a wrapper, the flow becomes much
simpler:

1. run simulator
2. wait for exit naturally
3. upload `/opt/simulator.log`
4. exit with the simulator's status

For this feature, the foreground model is the recommended default because it removes the
need to coordinate a later upload from a detached process.

## Implementation steps

### 1. Add Terraform inputs for S3 log upload

Update `Terraform/variables.tf` with new opt-in variables:

- `simulator_log_upload_enabled` (`bool`, default `false`)
- `simulator_log_bucket_name` (`string`, default `null`)
- `simulator_log_prefix` (`string`, default `"simulator-logs"`)
- `simulator_log_bucket_force_destroy` (`bool`, default `false`) if this stack should
  be allowed to create and later destroy a bucket with objects in it
- optionally `simulator_log_create_bucket` (`bool`, default `false`) if you want the
  stack to support either reusing an existing bucket or creating one

Notes:

- Keep the feature disabled by default to preserve current behavior.
- If the repo should only upload to an existing bucket, skip the create-bucket option.

### 2. Add S3 resources and IAM permissions

Update `Terraform/main.tf` to add the AWS pieces needed by the workers:

- an S3 bucket resource if bucket creation is managed by Terraform
- a bucket policy or public access block only if needed
- an IAM role for worker EC2 instances
- an IAM instance profile attached to each worker
- an IAM policy granting only:
  - `s3:PutObject`
  - `s3:AbortMultipartUpload`
  - optionally `s3:ListBucket` if the upload flow needs it

Scope permissions narrowly to the chosen bucket and prefix.

Recommended policy target:

- bucket ARN for list access if used
- object ARN like `arn:aws:s3:::<bucket>/<prefix>/*` for uploads

### 3. Attach the instance profile to workers

Update the existing `aws_instance.worker` resource in `Terraform/main.tf`:

- add `iam_instance_profile`
- pass the new S3-related template variables into `templatefile(...)`

This keeps credentials off disk and avoids embedding AWS keys in userdata.

### 4. Install the AWS CLI in worker bootstrap

Update `Terraform/userdata.sh` so workers can upload to S3.

Options:

- install `awscli` with `apt-get`
- or install the official AWS CLI v2

Recommendation:

- use the distro package first because it is simpler and consistent with the current
  bootstrap style

### 5. Replace the background simulator launch with a foreground wrapper

Add a small worker-side wrapper script, rendered from Terraform or written directly by
`userdata.sh`, for example:

- `/opt/run_simulator_and_upload.sh`

Responsibilities:

- compute a stable timestamp for the run
- query instance metadata for instance ID
- optionally include hostname or worker index in the object key
- run `RedisFileCacheLRU_Simulator`
- capture its exit code
- upload `/opt/simulator.log` to S3 if upload is enabled
- preserve the simulator exit code for troubleshooting

Pseudo-flow:

```bash
LOG_FILE=/opt/simulator.log
RUN_TS=$(date -u +%Y%m%dT%H%M%SZ)
INSTANCE_ID=$(curl -s http://169.254.169.254/latest/meta-data/instance-id)
OBJECT_KEY="${S3_PREFIX}/${PROJECT}/${RUN_TS}/${INSTANCE_ID}/simulator.log"

./RedisFileCacheLRU_Simulator ... >"$LOG_FILE" 2>&1
SIM_EXIT=$?

if [ "$SIMULATOR_LOG_UPLOAD_ENABLED" = "true" ]; then
  aws s3 cp "$LOG_FILE" "s3://${S3_BUCKET}/${OBJECT_KEY}"
fi

exit "$SIM_EXIT"
```

### 6. Launch the wrapper in the foreground

Recommended approach:

- have `userdata.sh` invoke the wrapper directly and let it run to completion in the
  foreground

Why this is simpler:

- upload can happen immediately after the simulator exits
- the wrapper can return the simulator's exit code directly
- all bootstrap flow stays in one place
- debugging is easier because the full flow is serialized

Tradeoff:

- the EC2 userdata execution will remain active until the simulator finishes

For this PoC, that tradeoff is reasonable and is simpler than adding `nohup` or a
`systemd` unit solely to make log upload work.

### 7. Define the S3 naming convention

Choose one object key structure and document it.

Recommended fields:

- Terraform project name
- UTC timestamp
- EC2 instance ID
- optional worker ordinal from `count.index`

Example:

```text
simulator-logs/poc-redis-cache/20260316T221500Z/worker-0-i-0123456789abcdef0/simulator.log
```

This avoids filename collisions when multiple workers upload logs from the same run.

### 8. Add outputs for operators

Update `Terraform/outputs.tf` with something like:

- `simulator_log_bucket_name`
- `simulator_log_s3_prefix`

If the bucket is Terraform-managed, also output:

- bucket ARN

This makes it easier to locate uploaded logs after `terraform apply`.

### 9. Update example variables and docs

Update:

- `Terraform/terraform.tfvars.example`
- `Terraform/README.md`
- `Terraform/DeepDive.md`

Document:

- how to enable the feature
- whether the bucket is created or pre-existing
- where logs are uploaded
- what IAM access is required
- that uploads happen when the simulator process exits
- any caveat if an instance is terminated before upload completes

## Validation plan

### Terraform validation

Run:

```bash
terraform -chdir=Terraform fmt
terraform -chdir=Terraform validate
```

### Functional validation in AWS

Use a short simulator run for testing, then verify:

1. `terraform apply` succeeds.
2. Worker bootstrap completes.
3. `/opt/simulator.log` exists on a worker.
4. The expected S3 object appears in the configured bucket.
5. The uploaded log content matches the local log.

### Failure-path validation

Verify these cases:

1. Upload feature disabled: workers still behave exactly as they do today.
2. Bucket name missing while upload is enabled: bootstrap should log a clear error.
3. S3 upload failure: simulator output is still kept locally and the failure is visible
   in `/var/log/userdata.log` or the wrapper log.

## Suggested file changes when implementing

Likely files to touch:

- `Terraform/main.tf`
- `Terraform/variables.tf`
- `Terraform/outputs.tf`
- `Terraform/userdata.sh`
- `Terraform/terraform.tfvars.example`
- `Terraform/README.md`
- `Terraform/DeepDive.md`

Possible new file:

- `Terraform/run_simulator_and_upload.sh.tpl`

Using a dedicated template file for the wrapper is cleaner than embedding a long shell
script inline inside `userdata.sh`.

## Open decisions to settle before implementation

1. Should Terraform create the S3 bucket, or should the user supply an existing bucket?
2. Should uploads be best-effort only, or should a failed upload make the worker setup
   visibly fail?
3. Do we want to upload only `simulator.log`, or also `/var/log/userdata.log` for
   debugging failed boots?

## Recommended implementation choice

If we want the most practical first version with a small diff:

1. Add an opt-in existing-bucket configuration.
2. Add an EC2 IAM role + instance profile.
3. Install `awscli`.
4. Add a dedicated wrapper script that runs the simulator in the foreground and uploads
   the log on exit.
5. Keep the feature disabled by default.
6. Document the S3 key layout and validation steps.

That approach stays tightly scoped, preserves current simulator behavior, and avoids
unnecessary C++ changes.

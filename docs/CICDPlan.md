# Plan: Add CI/CD for `poc-redis-cache`

## Goal

Add a practical CI/CD workflow for this repository that:

- automatically validates active C++ and Terraform changes
- gives fast feedback on pull requests
- keeps AWS deployment steps controlled and auditable
- avoids forcing full cloud integration tests on every commit

## Recommendation

Use **GitHub Actions** as the primary CI/CD platform.

Why GitHub Actions is the best fit here:

- the repo already lives on GitHub
- pull request checks are straightforward to wire in
- matrix builds, service containers, and artifact uploads are built in
- AWS deployment can later use GitHub OIDC instead of long-lived secrets
- it is more common and better supported than Travis CI for mixed C++ and Terraform repos

Travis CI can support the CI portion, but it is a weaker choice here because:

- Terraform and AWS deployment workflows are less natural there
- staged deployment approvals are less ergonomic
- long-term maintenance risk is higher than with GitHub Actions

## Current repo constraints that shape the plan

The active code paths are:

- `Cpp/` for the cache, simulator, and CppUnit tests
- `Terraform/` for the AWS worker/EFS/Redis testbed

Important constraints for automation:

- C++ builds depend on `hiredis` and `CppUnit`
- unit tests are registered with `ctest`, but they expect Redis access
- Terraform validation can run in CI without AWS credentials
- Terraform `plan` and `apply` should not run on every pull request
- `Terraform/userdata.sh` builds and runs the simulator on EC2, so end-to-end AWS testing belongs in a later gated stage

## Recommended pipeline design

Use one repo with **three workflow layers**:

1. **Pull request CI**
2. **Main-branch integration**
3. **Manual or gated deployment**

This keeps fast checks fast and keeps AWS costs and risk under control.

## Phase 1: Pull request CI

Create a GitHub Actions workflow such as:

- `.github/workflows/ci.yml`

Trigger on:

- pull requests targeting `main`
- pushes to feature branches if desired

Recommended jobs:

### 1. C++ build and unit test job

Run on Ubuntu and install:

- `cmake`
- `g++` or `clang++`
- `libhiredis-dev`
- `libcppunit-dev`
- `redis-server`

Recommended steps:

1. check out the repo
2. install system packages
3. start a local Redis service
4. configure with CMake
5. build with `cmake --build`
6. run `ctest --output-on-failure`

Notes:

- this validates the active LRU implementation and CppUnit tests
- using a local Redis service container or package install matches repo expectations better than mocking Redis
- if tests assume a clean Redis database, add an explicit `redis-cli FLUSHALL` step before `ctest`

### 2. C++ simulator smoke-build job

This can be separate or folded into the main C++ job.

Goal:

- confirm `RedisFileCacheLRU_Simulator` still builds
- optionally run a short smoke test against local Redis

Recommended first version:

- build only, no long-running simulator execution

### 3. Terraform formatting and validation job

Run:

- `terraform -chdir=Terraform fmt -check`
- `terraform -chdir=Terraform init -backend=false`
- `terraform -chdir=Terraform validate`

Why:

- this catches syntax and formatting issues early
- it does not require AWS credentials
- it fits the current repo guidance

### 4. Optional shell script lint job

Because `Terraform/userdata.sh` is operationally important, add:

- `shellcheck Terraform/userdata.sh`

This is optional, but it will likely catch brittle bootstrap issues early.

## Phase 2: Main-branch integration

Create a second workflow such as:

- `.github/workflows/integration.yml`

Trigger on:

- push to `main`

Purpose:

- run everything from PR CI
- publish useful artifacts
- prepare for controlled deployment

Recommended additions:

### 1. Upload build and test artifacts

Store:

- `build/Testing/Temporary/LastTest.log` if present
- CTest output logs
- optional simulator logs from any smoke run

This will make CI failures easier to diagnose.

### 2. Generate a Terraform plan artifact

Only if AWS credentials are available in the repo environment and the team wants plan visibility.

Recommended behavior:

- run `terraform init`
- run `terraform plan -out=tfplan`
- upload the text plan as an artifact

Important:

- do this only on `main` or on manual dispatch
- never expose secrets, state, or user-specific values in artifacts

## Phase 3: Controlled CD for AWS testbed

Create a deployment workflow such as:

- `.github/workflows/deploy-terraform.yml`

Trigger on:

- manual `workflow_dispatch`
- optionally on tagged releases

Use GitHub Environments such as:

- `testbed`
- optionally later `staging`

Recommended deployment flow:

1. authenticate to AWS using GitHub OIDC
2. run `terraform init`
3. run `terraform plan`
4. require environment approval
5. run `terraform apply`
6. capture outputs as workflow summary or artifact

Why manual first:

- this repo provisions real AWS infrastructure
- the stack creates public networking and billable resources
- `userdata.sh` launches long-running simulator activity

That makes automatic apply on every merge too risky for the first iteration.

## Suggested branch and approval model

Recommended repository policy:

- require PR review before merge to `main`
- require the `ci.yml` workflow to pass before merge
- restrict deployment workflow to maintainers
- require manual approval for the AWS environment

This gives CI on every change without making infrastructure changes automatic.

## Secrets and identity plan

Prefer this order:

### 1. GitHub OIDC for AWS

Best long-term option for CD because:

- no static AWS keys need to live in GitHub secrets
- access can be scoped to one repository and one environment
- role permissions can be limited to Terraform actions for this stack

### 2. Minimal GitHub secrets for non-AWS settings

If needed later:

- Terraform variable values that should not live in the repo
- region overrides
- key-pair names if the stack still needs them

Avoid storing:

- AWS access keys, if OIDC can be used
- `terraform.tfstate`
- personal `terraform.tfvars`

## Recommended rollout order

### Step 1. Add CI only

Implement:

- GitHub Actions PR workflow
- C++ build and test against local Redis
- Terraform fmt and validate
- optional shellcheck

Success criterion:

- every PR gets a reliable pass/fail signal in under about 10 minutes

### Step 2. Stabilize test assumptions

During CI adoption, tighten anything that makes tests flaky:

- ensure Redis is cleaned before tests
- ensure test failures return nonzero exit status consistently
- confirm `ctest` is the standard entry point

If needed, make small repo changes so CI can run deterministically.

### Step 3. Add main-branch artifacts

Implement:

- upload logs and test output
- optionally add a short simulator smoke run

### Step 4. Add gated Terraform deployment

Implement only after the team is comfortable with CI signal quality.

Start with:

- manual deploy to one AWS test environment

Later, if useful:

- scheduled teardown checks
- nightly end-to-end runs
- post-deploy health verification

## Later enhancements

After the first CI/CD version is stable, consider:

- a compiler matrix: `gcc` and `clang`
- a sanitizer job using `-DUSE_ASAN=ON`
- caching for Terraform plugins and CMake build outputs
- a nightly AWS integration run that provisions the stack, waits for the simulator, and archives logs
- status badges in `README.md`
- branch protection requiring both C++ and Terraform checks

## Minimal first deliverable

If the team wants the smallest useful version, the first PR should add:

- `.github/workflows/ci.yml`
- C++ build plus `ctest` with local Redis
- Terraform `fmt -check` and `validate`
- a short README section describing the new checks

That delivers real value without changing runtime behavior or provisioning policy.

## Risks and mitigations

### Risk: Redis-backed tests may be flaky in CI

Mitigation:

- start Redis in the workflow
- clear the database before tests
- keep test execution single-host and deterministic

### Risk: Terraform apply in automation could create unwanted cost

Mitigation:

- keep `apply` manual and environment-gated
- do not run deployment on every merge

### Risk: `userdata.sh` failures are not visible from simple validation

Mitigation:

- lint the script in CI
- treat full AWS bootstrap verification as a later integration workflow

## Travis CI fallback

If the team prefers Travis, use it only for **CI**, not for the full CD design.

A Travis version should include:

- Ubuntu build image
- install `cmake`, `libhiredis-dev`, `libcppunit-dev`, `terraform`, and `redis-server`
- run the C++ build and `ctest`
- run Terraform `fmt -check` and `validate`

Even then, the recommended deployment path remains GitHub-hosted because the repo, review flow, and environment approvals are already centered there.

## Recommendation summary

Adopt **GitHub Actions** in phases:

1. PR CI for C++ and Terraform validation
2. main-branch artifact publishing
3. manual, approval-gated Terraform deployment to AWS

This gives the repo a useful CI/CD path without forcing unreliable cloud tests or risky automatic infrastructure changes in the first iteration.

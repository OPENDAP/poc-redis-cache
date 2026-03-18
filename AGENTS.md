# AGENTS.md

## Scope

These instructions apply to the entire `poc-redis-cache` repository.

## Project Context

- This repo is a proof-of-concept for using Redis as a cluster-level lock manager for a shared file cache.
- The two primary implementation areas are `Cpp/` and `Terraform/`.
- Prefer small, reviewable changes that preserve existing behavior unless the task explicitly calls for broader redesign.
- There are experimental and retired areas in the repo; when a request targets active code, prefer `Cpp/` and `Terraform/` over `retired/` or older variants unless the user asks otherwise.

## Change Discipline

- Keep diffs tightly scoped to the user request.
- Do not rewrite nearby files just for style consistency.
- Do not revert unrelated local changes in a dirty worktree.
- If you find conflicting user edits in files you need to change, stop and ask before overwriting them.
- Only make changes on a branch, never 'main.'

## C++ Guidance

- Treat `Cpp/RedisFileCacheLRU.cpp`, `Cpp/RedisFileCacheLRU.h`, `Cpp/ScriptManager.h`, the simulator, and `Cpp/unit-tests/` as the active C++ implementation.
- The repository uses CMake for the C++ code. Prefer targeted CMake builds over ad hoc compile commands.
- Match the local C++ style in touched files: preserve naming, comment style, and the existing low-refactor approach.
- Avoid API or behavior changes unless the request clearly requires them.
- Be careful with Redis key semantics, locking behavior, filesystem side effects, and cache eviction logic; these are the core behaviors of the PoC.
- When changing tests or simulator behavior, preserve the repo's current assumption that a Redis server is available externally.

## C++ Build And Test

Typical flow from the repo root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Notes:

- `hiredis` and `CppUnit` must be installed and discoverable by CMake.
- If a change only affects one executable or unit test, prefer running the smallest relevant build/test scope first.
- If Redis-backed tests cannot run in the current environment, say so clearly in the summary.

## Terraform Guidance

- Treat `Terraform/` as the active Terraform stack for AWS testbed work.
- Keep Terraform compatible with the style already used in `Terraform/main.tf`, `Terraform/variables.tf`, `Terraform/outputs.tf`, and `Terraform/userdata.sh`.
- Favor minimal infrastructure changes and preserve current variable names, outputs, and user-data assumptions unless the task requires otherwise.
- Be especially careful with security group rules, public exposure, AMI selection, and boot-time provisioning behavior; small edits there can materially change runtime behavior or cost.
- Do not edit generated or local-only artifacts such as plan captures or state files unless the user explicitly asks for that.

## Terraform Validation

For Terraform changes in `Terraform/`, prefer this flow:

```sh
terraform -chdir=Terraform fmt
terraform -chdir=Terraform validate
```

Notes:

- Run `terraform plan` only when it is useful for the task and credentials/environment are available.
- Never commit secrets or user-specific values from `terraform.tfvars`, plan output, or state files.
- Treat `Terraform/terraform.tfvars.example` as the template; do not assume `Terraform/terraform.tfvars` should be edited unless requested.

## Documentation

- If code behavior changes, update the most relevant README or deep-dive document in the same area when it materially helps future users.
- Keep documentation practical and close to the code being changed.

## Review Priorities

When reviewing or self-checking changes, prioritize:

1. Behavioral regressions in cache locking, cache eviction, and shared-directory behavior
2. Terraform changes that alter network exposure, instance boot behavior, or Redis connectivity
3. Build or test regressions in CMake, CppUnit, or Terraform validation
4. Missing documentation for non-obvious operational changes

## Communication

- State what you validated and what you could not validate.
- If a task touches AWS, Redis, or multi-process behavior but full end-to-end testing was not possible, call that out explicitly.

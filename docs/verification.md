# Verification

Repair baseline: `5118fe4`. Local repair branch: `codex/verification-and-docs`.

Executed on 2026-09-10T07:30:28+00:00 in the
`hermitdb-dev` Linux aarch64 Docker image, GCC 13.3.0, CMake RelWithDebInfo.

| Check | Observed result |
|---|---|
| Full GCC build (-Wall -Wextra -Werror) | Passed |
| `ctest --test-dir build --output-on-failure` | 16/16 CTest groups passed |
| Integration suite, `--threads=2` | 7/7 groups passed |
| Integration suite, `--threads=4` | 7/7 groups passed |
| Integration suite, `--threads=8` | 7/7 groups passed |
| TSan build of server, `HERMIT_TSAN=ON` | Passed |
| TSan concurrency and INCR integration, `--threads=4` | 2/2 groups passed; no sanitizer report |
| `git diff --check` | Passed |

CTest groups are executables/scripts, not individual assertions. Each crash
script run includes five acknowledged-state rounds, eight pending-command
rounds, and one everysec prefix-consistency round. The workload is seeded and
compared against an independent dictionary shadow model. The INCR integration
checks 16 clients x 500 increments = 8,000.

New unit cases exercise failed writes with Linux `/dev/full`, failed deferred
fsync with `/dev/null`, and a lazy-expiry logging failure. Another case checks
that disabled compaction does not invoke its callback or change the WAL.

The TSan run was targeted: the complete sanitizer unit suite configured in CI
was not run locally. GitHub Actions has not yet verified this branch. No fresh
performance benchmark, hardware power-loss test, or deployed-service test was run.

## Scope

Regression coverage checks WAL error handling, refusal of unsafe compaction,
and acknowledged writes surviving process termination. SIGKILL retains the
host's page cache and is not a power-loss simulation. These tests do not prove
all filesystem crash windows or complete Redis compatibility.

Historical 15/15 and performance claims remain in docs/history/README.md.
They are not results for this revision. No benchmark raw data was supplied.

# native_sim CI Checklist

Use this checklist when adding or reviewing native_sim coverage in CI.

- [ ] Build command for native_sim is part of PR checks.
- [ ] At least one smoke test target runs under native_sim.
- [ ] Logs are archived as CI artifacts.
- [ ] Crash signatures are scanned (assert, panic, segfault).
- [ ] Optional host tooling checks (GDB/Valgrind) are enabled for nightly runs.

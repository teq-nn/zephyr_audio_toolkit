# Repository Guidelines

## Documentation Hierarchy
- `audio_pipeline_manifest.md` is the architectural contract; update it before changing thread, buffer, or role semantics.
- `audio_pipeline_spec_v2.md` is the implementation blueprint; keep it authoritative and mirror any API edits (e.g., `audio_node_ops` signatures) here first.
- `audio_pipeline_spec_short.md` is the onboarding digest; refresh it after major manifest/spec edits so new agents grasp deltas quickly. It never overrides the manifest or the spec — on conflict, those win.
- `README.md` describes the tree and the build/test entry points; keep its layout section in step with the actual directories.

## Project Structure & Module Organization
This repository is an out-of-tree Zephyr module. The tree follows manifest §12 / spec §14 — keep it that way and document any new file tree in the manifest to preserve the systems view.

- `zephyr/module.yml` is the Zephyr module manifest; `CMakeLists.txt` and `Kconfig` at the repository root are the module glue it points at (`cmake: .`, `kconfig: Kconfig`, both resolved from the module root). The file must live in `zephyr/` — `scripts/zephyr_module.py` only looks for `zephyr/module.{yml,yaml}`, so a root-level `module.yml` is silently ignored and `CONFIG_AUDIO_PIPELINE` never gets defined.
- `include/zephyr/audio/` holds the public headers (`audio_format.h`, `audio_node.h`, `audio_nodes.h`, `audio_pipeline.h`, `audio_pipeline_events.h`, `audio_wav.h`); applications include them as `<zephyr/audio/...>`. `audio_nodes.h` carries the per-node state types, the `<role>_node_ops` externs, and the node `*_DEFINE` macros; `audio_wav.h` is the RIFF/WAVE header codec, and everything that reads or emits a WAV header goes through it rather than spelling out field offsets - the sole exception being tests that construct deliberately malformed headers the module refuses to produce.
- `subsys/audio/pipeline/` holds the implementation (`audio_pipeline_core.c`, `audio_pipeline_config.c`, `audio_pipeline_events.c`, `audio_node_core.c`, `audio_wav.c`, private `audio_internal.h`), with node implementations under `nodes/`.
- `samples/audio/pipeline_basic/` holds the reference application.
- `tests/subsys/audio/pipeline/` holds the pipeline Ztest suites and their shared fixture and fake nodes; `tests/subsys/audio/wav/` holds the standalone WAV header suite, which builds without `CONFIG_AUDIO_PIPELINE`.

New sources belong in `subsys/audio/pipeline/` (and must be added to its `CMakeLists.txt`); new public API belongs in `include/zephyr/audio/`.

## Build, Test, and Development Commands
Run these from the repository root inside an initialised west workspace. If the module is registered in the west manifest, the `ZEPHYR_EXTRA_MODULES` arguments can be dropped.

- `west build -b native_sim -d build/sample samples/audio/pipeline_basic -- -DZEPHYR_EXTRA_MODULES=$PWD` builds the sample against Zephyr on the host simulator; swap `-b` for your board target when validating filters or sinks.
- `west flash -d build/hw` deploys a hardware build onto the target — run after smoke-testing on `native_sim`.
- `west twister -T tests -p native_sim -x=ZEPHYR_EXTRA_MODULES=$PWD` executes the suites headlessly; swap `-p native_sim` for `-p <BOARD>` when coverage must include driver-backed sinks.

## Coding Style & Naming Conventions
- Follow Zephyr's K&R C style: tabs for indentation (8 spaces visual), braces on the same line, and snake_case identifiers.
- Public symbols use the `audio_` prefix and live under the `zephyr/audio/` include namespace; node implementations use the `audio_node_*` / `<role>_node` naming already in `nodes/`. Static allocators stay in `*_DEFINE` macros — the subsystem never calls `k_malloc`.
- Kconfig symbols are prefixed `AUDIO_PIPELINE_`.
- Run `checkpatch.pl --strict` and `clang-format -style=file` (when provided) before submitting even documentation-owned changes touching code blocks.

## Testing Guidelines
- Add Ztest cases under `tests/subsys/audio/pipeline/` (or `tests/subsys/audio/wav/` for the header codec) and mirror the frame/EOF behaviors described in the manifest and spec §12. Each suite directory needs a `testcase.yaml` (plus `prj.conf`) for Twister to discover it.
- Name tests `test_<role>_<behavior>` (e.g., `test_sink_reports_eof`). Gate merges on the full Twister run plus hardware smoke tests for new sinks.
- Tests must run on `native_sim`/QEMU without real audio hardware (spec §12.1).
- Validate documentation changes by cross-referencing manifest/spec diffs; unresolved conflicts block release tagging.

## Commit & Pull Request Guidelines
- Prefer single-purpose commits with imperative summaries (`Add sink EOF callback`). Wrap bodies at 72 chars and reference the issue being implemented.
- PRs must link to the updated spec section, describe validation steps (`west build`, `west twister`), and include logs or screenshots when hardware output mattered.
- Request review from both architecture and platform maintainers; manifest/spec edits require dual approval to keep the contract synchronized.

## Agent skills

### Issue tracker

Issues and PRDs live as GitHub issues in `teq-nn/zephyr_audio_toolkit`, managed via the `gh` CLI. See `docs/agents/issue-tracker.md`.

### Triage labels

The five canonical triage roles, each label string equal to its name. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context — `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

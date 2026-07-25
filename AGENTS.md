# Repository Guidelines

## Where the truth lives

**The code is the specification.** There is no separate spec document, and adding one back is not an improvement — the pair drifts, and a reader who cannot tell which side is stale has less than they started with. Behaviour is defined by the public headers, the Kconfig help text, and the tests, in that order.

Only two kinds of document survive alongside the code, both chosen because **no code change can falsify them**:

- **`docs/adr/`** — architectural decisions, one paragraph each. An ADR records *that* a decision was made and *why*, at a point in time. It never describes current API shape. Write one only when the decision is hard to reverse, surprising without context, *and* the result of a real trade-off; see `.claude/skills/domain-modeling/ADR-FORMAT.md`. Supersede an ADR by writing a new one, never by editing it into agreement with the code.
- **`CONTEXT.md`** — the glossary. Use its terms in code, tests, commit messages and issues, and prefer them over the synonyms it lists under `_Avoid_`. It holds no implementation detail.

Two rules follow:

- **Do not restate code in prose.** Signatures, struct fields, Kconfig defaults, file trees and test-file listings have exactly one home. If a sentence you are about to write could be falsified by an edit elsewhere in the tree, it belongs in a doc comment next to the thing it describes, or nowhere.
- **Unimplemented is not documented.** Work that has not been done is a GitHub issue, not a paragraph. A deliberate *boundary* — something v1 will not do — is an ADR.

If a contradiction between an ADR and the code turns up, surface it rather than silently picking a side: the code may be a bug, or the decision may have been superseded without a record.

## Project Structure & Module Organization
This repository is an out-of-tree Zephyr module. `ls` is the authority on the tree; do not maintain a copy of it in prose.

Where things belong:

- Public API goes in `include/zephyr/audio/`, and applications include it as `<zephyr/audio/...>`. Implementation goes in `subsys/audio/pipeline/` and must be added to its `CMakeLists.txt`; node implementations go under `nodes/`.
- Pipeline Ztest suites go in `tests/subsys/audio/pipeline/`, alongside the shared fixture and fake nodes they reuse. `tests/subsys/audio/wav/` is separate because it builds without `CONFIG_AUDIO_PIPELINE`.

Two constraints that are not visible from the tree itself:

- **`zephyr/module.yml` must live in `zephyr/`.** `scripts/zephyr_module.py` looks only for `zephyr/module.{yml,yaml}`, so a root-level `module.yml` is silently ignored and `CONFIG_AUDIO_PIPELINE` never gets defined. It points the build at the root `CMakeLists.txt` and `Kconfig` (`cmake: .`, `kconfig: Kconfig`, both resolved from the module root).
- **All RIFF/WAVE header handling goes through `audio_wav.h`.** Nothing else may spell out field offsets. The sole exception is tests that construct deliberately malformed headers the module refuses to produce.

## Build, Test, and Development Commands
Run these from the repository root inside an initialised west workspace. If the module is registered in the west manifest, the `ZEPHYR_EXTRA_MODULES` arguments can be dropped.

- `west build -b native_sim -d build/sample samples/audio/pipeline_basic -- -DZEPHYR_EXTRA_MODULES=$PWD` builds the sample against Zephyr on the host simulator; swap `-b` for your board target when validating filters or sinks.
- `west flash -d build/hw` deploys a hardware build onto the target — run after smoke-testing on `native_sim`.
- `west twister -T tests -p native_sim -x=ZEPHYR_EXTRA_MODULES=$PWD` executes the suites headlessly; swap `-p native_sim` for `-p <BOARD>` when coverage must include driver-backed sinks.

## Coding Style & Naming Conventions
- Follow Zephyr's K&R C style: tabs for indentation (8 spaces visual), braces on the same line, and snake_case identifiers.
- Public symbols use the `audio_` prefix and live under the `zephyr/audio/` include namespace; node implementations use the `audio_node_*` / `<role>_node` naming already in `nodes/`. Static allocators stay in `*_DEFINE` macros — the subsystem never calls `k_malloc`.
- Kconfig symbols are prefixed `AUDIO_PIPELINE_`.
- Run `checkpatch.pl --strict` and `clang-format -style=file` (when provided) before submitting.

## Testing Guidelines
- Add Ztest cases under `tests/subsys/audio/pipeline/` (or `tests/subsys/audio/wav/` for the header codec). Each suite directory needs a `testcase.yaml` (plus `prj.conf`) for Twister to discover it.
- Name tests `test_<role>_<behavior>` (e.g., `test_sink_reports_eof`), using the vocabulary in `CONTEXT.md`. Gate merges on the full Twister run plus hardware smoke tests for new sinks.
- Tests must run on `native_sim`/QEMU without real audio hardware.
- The tests are part of the specification: a behaviour worth stating in prose is worth asserting in a test. When you change behaviour deliberately, change the test name too, so the diff records the decision.

## Commit & Pull Request Guidelines
- Prefer single-purpose commits with imperative summaries (`Add sink EOF callback`). Wrap bodies at 72 chars and reference the issue being implemented.
- The commit message is where rationale goes when it is too specific for an ADR. Say what was wrong and why the new shape is right, not just what changed — this is the only record a future reader gets.
- PRs must describe validation steps (`west build`, `west twister`) and include logs when hardware output mattered. Link any ADR the change adds or supersedes.
- Request review from both architecture and platform maintainers; a new or superseded ADR requires dual approval.

## Agent skills

### Issue tracker

Issues and PRDs live as GitHub issues in `teq-nn/zephyr_audio_toolkit`, managed via the `gh` CLI. See `docs/agents/issue-tracker.md`.

The tracker is the record of unfinished work: anything not built yet is an issue, never a paragraph in a document. `ready-for-agent` means fully specified; `ready-for-human` means a decision is still open.

### Triage labels

The five canonical triage roles, each label string equal to its name. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context — `CONTEXT.md` and `docs/adr/` at the repo root. See `docs/agents/domain.md`.

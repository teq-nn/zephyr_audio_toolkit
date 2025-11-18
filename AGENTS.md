# Repository Guidelines

## Project Structure & Module Organization
- `audio_pipeline_manifest.md` is the architectural contract; update it before changing thread, buffer, or role semantics.
- `audio_pipeline_spec_v2.md` is the implementation blueprint; keep it authoritative and mirror any API edits (e.g., `audio_node_ops` signatures) here first.
- `audio_pipeline_spec_short.md` is the onboarding digest; refresh it after major manifest/spec edits so new agents grasp deltas quickly.
- Place forthcoming Zephyr sources under `src/` and public headers in `include/audio_pipeline/`; reserve `tests/` for Twister scenarios and fixture data. Keep new file trees documented in the manifest to preserve the systems view.

## Build, Test, and Development Commands
- `west build -b nrf5340_audio src` builds the current module against Zephyr; swap `-b` for your board target when validating filters or sinks.
- `west flash` deploys the last build onto hardware—run after smoke-testing filters with the host-based harness.
- `west twister -T tests` executes unit and integration suites headlessly; use `-p YOUR_BOARD` when coverage must include driver-backed sinks.

## Coding Style & Naming Conventions
- Follow Zephyr’s K&R C style: tabs for indentation (8 spaces visual), braces on the same line, and snake_case identifiers.
- Node structs must take the `audio_node_*` prefix (`audio_node_filter_eq`, `audio_node_sink_i2s`). Static allocators stay in `*_DEFINE` macros.
- Run `checkpatch.pl --strict` and `clang-format -style=file` (when provided) before submitting even documentation-owned changes touching code blocks.

## Testing Guidelines
- Implement Twister cases per feature (`tests/filters/eq/`, `tests/sinks/file/`) and mirror the frame/EOF behaviors described in the manifest.
- Name tests `test_<role>_<behavior>` (e.g., `test_sink_reports_eof`). Gate merges on all tests plus hardware smoke tests for new sinks.
- Validate documentation changes by cross-referencing manifest/spec diffs; unresolved conflicts block release tagging.

## Commit & Pull Request Guidelines
- Prefer single-purpose commits with imperative summaries (`Add sink EOF callback`). Wrap bodies at 72 chars and reference Zephyr issues using `Fixes ZEPHYR-####` when applicable.
- PRs must link to the updated spec section, describe validation steps (`west build`, `west twister`), and include logs or screenshots when hardware output mattered.
- Request review from both architecture and platform maintainers; manifest/spec edits require dual approval to keep the contract synchronized.

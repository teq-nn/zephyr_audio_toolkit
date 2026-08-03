#!/usr/bin/env bash
#
# Run the audio pipeline Ztest suites - or the samples - headlessly with Twister.
#
# This script is the *only* place the test invocation is spelled out: CI
# (.github/workflows/ci.yml) runs exactly this, so a local run and a CI run
# cannot drift apart.
#
# Usage:
#   ./scripts/ci-test.sh [extra twister args...]
#
# Examples:
#   ./scripts/ci-test.sh                       # full suite on native_sim
#   ./scripts/ci-test.sh --no-clean            # keep previous build output
#   CI_TEST_PLATFORM=native_sim/native/64 ./scripts/ci-test.sh
#   CI_TEST_PATH=tests/subsys/audio/pipeline ./scripts/ci-test.sh
#   CI_TEST_PLATFORM=nucleo_h723zg CI_TEST_PATH=samples ./scripts/ci-test.sh
#       ^ the hardware sample; its sample.yaml is build_only, so this compiles
#         and stops. Needs the arm-zephyr-eabi toolchain rather than the host
#         compiler native_sim uses.
#
# Environment:
#   CI_TEST_PLATFORM  Board target passed to `twister -p` (default: native_sim)
#   CI_TEST_PATH      Test root passed to `twister -T`    (default: tests)
#   CI_TEST_OUTDIR    Twister output directory            (default: ./twister-out)
#
# Prerequisites (this script installs nothing):
#   * west, and an initialised west workspace that contains both this
#     repository and a Zephyr checkout. From a fresh clone:
#         pip install west
#         west init -l /path/to/zephyr_audio_toolkit
#         cd /path/to/workspace && west update
#         west packages pip --install        # Zephyr's Python requirements
#   * Zephyr build tooling: CMake >= 3.20.5, Ninja, and the Zephyr SDK host
#     tools (https://docs.zephyrproject.org/latest/develop/getting_started/).
#     native_sim compiles with the *host* gcc, so no cross toolchain is needed.
#   * The default `native_sim` target is 32-bit, which needs the 32-bit host C
#     library:  sudo apt-get install gcc-multilib g++-multilib libc6-dev-i386
#     (or run the 64-bit variant: CI_TEST_PLATFORM=native_sim/native/64).
#
# Exit status is Twister's: non-zero if any suite fails to build or any ztest
# case fails, which is what gates merges.
#
# SPDX-License-Identifier: Apache-2.0

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

platform="${CI_TEST_PLATFORM:-native_sim}"
test_path="${CI_TEST_PATH:-tests}"
outdir="${CI_TEST_OUTDIR:-${repo_root}/twister-out}"

die() {
	echo "ci-test.sh: $*" >&2
	exit 1
}

# Everything below is relative to the repository, no matter where the script
# was invoked from.
cd "${repo_root}"

command -v west >/dev/null 2>&1 ||
	die "west not found - see the prerequisites at the top of this script"

west topdir >/dev/null 2>&1 ||
	die "${repo_root} is not inside a west workspace - run 'west init -l ${repo_root}' and 'west update' first"

# When this repository is part of the workspace (it is the manifest repo in
# CI), Zephyr discovers the module through west and passing
# ZEPHYR_EXTRA_MODULES as well would register it twice. Only inject it when
# the repo is invisible to west, e.g. a plain clone dropped next to a
# pre-existing workspace.
extra_args=()
if west list --format='{abspath}' 2>/dev/null | grep -qxF "${repo_root}"; then
	echo "ci-test.sh: module is a west project; relying on west module discovery"
else
	echo "ci-test.sh: module is not a west project; passing ZEPHYR_EXTRA_MODULES"
	extra_args+=("-x=ZEPHYR_EXTRA_MODULES=${repo_root}")
fi

set -x
exec west twister \
	-T "${test_path}" \
	-p "${platform}" \
	--outdir "${outdir}" \
	--inline-logs \
	--verbose \
	${extra_args[@]+"${extra_args[@]}"} \
	"$@"

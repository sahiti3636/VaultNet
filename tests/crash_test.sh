#!/usr/bin/env bash
set -euo pipefail
make -s crash-test
exec ./bin/vaultnet_crash_test "${CRASH_ITERS:-20}"

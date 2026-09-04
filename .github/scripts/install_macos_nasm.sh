#!/usr/bin/env bash

set -euo pipefail

readonly NASM_VERSION="2.16.03"
readonly NASM_SHA256="5bc940dd8a4245686976a8f7e96ba9340a0915f2d5b88356874890e207bdb581"
readonly NASM_ARCHIVE="${RUNNER_TEMP:?RUNNER_TEMP must be set}/nasm-${NASM_VERSION}.tar.gz"
readonly NASM_SOURCE_DIR="${RUNNER_TEMP}/nasm-${NASM_VERSION}"
readonly NASM_INSTALL_DIR="${RUNNER_TEMP}/nasm-${NASM_VERSION}-install"

curl --fail --location --silent --show-error \
    --output "${NASM_ARCHIVE}" \
    "https://www.nasm.us/pub/nasm/releasebuilds/${NASM_VERSION}/nasm-${NASM_VERSION}.tar.gz"
printf '%s  %s\n' "${NASM_SHA256}" "${NASM_ARCHIVE}" | shasum --algorithm 256 --check
tar --extract --gzip --file "${NASM_ARCHIVE}" --directory "${RUNNER_TEMP}"

(
    cd "${NASM_SOURCE_DIR}"
    ./configure --prefix="${NASM_INSTALL_DIR}"
    make -j"$(sysctl -n hw.logicalcpu)"
    make install
)

"${NASM_INSTALL_DIR}/bin/nasm" -v | grep --fixed-strings "version ${NASM_VERSION}"
printf '%s\n' "${NASM_INSTALL_DIR}/bin" >> "${GITHUB_PATH:?GITHUB_PATH must be set}"

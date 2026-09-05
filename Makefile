PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXT_NAME=lerobot
EXT_CONFIG=$(PROJ_DIR)extension_config.cmake

# GITHUB_PATH only affects later Actions steps. Export the same install path
# here so commands in this make invocation can also find NASM after setup.
ifeq ($(shell uname -s),Darwin)
ifneq ($(RUNNER_TEMP),)
export PATH := $(RUNNER_TEMP)/lerobot-nasm/bin:$(PATH)
endif
endif

include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: install_ci_dependencies
configure_ci: install_ci_dependencies

install_ci_dependencies:
	@if [ "$$(uname -s)" = "Darwin" ]; then \
		bash .github/scripts/install_macos_nasm.sh; \
	fi

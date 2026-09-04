BUILD_DIR ?= build/release
BUILD_TYPE ?= Release
JOBS ?= 8
EXTRA_CMAKE_ARGS ?=

CMAKE_ARGS := \
	-S duckdb \
	-B $(BUILD_DIR) \
	-DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
	-DDUCKDB_EXTENSION_CONFIGS=$(CURDIR)/extension_config.cmake \
	$(EXTRA_CMAKE_ARGS)

.PHONY: all configure build test

all: build

configure:
	cmake $(CMAKE_ARGS)

build: configure
	cmake --build $(BUILD_DIR) --target lerobot_loadable_extension shell -j$(JOBS)

test: configure
	cmake --build $(BUILD_DIR) --target lerobot_loadable_extension shell unittest -j$(JOBS)
	LD_LIBRARY_PATH="$(CURDIR)/$(BUILD_DIR)/src:$${LD_LIBRARY_PATH}" \
		$(BUILD_DIR)/test/unittest "$(CURDIR)/test/*"

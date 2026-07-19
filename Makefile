BUILD_DIR := build
JOBS ?= $(shell nproc)
# Subset filter for `make test-one` (ctest -R regex).
T ?=

.PHONY: configure build run clean test test-failed test-one snapshots-update snapshots-check

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release

build:
	@test -f $(BUILD_DIR)/build.ninja || $(MAKE) configure
	cmake --build $(BUILD_DIR)

run: build
	./$(BUILD_DIR)/suopTranscriber

# Build everything, then run the whole ctest suite in parallel. ctest only RUNS
# binaries, so build first (this target does). tst_visual_snapshot shows as
# skipped here by design — it's opt-in (see snapshots-* below). Some tests use
# the real system trash but restore it, and print harmless "failed to load
# model" lines; neither is a failure.
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -j $(JOBS)

# Re-run only the tests that failed last run.
test-failed:
	ctest --test-dir $(BUILD_DIR) --output-on-failure --rerun-failed

# Run a subset by name regex: `make test-one T=sidebar`
test-one: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -R "$(T)"

# Opt-in pixel snapshots (dormant in `make test`): generate goldens, then check.
# Goldens are environment-specific and .gitignore'd (tests/snapshots/).
snapshots-update: build
	env SUOP_SNAPSHOT=update $(BUILD_DIR)/tests/tst_visual_snapshot
snapshots-check: build
	env SUOP_SNAPSHOT=compare $(BUILD_DIR)/tests/tst_visual_snapshot

clean:
	rm -rf $(BUILD_DIR)

# One build tree PER build type, rather than one `build/` whose CMAKE_BUILD_TYPE
# is whatever the last configure happened to set. A shared tree silently
# inherits a type nobody chose: the IDE configures Debug, `make build` only
# runs `configure` when build.ninja is MISSING, so `make run` goes on building
# the IDE's Debug cache forever — which is how a Debug-only ggml assert in
# whisper's VAD (see patches/whisper-vad-lstm-contiguous-input.patch) ended up
# reachable from `make run` at all. Keying the directory on the type makes that
# drift impossible instead of merely detectable, and both trees stay warm, so
# alternating between the IDE and the terminal costs an incremental build
# rather than a full one (a type flip in one tree invalidates every object).
#
# `make run BUILD_TYPE=Debug` is the assert-enabled build — worth reaching for
# when something misbehaves, since ggml/whisper assert-guard real invariants
# that Release compiles out. Note `make install BUILD_TYPE=Debug` repoints the
# ~/.local/bin wrapper at the Debug tree: last install wins, by design.
BUILD_TYPE ?= Release
BUILD_DIR  := build-$(BUILD_TYPE)
JOBS ?= $(shell nproc)
# Subset filter for `make test-one` (ctest -R regex).
T ?=

# Desktop integration, mirroring the sister repos suopPanel/suopNotes: a
# launcher in ~/.local/share/applications is what makes KRunner / the app grid
# able to start this outside an IDE.
#
# Nothing is COPIED out of the working tree, unlike those repos. The binary
# resolves models/, engines/, and the CUDA compat lib/ relative to itself
# (QCoreApplication::applicationDirPath), so an installed copy in ~/.local/bin
# would look for all three in the wrong place. ~/.local/bin gets a one-line
# exec wrapper onto the build tree instead — which also keeps `make build` the
# only step needed to pick a change up. Moving or deleting the repo breaks the
# launcher, by design.
PREFIX  := $(HOME)/.local
BINDIR  := $(PREFIX)/bin
APPDIR  := $(PREFIX)/share/applications
LAUNCHER_BIN := $(BINDIR)/suopTranscriber
# The .desktop id must match QGuiApplication::setDesktopFileName() in
# src/main.cpp, or Wayland's app_id won't match this entry and the running
# window shows no icon/name in the task manager. tst_styleguide guards the pair.
DESKTOP_FILE := $(APPDIR)/org.suop.Transcriber.desktop

.PHONY: configure build install uninstall run clean clean-all test test-failed test-one snapshots-update snapshots-check

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)

build:
	@test -f $(BUILD_DIR)/build.ninja || $(MAKE) configure
	cmake --build $(BUILD_DIR)

install: build
	mkdir -p $(BINDIR) $(APPDIR)
	printf '#!/bin/sh\nexec "%s" "$$@"\n' '$(CURDIR)/$(BUILD_DIR)/suopTranscriber' > $(LAUNCHER_BIN)
	chmod 755 $(LAUNCHER_BIN)
	sed -e 's|@SUOP_TRANSCRIBER_EXEC@|$(LAUNCHER_BIN)|g' \
	    dist/org.suop.Transcriber.desktop.in > $(DESKTOP_FILE)
	-update-desktop-database $(APPDIR) 2>/dev/null || true
	@echo "Installed $(LAUNCHER_BIN) and $(DESKTOP_FILE)"

uninstall:
	rm -f $(LAUNCHER_BIN) $(DESKTOP_FILE)
	-update-desktop-database $(APPDIR) 2>/dev/null || true
	@echo "Removed launcher wrapper and desktop entry"

run: install
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

# Removes only THIS build type's tree; `clean-all` takes every type. Both are
# expensive to undo — a from-scratch build is ~80 minutes of compile work and
# re-downloads ~2.9G into _deps (onnxruntime + the CUDA-12 wheels), so it needs
# the network back. Ninja's incremental build is reliable here; reach for these
# when a configure-level input changed, not on suspicion of staleness.
clean:
	rm -rf $(BUILD_DIR)

clean-all:
	rm -rf build build-*

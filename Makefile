BUILD_DIR := build
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

.PHONY: configure build install uninstall run clean test test-failed test-one snapshots-update snapshots-check

configure:
	cmake -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=Release

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

clean:
	rm -rf $(BUILD_DIR)

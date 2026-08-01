# suopTranscriber

A GPU-accelerated transcription app for Linux, built around whisper.cpp and
sherpa-onnx (Parakeet-TDT). Transcripts are durable, human-readable Markdown
files in an Obsidian-style vault — one `.md` per audio file, YAML frontmatter
plus one timestamped line per sentence — with sentence-level highlighting
synced to audio playback.

Features:

- Transcription via whisper.cpp (linked in-process, warm model) or
  Parakeet-TDT 0.6B v2 (in-process via sherpa-onnx). Additional engines can
  run out-of-process behind a JSONL manifest protocol.
- Speaker diarization (sherpa-onnx: pyannote/Reverb segmentation +
  WeSpeaker/3D-Speaker/TitaNet embeddings), with speaker rename/merge.
- AI auto-titles and auto-chapters via a local llama.cpp model (optional,
  `SUOP_ENABLE_LABELING`).
- Built-in recorder (microphone, system audio, and per-application capture
  via PipeWire/PulseAudio).
- Vault file management (rename/move/trash/combine/import) as undoable
  queued tasks.

Linux-first: developed on CachyOS. NVIDIA CUDA is an optional fast path
(auto-detected); the Vulkan backend is the cross-vendor GPU baseline and
works on NVIDIA and AMD.

## Requirements

- Qt >= 6.7 with Core, Gui, Qml, Quick, QuickControls2, Multimedia,
  Core5Compat, DBus, Svg
- KF6WindowSystem >= 6.19
- CMake >= 3.21 and Ninja
- A C++20 compiler
- `ffmpeg` on PATH (all audio decode/resample; Qt Multimedia only plays)
- `curl` and `tar` on PATH (runtime model downloads)
- Optional: CUDA toolkit — auto-detected at configure time; pass
  `-DSUOP_ENABLE_CUDA=OFF` for the fully open Vulkan-only build
- Optional: PipeWire (`pw-record`, `pw-dump`) or PulseAudio (`parec`) for
  the recorder's system/app capture

## Build

The submodules are mandatory — the build vendors whisper.cpp, llama.cpp, and
sherpa-onnx in-tree. Note that `third_party/sherpa-onnx` points at the fork
`github.com/suopuwu/sherpa-onnx` (see THIRD_PARTY_NOTICES.md); a plain
checkout against upstream cannot resolve the pinned commit.

```
git clone --recursive https://github.com/suopuwu/suopTranscriber.git
cd suopTranscriber
make build        # = cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
                  #   + cmake --build build
make install      # desktop entry + ~/.local/bin/suopTranscriber wrapper
make run          # build + install + launch
make uninstall    # remove both
```

`make install` doesn't copy anything out of the working tree — the app finds
`models/`, `engines/`, and its CUDA compat `lib/` next to the binary, so the
installed wrapper just `exec`s `build/suopTranscriber` in place. It makes the
app startable from KRunner / an app launcher (and from any shell); the repo has
to stay put, and `make build` alone picks up changes.

Or directly:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/suopTranscriber [/path/to/vault]
```

Configure options:

- `-DSUOP_ENABLE_VULKAN=ON` (default) — cross-vendor GPU baseline
- `-DSUOP_ENABLE_CUDA=ON/OFF` — NVIDIA fast path; defaults ON only when a
  CUDA compiler is detected
- `-DSUOP_ENABLE_LABELING=ON` (default) — builds llama.cpp for AI
  auto-titles/chapters

## Models

Nothing is bundled. The app downloads models at runtime, on demand, into the
vault's models store — whisper ggml tiers, the Silero VAD model, the
Parakeet-TDT bundle, diarization segmentation/embedding models, and the
optional Qwen3 labeling model. Every catalog download is SHA-256 pinned and
verified at install time (see DEPENDENCIES.md).

Some models carry their own licenses — e.g. Reverb diarization is
non-commercial, Parakeet-TDT is CC-BY-4.0. See THIRD_PARTY_NOTICES.md before
redistributing anything.

## Testing

```
make test              # build + full ctest suite in parallel
make test-one T=regex  # subset by name
make test-failed       # re-run last failures
```

Many tests exercise real models and QSKIP when none is available locally.
Enable them with:

| env var | what it points at |
|---|---|
| `SUOP_TEST_MODEL` | a whisper ggml `.bin` (real transcription tests) |
| `SUOP_TEST_VAD_MODEL` | a Silero VAD ggml (`tst_vadchunking`) |
| `SUOP_TEST_LABEL_MODEL` | a labeling GGUF (`tst_labelingengine`, auto-title/chapters tests) |
| `SUOP_TEST_DIARIZATION_SEG_MODEL` | a diarization segmentation ONNX |
| `SUOP_TEST_DIARIZATION_EMBED_MODEL` | a speaker-embedding ONNX |
| `SUOP_PARAKEET_DIR` | a downloaded Parakeet bundle dir (`tst_parakeetengine` real decode) |
| `SUOP_SNAPSHOT` | `update`/`compare` — opt-in pixel snapshots (`make snapshots-update` / `snapshots-check`) |
| `SUOP_ICON_GALLERY` | `1` — render icon-gallery PNGs (`tst_icontheme`) |

LIVE_TESTING.md is the manual checklist for what automation can't reach
(real GPU, real audio devices, real compositor).

## Architecture

Read DESIGN.md first — it is the source of truth for architecture and
decisions. StyleGuide.md holds the UI/UX conventions (enforced by
`tst_styleguide`). DEPENDENCIES.md documents every pin and the procedure
for bumping a submodule, wheel, or model.

## License

GPL-3.0-or-later — see [LICENSE](LICENSE). Third-party components keep
their own licenses; see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

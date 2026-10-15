# suopTranscriber — Dependencies

Where every vendored/downloaded dependency is pinned, how to bump it, and what
breaks silently if the bump is done wrong. Read this before touching a
submodule commit, a fetched wheel, or a `ModelManager` file name/URL.

## Where every pin lives

**Submodules** (`.gitmodules`, pin from `git submodule status`):

| path | pin (describe) |
|---|---|
| `third_party/llama.cpp` | `gguf-v0.19.0-985-gaff6eb6e7` |
| `third_party/whisper.cpp` | `v1.8.5-251-ga630b35c` |
| `third_party/sherpa-onnx` | `v1.13.4-3-ga5ceedc2` — fork, `suopuwu/sherpa-onnx`, not upstream `k2-fsa/sherpa-onnx` |

**Downloaded model artifacts** (`src/core/ModelManager.{h,cpp}`). Each installed file is
pinned by SHA-256 in `pinnedSha256()` in `ModelManager.cpp` — the runtime counterpart of the
`suop_fetch_cuda12_wheel` digests below. The pin is on the **installed** file, not the
download: both tar.bz2 routes delete the archive right after extracting. It is verified once,
at install time, and a mismatch fails the `DownloadTask` and deletes the file. All 16 catalog
artifacts are pinned as of this writing — the five whisper tiers plus VAD/label models came
from huggingface's `x-linked-etag` header; three of the five whisper tiers and the VAD/label
models additionally had a local copy whose `sha256sum` was cross-checked against that header
and agreed exactly (`ggml-base.en.bin`/`ggml-small.en.bin` had no local copy at pin time, so
their pin is `x-linked-etag` only). The sherpa-onnx GitHub release assets (parakeet bundle,
segmentation, embedding) are local-copy-only, since GitHub exposes no equivalent header. An
artifact added later without a local copy or a huggingface LFS entry is downloaded unverified
and logs the digest it computed, at `qInfo`, which is how the constant gets filled in. Capture
a digest either way:

- `sha256sum <file>` inside the models store, for a copy known to work; and/or
- `curl -sIL <url> | grep -i x-linked-etag` for a huggingface LFS object — the digest without
  downloading. GitHub release assets expose no equivalent header, so those are local-copy only.

Bumping a model name/URL means recomputing its pin in the same change; leaving the old digest
is the loud failure this pin exists to produce.

| what | pinned name/URL, where it lives | remote |
|---|---|---|
| whisper ggml tiers | `availableModels()` in `ModelManager.cpp` (file names) | `huggingface.co/ggerganov/whisper.cpp/resolve/main/%1` — floating `main` |
| VAD | `kVadModelFileName` = `"ggml-silero-v6.2.0.bin"` in `ModelManager.h` | `huggingface.co/ggml-org/whisper-vad` |
| auto-labeling | `kLabelModelFileName` = `"Qwen3-4B-Instruct-2507-Q4_K_M.gguf"` in `ModelManager.h` | `huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF` |
| Parakeet bundle | `kParakeetArchiveUrl` in `ModelManager.cpp`, ends `sherpa-onnx-nemo-parakeet-tdt-0.6b-v2-int8.tar.bz2` | sherpa-onnx `asr-models` release |
| diarization segmentation ×2 | `availableSegmentationModels()` in `ModelManager.cpp` | sherpa-onnx `speaker-segmentation-models` release |
| diarization embedding ×3 | `availableEmbeddingModels()` in `ModelManager.cpp` | sherpa-onnx `speaker-recongition-models` release |

**CUDA-12 compat wheels** (`CMakeLists.txt`, `suop_fetch_cuda12_wheel` calls,
downstream of the sherpa-onnx pin — see lockstep rules below): exact
version + SHA256 pinned per wheel:

| wheel | version | SHA256 |
|---|---|---|
| `nvidia_cublas_cu12` | 12.9.2.10 | `e4f53a8ca8c5d6e8c492d0d0a3d565ecb59a751b19cfdaa4f6da0ab2104c1702` |
| `nvidia_cuda_runtime_cu12` | 12.9.79 | `25bba2dfb01d48a9b59ca474a1ac43c6ebf7011f1b0b8cc44f54eb6ac48a96c3` |
| `nvidia_cufft_cu12` | 11.4.1.4 | `c67884f2a7d276b4b80eb56a79322a95df592ae5e765cf1243693365ccab4e28` |
| `nvidia_cuda_nvrtc_cu12` | 12.9.86 | `210cf05005a447e29214e9ce50851e83fc5f4358df8b453155d5e1918094dcb4` |

**Toolchain floors** (`CMakeLists.txt`, `find_package` calls): Qt 6.7;
`KF6WindowSystem` 6.19 specifically (`KWaylandExtras::xdgActivationToken()`'s
QFuture overload).

## Bump procedure, per dependency

**llama.cpp** — edit the submodule pin, `cd third_party/llama.cpp && git
checkout <new-commit>`, `git -C third_party/llama.cpp rev-parse HEAD` to
confirm. Bump this one FIRST — see lockstep rules. Build:
`make build`. Check: labeling still runs (`SUOP_ENABLE_LABELING`
build, run an auto-title/chapters task) — a ggml ABI break shows up as a
build failure, not a silent one.

**whisper.cpp** — same submodule-checkout procedure, done AFTER llama.cpp,
plus re-deriving `patches/whisper-vad-lstm-contiguous-input.patch` against the
new tree (`git apply --check` it, don't force it — the configure guard fails
loudly if it applies in neither direction, but can't tell you it landed on the
wrong hunk). Build, then transcribe a short real file with the default tier —
confirms the in-process link (`WhisperCppEngine`) still produces segments, not
just that it compiled — and run `tests/tst_vadchunking` with
`SUOP_TEST_VAD_MODEL` set, which is what actually exercises the patched VAD
graph.

**sherpa-onnx** — the pin is a fork (`suopuwu/sherpa-onnx`) sitting on top of
upstream tags, plus a configure-time patch
(`patches/sherpa-onnx-batch-segmentation.patch`) for what hasn't been folded
into the fork yet. A bump means:
1. Rebase the fork's own commits (currently "Use gpu better", "Update
   session.cc") onto the new upstream tag, push that as the new fork tip, and
   update the submodule pin to it.
2. Re-derive `patches/sherpa-onnx-batch-segmentation.patch` against the new
   tree — `git apply --check` it, don't force it. The configure-time guard in
   `CMakeLists.txt` (the `FATAL_ERROR` around `SUOP_SHERPA_ONNX_PATCH_CAN_APPLY`)
   catches a patch that no longer applies in either direction, but it can't
   tell you the patch is now applying to the WRONG lines — read the diff.
3. Re-check the onnxruntime CUDA SONAMEs (next entry) — a bumped sherpa-onnx
   can ship a different bundled onnxruntime.
Build: `make build` (the configure step fails loudly if the patch
doesn't apply). Check: run a diarization task and confirm the GPU is actually
used (see the CUDA-12 wheels check below) — CPU fallback is silent.

**CUDA-12 compat wheels** — only relevant if the sherpa-onnx bump changed its
fetched onnxruntime build. Run `ldd` on the built
`libonnxruntime_providers_cuda.so` and read off the exact SONAMEs it needs
(currently `libcublasLt.so.12`, `libcublas.so.12`, `libcudart.so.12`,
`libcufft.so.11`, `libnvrtc.so.12`). If the major version changed (e.g. a
CUDA-13 onnxruntime build), find the matching PyPI wheel version for each
`nvidia_*_cu1N` package, update both the version and the `URL_HASH SHA256=`
in the matching `suop_fetch_cuda12_wheel` call. Check: `ldd` the copied
`.so`s in `build-Release/lib/` against what the provider actually needs — don't trust
that "it downloaded" means "it matches".

**Downloaded models** — edit the constant (`kVadModelFileName` /
`kLabelModelFileName`) or the catalog entry (`availableModels()` /
`availableSegmentationModels()` / `availableEmbeddingModels()`) in
`ModelManager`. Delete any stale local copy under the vault's models dir so
`ensureModel()` re-downloads rather than reusing the old file under the old
name. Checks, per model:
- VAD: run with `SUOP_TEST_VAD_MODEL=<path>` on `tests/tst_vadchunking.cpp` —
  without it the relevant cases `QSKIP`, so a bad bump passes CI silently.
- Diarization segmentation: MUST come from sherpa-onnx's own packaged
  release, never a generic community ONNX export — see the doc comment on
  `downloadAndExtractFromTarBz2()` in `ModelManager.cpp` for why (missing
  baked-in ONNX metadata crashes at load, not a graceful failure).
- Everything else: download it via the Models page, then run the
  corresponding task once (transcribe / auto-label / diarize) end to end.

## The lockstep rules

- **llama.cpp leads whisper.cpp.** `CMakeLists.txt` `add_subdirectory`s
  llama.cpp first so the shared `ggml` target comes from its copy —
  whisper.cpp's own ggml sync lags upstream. Bump llama.cpp first; bumping
  whisper.cpp alone can compile against an older ggml than it expects.
- **A sherpa-onnx bump requires re-deriving the patch AND re-checking the
  onnxruntime CUDA SONAMEs** — both steps, not just the one the configure
  guard catches (see the sherpa-onnx procedure above).
- **A Flatpak manifest, when it exists, mirrors the whisper.cpp submodule
  commit** (DESIGN.md "Packaging") — a second site to update once that
  manifest is written; it isn't yet.

## Quiet-failure checklist

Degradations that never raise an error — the only way to catch them is the
per-dependency check above, not "it compiled" or "it launched":

- CUDA execution provider fails to `dlopen` (SONAME mismatch) → diarization
  silently falls back to CPU, still correct, just slow.
- A bad/incompatible VAD ggml → whisper's VAD gating silently degrades rather
  than erroring.
- A decorative/no-op patch → `git apply --reverse --check` can misreport
  "already applied" if it stops matching in both directions at once; the
  configure guard now catches the neither-direction case, but a patch
  quietly applying to the wrong hunk isn't caught by any automation — read
  the diff after a sherpa-onnx bump.
- An artifact with no pin recorded in `pinnedSha256()` is downloaded UNVERIFIED — it only logs
  its digest at `qInfo`. The pin-coverage assertion in `tests/tst_modelmanager.cpp` is what
  stops a newly added model from joining that set silently; today's catalog is fully pinned, so
  the exemption list there is empty.

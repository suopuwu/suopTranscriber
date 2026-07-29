# Third-party notices

suopTranscriber builds on the components below. License names were verified
against the LICENSE files present in this tree where noted; entries marked
"(verify)" could not be confirmed locally and should be checked upstream.

## Submodules (compiled and linked in-process)

| component | source | license |
|---|---|---|
| whisper.cpp | github.com/ggerganov/whisper.cpp (`third_party/whisper.cpp`) | MIT |
| llama.cpp | github.com/ggerganov/llama.cpp (`third_party/llama.cpp`) — optional, `SUOP_ENABLE_LABELING` | MIT |
| sherpa-onnx | **modified** — see below | Apache-2.0 |

**sherpa-onnx is modified.** It is built not from upstream
`k2-fsa/sherpa-onnx` but from the fork
`github.com/suopuwu/sherpa-onnx`, whose tip sits 2 commits above the
upstream `v1.13.4` tag ("Use gpu better", "Update session.cc"). In
addition, `patches/sherpa-onnx-batch-segmentation.patch` in this repository
is applied to the submodule at configure time. This notice is the change
statement required by Apache-2.0 section 4(b).

## Statically linked into the sherpa-onnx libraries

Fetched by sherpa-onnx's build at configure time:

| component | license |
|---|---|
| onnxruntime (Microsoft) | MIT |
| openfst | Apache-2.0 |
| kaldifst | Apache-2.0 |
| kaldi-decoder | Apache-2.0 |
| kaldi-native-fbank | Apache-2.0 |
| simple-sentencepiece | Apache-2.0 |
| kissfft | BSD-3-Clause |
| hclust-cpp | BSD-2-Clause |
| nlohmann/json | MIT |
| Eigen | MPL-2.0 |

hclust-cpp's license requires reproducing its copyright notice in binary
distributions:

> Copyright: fastcluster_dm.cpp & fastcluster_R_dm.cpp:
> (c) 2011 Daniel Müllner <http://danifold.net>;
> fastcluster.(h|cpp) & demo.cpp & plotresult.r:
> (c) 2018 Christoph Dalitz <http://www.hsnr.de/ipattern/>.
> All rights reserved.

## Dynamically linked

| component | license |
|---|---|
| Qt 6 (Core, Gui, Qml, Quick, QuickControls2, Multimedia, Core5Compat, DBus, Svg) | LGPL-3.0 (verify — system library, license per your Qt distribution) |
| KF6WindowSystem | LGPL-2.1-or-later (verify — system library) |

## Invoked as external programs

Not linked and not distributed with the app; provided by the user's system:

- ffmpeg (audio decode/resample, silence detection, combine)
- curl, tar (model downloads and extraction)
- PipeWire tools (`pw-record`, `pw-dump`) / PulseAudio tools (`parec`)
  for the recorder

## Optional CUDA build

When built with `SUOP_ENABLE_CUDA=ON`, the build fetches NVIDIA CUDA
redistributable libraries (cuBLAS, CUDA runtime, cuFFT, NVRTC) as wheels
from PyPI at configure time, SHA-256 pinned in `CMakeLists.txt`. These are
proprietary, governed by the NVIDIA EULA for CUDA redistributables. The
default Vulkan build (`-DSUOP_ENABLE_CUDA=OFF`, or no CUDA toolkit
installed) contains no proprietary components.

## Bundled assets

- **Material Symbols icons** (`assets/icons/material/`) — vendored from
  google/material-design-icons, Apache-2.0 (LICENSE ships in that
  directory). Modified: fills are stripped from the SVGs so the app's icon
  tinting owns color.

## Runtime-downloaded models

These are downloaded by the user at runtime, on demand — they are not
distributed with the app — and each is governed by its own upstream terms:

| model | source | license |
|---|---|---|
| whisper ggml tiers | huggingface.co/ggerganov/whisper.cpp | MIT (verify) |
| Silero VAD (ggml) | huggingface.co/ggml-org/whisper-vad | MIT (verify) |
| Qwen3-4B-Instruct GGUF (labeling) | huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF | Apache-2.0 (verify) |
| Parakeet-TDT 0.6B v2 (int8 ONNX bundle) | k2-fsa/sherpa-onnx release rehost | CC-BY-4.0 (verify) |
| pyannote segmentation 3.0 (int8 ONNX) | k2-fsa/sherpa-onnx release rehost | MIT code; upstream weights are gated on Hugging Face (verify) |
| Reverb diarization v1 (int8 ONNX) | k2-fsa/sherpa-onnx release rehost | Rev.ai license — **non-commercial** (verify) |
| WeSpeaker ResNet293-LM embedding | k2-fsa/sherpa-onnx release rehost | Apache-2.0 (verify) |
| 3D-Speaker CAM++ embedding | k2-fsa/sherpa-onnx release rehost | Apache-2.0 (verify) |
| NVIDIA NeMo TitaNet-large embedding | k2-fsa/sherpa-onnx release rehost | CC-BY-4.0 (verify) |

Attribution for Parakeet-TDT and TitaNet: these models were created by
NVIDIA and are used under the Creative Commons Attribution 4.0 license
(CC-BY-4.0).

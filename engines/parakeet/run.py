#!/usr/bin/env python3
"""Out-of-process ASR engine for NVIDIA Parakeet-TDT 0.6B v2 (NeMo).

Speaks suopTranscriber's JSONL wire protocol (DESIGN.md "Pluggable engines").
The app spawns this with:

    python3 run.py --audio chunk.wav --offset 120.0 --lang en

and reads newline-delimited JSON objects from stdout:

    {"type":"progress","seconds_done":..,"seconds_total":..}
    {"type":"segment","t0":..,"t1":..,"text":".."}
    {"type":"done"}
    {"type":"error","message":"..","recoverable":true|false}

CONTRACT NOTES
- Timestamps are CHUNK-RELATIVE. The app owns the offset merge — it adds the
  chunk's global position itself. The --offset arg is informational; do NOT add
  it to t0/t1 here (doing so double-applies it).
- --selftest checks only that dependencies are importable (fast, no model load)
  and exits 0/1. The app's capability detection uses this to gray the engine out
  with a reason ("dependencies not installed") when NeMo is absent, rather than
  failing at transcribe time.
- Any failure emits an {"type":"error"} line and exits non-zero. The app then
  degrades gracefully to "no segments" — a crash here never takes the app down
  (that isolation is the point of running out-of-process).

STATUS: NeMo is not installed in the development environment, so this runner is
shipped verified-by-contract (the C++ SubprocessEngine is tested against a fake
JSONL engine) and exercised for real by the user after `pip install nemo_toolkit[asr]`.
"""
import argparse
import importlib.util
import json
import sys

MODEL_NAME = "nvidia/parakeet-tdt-0.6b-v2"


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def deps_available():
    # find_spec does NOT import the (heavy) package — fast enough for a probe.
    return importlib.util.find_spec("nemo") is not None


def run(audio_path, lang):
    # Imported lazily so --selftest and a clean error path don't pay NeMo's
    # multi-second import cost, and so a missing dependency is reported as a
    # protocol error rather than a Python traceback on stderr.
    import nemo.collections.asr as nemo_asr  # noqa: WPS433 (intentional local import)

    model = nemo_asr.models.ASRModel.from_pretrained(model_name=MODEL_NAME)

    # timestamps=True yields word/segment/char level timing; we use segment
    # granularity (sentence-level is all this app consumes — no word timing).
    outputs = model.transcribe([audio_path], timestamps=True)
    if not outputs:
        emit({"type": "done"})
        return 0

    result = outputs[0]
    segments = []
    timestamp_data = getattr(result, "timestamp", None)
    if isinstance(timestamp_data, dict):
        segments = timestamp_data.get("segment", []) or []

    if segments:
        total = segments[-1].get("end", 0.0)
        for i, seg in enumerate(segments):
            emit({
                "type": "segment",
                # Chunk-relative — NeMo's timings are already relative to this
                # clip's start, which is exactly what the app wants; do not add
                # the --offset here.
                "t0": float(seg.get("start", 0.0)),
                "t1": float(seg.get("end", 0.0)),
                "text": (seg.get("segment") or seg.get("text") or "").strip(),
            })
            emit({"type": "progress", "seconds_done": float(seg.get("end", 0.0)),
                  "seconds_total": float(total)})
    else:
        # No per-segment timing came back — emit the whole utterance as one
        # segment so the app still gets text (timings default to 0/0).
        text = getattr(result, "text", "") or ""
        if text.strip():
            emit({"type": "segment", "t0": 0.0, "t1": 0.0, "text": text.strip()})

    emit({"type": "done"})
    return 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--selftest", action="store_true")
    parser.add_argument("--audio")
    parser.add_argument("--offset", default="0.0")  # informational; see module docstring
    parser.add_argument("--lang", default="en")
    args = parser.parse_args()

    if args.selftest:
        return 0 if deps_available() else 1

    if not args.audio:
        emit({"type": "error", "message": "no --audio given", "recoverable": False})
        return 2

    if not deps_available():
        emit({"type": "error", "message": "NeMo is not installed (pip install nemo_toolkit[asr])",
              "recoverable": False})
        return 1

    try:
        return run(args.audio, args.lang)
    except Exception as exc:  # noqa: BLE001 — any failure must degrade, not crash the app
        emit({"type": "error", "message": "%s: %s" % (type(exc).__name__, exc), "recoverable": False})
        return 1


if __name__ == "__main__":
    sys.exit(main())

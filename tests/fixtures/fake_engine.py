#!/usr/bin/env python3
"""Fake out-of-process ASR engine for tst_subprocessengine — NO NeMo, no models.

Speaks the DESIGN.md JSONL wire protocol so SubprocessEngine's parsing, callback
dispatch, abort handling, and graceful-degradation paths can be exercised with
zero heavyweight dependencies. The behavior is chosen by a --mode arg baked into
the test's manifest `command`; SubprocessEngine appends --audio/--offset/--lang
after it, which we accept and (mostly) ignore.

Segment timestamps emitted here are deliberately CHUNK-RELATIVE and small
(0.0-2.0s) even though the test passes a large --offset: that lets the test prove
SubprocessEngine does NOT pre-apply the offset (the app owns the merge).
"""
import argparse
import json
import os
import sys
import time


def emit(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--mode", default="normal")
    parser.add_argument("--audio", default="")
    parser.add_argument("--offset", default="0.0")
    parser.add_argument("--lang", default="auto")
    args = parser.parse_args()

    if args.mode == "normal":
        emit({"type": "progress", "seconds_done": 1.0, "seconds_total": 2.0})
        emit({"type": "segment", "t0": 0.0, "t1": 1.0, "text": "hello world"})
        emit({"type": "segment", "t0": 1.0, "t1": 2.0, "text": "second segment"})
        emit({"type": "progress", "seconds_done": 2.0, "seconds_total": 2.0})
        emit({"type": "done"})
        return 0

    if args.mode == "fatal_error":
        emit({"type": "error", "message": "simulated fatal failure", "recoverable": False})
        return 1

    if args.mode == "recoverable_then_done":
        emit({"type": "segment", "t0": 0.0, "t1": 1.0, "text": "before hiccup"})
        emit({"type": "error", "message": "transient", "recoverable": True})
        emit({"type": "segment", "t0": 1.0, "t1": 2.0, "text": "after hiccup"})
        emit({"type": "done"})
        return 0

    if args.mode == "nonzero_exit":
        # One segment, then exit non-zero with NO done line — a crashy engine.
        emit({"type": "segment", "t0": 0.0, "t1": 1.0, "text": "partial before crash"})
        return 3

    if args.mode == "split_lines":
        # Write a segment message in two stdout writes with the newline only at
        # the very end, so SubprocessEngine must buffer the partial line.
        first = json.dumps({"type": "segment", "t0": 0.0, "t1": 1.0, "text": "split line"})
        half = len(first) // 2
        sys.stdout.write(first[:half])
        sys.stdout.flush()
        time.sleep(0.15)
        sys.stdout.write(first[half:] + "\n")
        sys.stdout.flush()
        emit({"type": "done"})
        return 0

    if args.mode == "slow":
        # Stream a segment every 100ms indefinitely so the test can abort us.
        i = 0
        while True:
            emit({"type": "segment", "t0": float(i), "t1": float(i + 1), "text": "tick %d" % i})
            i += 1
            time.sleep(0.1)

    emit({"type": "error", "message": "unknown mode %s" % args.mode, "recoverable": False})
    return 1


if __name__ == "__main__":
    sys.exit(main())

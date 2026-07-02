#pragma once

#include <string>

// Sentence-level only — no word timestamps. See DESIGN.md "Sentence highlighting".
struct Segment {
    double t0 = 0.0;
    double t1 = 0.0;
    std::string text;
};

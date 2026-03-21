#pragma once
#include <string>
#include <vector>

// One entry in a JSONL overlay file.
// roi   : region of interest in image-pixel coordinates
// dx,dy : displacement vector in image pixels
// angle : rotation in radians (positive = counter-clockwise)
// label : optional display string
struct roi_entry {
    int   x = 0, y = 0, w = 0, h = 0;
    float dx    = 0.0f;
    float dy    = 0.0f;
    float angle = 0.0f;
    std::string label;
};

#pragma once
#include <string>
#include <vector>

// One overlay entry with pixel-space coordinates.
// x,y,w,h : rectangle in image-pixel coordinates (computed from grid index * tile size)
// dx,dy   : displacement vector in image pixels
// angle   : rotation in radians (positive = counter-clockwise)
// label   : display string (inherited from group label)
struct roi_entry {
    int   x = 0, y = 0, w = 0, h = 0;
    float dx    = 0.0f;
    float dy    = 0.0f;
    float angle = 0.0f;
    std::string label;
};

// A group of overlay entries sharing the same tile size and label.
// entries[i].x == col * tile_w, entries[i].y == row * tile_h
struct roi_group {
    std::string            label;
    int                    tile_w = 0, tile_h = 0;
    std::vector<roi_entry> entries;
};

namespace overlay_io {

// Load overlay JSON file. Returns true if the file was opened successfully.
// Expected format:
//   {
//     "version": 1,
//     "groups": [
//       {
//         "label": "foo",
//         "size": { "w": 476, "h": 714 },
//         "entries": [
//           { "x": 0, "y": 0, "dx": 0.0, "dy": 0.0, "angle": 0.0 },
//           ...
//         ]
//       }
//     ]
//   }
bool load(const std::string& path, std::vector<roi_group>& out);

// Convenience: flatten all groups into a single roi_entry list.
bool load_flat(const std::string& path, std::vector<roi_entry>& out);

} // namespace overlay_io

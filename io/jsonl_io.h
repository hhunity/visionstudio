#pragma once
#include "util/roi_data.h"
#include <string>
#include <vector>

namespace jsonl_io {

// Load ROI entries from a JSONL file (one JSON object per line).
// Expected line format:
//   {"roi":{"x":50,"y":100,"w":100,"h":100},"info":{"dx":5,"dy":-4,"angle":-0.1},"label":"A"}
// Returns true if the file was opened; malformed lines are silently skipped.
bool load(const std::string& path, std::vector<roi_entry>& out);

} // namespace jsonl_io

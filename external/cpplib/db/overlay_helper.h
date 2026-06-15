#pragma once
#include <string>

#if defined(_WIN32)
#  ifdef DB_BUILD_DLL
#    define DB_API __declspec(dllexport)
#  else
#    define DB_API __declspec(dllimport)
#  endif
#else
#  define DB_API
#endif

// ---------------------------------------------------------------------------
// Overlay file conversion helpers
// ---------------------------------------------------------------------------
//
// JSONL format (one JSON object per line):
//   {"roi":{"x":px,"y":py,"w":tw,"h":th},
//    "info":{"dx":...,"dy":...,"angle":...},
//    "label":"c0r0"}
//
// JSON format (grouped by tile size):
//   {
//     "version": 1,
//     "groups": [
//       {
//         "label": "<stem of src file>",
//         "size": {"w": tw, "h": th},
//         "entries": [
//           {"x": col, "y": row, "dx": ..., "dy": ..., "angle": ...},
//           ...
//         ]
//       }
//     ]
//   }
//
// Pixel coordinates are converted to grid indices:
//   col = roi.x / tile_w,  row = roi.y / tile_h
// ---------------------------------------------------------------------------

// Convert an overlay JSONL file to the grouped JSON format.
// Entries are grouped by tile size. The group label defaults to the file stem of src.
// Returns true on success.
DB_API bool convert_overlay_jsonl_to_json(const std::string& src, const std::string& dst);

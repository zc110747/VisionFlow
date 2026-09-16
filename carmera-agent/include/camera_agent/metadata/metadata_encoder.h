#pragma once

// Hand-written JSON encoder (no third-party JSON library).
//
// spec 15: frame_id / timestamp are COPIED VERBATIM from the AIFrameResult.
// Regenerating them here would silently break the video <-> AI frame alignment.

#include <string>

#include "camera_agent/ai/ai_types.h"
#include "camera_agent/metadata/metadata_types.h"

namespace ca {

// {"version":1,"camera_id":"...","frame_id":N,"timestamp":N,"video_width":N,
//  "video_height":N,"objects":[{"class":"person","confidence":0.93,
//  "track_id":17,"bbox":[x1,y1,x2,y2]}]}
//
// bbox is clamped into the original video frame (spec 5) and expressed as
// integers in original video pixels.
std::string encode_frame_metadata(const AIFrameResult& r, const MetadataConfig& cfg);

// {"version":1,"type":"status","camera_id":"...","timestamp":N,
//  "ai":{"enable":true,"running":true,"fps":5.0,"model":"...","tracker":"...",
//        "last_frame_id":N,"last_timestamp":N,"processed":N},"wall_clock":N}
//
// `timestamp` repeats the AI video time base; `wall_clock` is epoch ms and is
// only meant for server-side freshness checks - it is never used as a frame time.
std::string encode_status_metadata(const AIStatusInfo& s, const MetadataConfig& cfg);

} // namespace ca

#pragma once

namespace devmode {

enum class CandidateSourceContext {
    AnchorNonLight,
    AnchorLight,
    OvalPoint,
    OvalCenter,
    FloorBox,
};

inline const char* candidate_source_context_name(CandidateSourceContext context) {
    switch (context) {
        case CandidateSourceContext::AnchorNonLight:
            return "anchor_non_light";
        case CandidateSourceContext::AnchorLight:
            return "anchor_light";
        case CandidateSourceContext::OvalPoint:
            return "oval_point";
        case CandidateSourceContext::OvalCenter:
            return "oval_center";
        case CandidateSourceContext::FloorBox:
            return "floor_box";
        default:
            return "unknown";
    }
}

}  // namespace devmode

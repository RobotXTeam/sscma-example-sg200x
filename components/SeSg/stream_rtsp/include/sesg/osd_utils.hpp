#pragma once

#include <algorithm>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include "sesg/stream_rtsp.h"
#include "font5x7.hpp"

namespace sesg {
namespace osd {

// Return a stable distinct color for a given class ID
static inline uint32_t color_for_class(int cls) {
    static const uint32_t kPalette[8] = {
        0xFFFF3030u, 0xFF30FF30u, 0xFF3080FFu, 0xFFFFD030u,
        0xFFFF30FFu, 0xFF30FFFFu, 0xFFFF8030u, 0xFF80FF30u,
    };
    if (cls < 0) cls = 0;
    return kPalette[cls % 8];
}

// Append a filled rectangle to the overlay list
static inline void push_filled_px(std::vector<sesg::stream_rtsp::OverlayRect>& out,
                                  int px, int py, int pw, int ph, uint32_t argb,
                                  uint32_t overlay_w, uint32_t overlay_h) {
    if (pw <= 0 || ph <= 0) return;
    if (px >= (int)overlay_w || py >= (int)overlay_h) return;
    if (px + pw <= 0 || py + ph <= 0) return;
    sesg::stream_rtsp::OverlayRect r;
    r.x = (float)px / (float)overlay_w;
    r.y = (float)py / (float)overlay_h;
    r.w = (float)pw / (float)overlay_w;
    r.h = (float)ph / (float)overlay_h;
    r.argb = argb;
    r.thickness = (uint16_t)std::max(1, std::min(pw, ph));
    out.push_back(r);
}

// Draw text on the overlay using the 5x7 font
static inline int draw_text_px(std::vector<sesg::stream_rtsp::OverlayRect>& out, int px, int py,
                               const char* text, uint32_t argb, int scale,
                               uint32_t overlay_w, uint32_t overlay_h) {
    int cx = px;
    for (const char* p = text; *p; ++p) {
        const uint8_t* cols = font5x7::columns(*p);
        for (int c = 0; c < 5; ++c) {
            for (int row = 0; row < 7; ++row) {
                if (cols[c] & (1u << row)) {
                    push_filled_px(out, cx + c * scale, py + row * scale, scale, scale, argb, overlay_w, overlay_h);
                }
            }
        }
        cx += 6 * scale;
    }
    return cx - px;
}

// Generic template to convert YOLO detections to OSD rectangles
template<typename DetectionT, typename LabelFunc>
static inline void build_overlay_rects(const std::vector<DetectionT>& dets,
                                       std::vector<sesg::stream_rtsp::OverlayRect>& out,
                                       LabelFunc get_label,
                                       uint32_t overlay_w = 1280,
                                       uint32_t overlay_h = 720,
                                       float active_y_start = 140.0f / 640.0f,
                                       float active_h_ratio = 360.0f / 640.0f) {
    out.clear();
    for (const auto& d : dets) {
        float original_y = (d.y - active_y_start) / active_h_ratio;
        float original_h = d.h / active_h_ratio;

        const float x1 = d.x - d.w * 0.5f;
        const float y1 = original_y - original_h * 0.5f;

        sesg::stream_rtsp::OverlayRect r;
        r.x = std::clamp(x1, 0.0f, 1.0f);
        r.y = std::clamp(y1, 0.0f, 1.0f);
        r.w = std::clamp(d.w, 0.0f, 1.0f);
        r.h = std::clamp(original_h, 0.0f, 1.0f);
        const uint32_t col = color_for_class(d.target);
        r.argb = col;
        r.thickness = 3;

        if (r.w <= 0.0f || r.h <= 0.0f) continue;
        if (r.x >= 1.0f || r.y >= 1.0f) continue;
        if (r.x + r.w <= 0.0f || r.y + r.h <= 0.0f) continue;
        out.push_back(r);

        char label[40];
        int pct = (int)(d.score * 100.0f + 0.5f);
        if (pct > 99) pct = 99;
        snprintf(label, sizeof(label), "%s %d", get_label(d.target), pct);

        const int scale = 2;
        const int text_h = 7 * scale;
        int tx = (int)(r.x * overlay_w) + 1;
        int ty = (int)(r.y * overlay_h) - text_h - 2;
        if (ty < 0) ty = (int)(r.y * overlay_h) + 2;

        int label_w = (int)strlen(label) * 6 * scale;
        push_filled_px(out, tx - 1, ty - 1, label_w + 2, text_h + 2, 0xFF101010u, overlay_w, overlay_h);
        draw_text_px(out, tx, ty, label, col, scale, overlay_w, overlay_h);
    }
}

} // namespace osd
} // namespace sesg

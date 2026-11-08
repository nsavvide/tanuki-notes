#include "text.hpp"

// Private copy of stb_truetype for vector glyphs. NanoVG's copy routes its
// allocations through fontstash, so it can't be called directly.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>


namespace text {

namespace {

constexpr double ref_px = 100;    // measuring size
constexpr double vector_px = 120; // above this, draw outlines

// The fonts live inside the binary.
const unsigned char virgil_ttf[] = {
#embed "../assets/fonts/Virgil.ttf"
};
const unsigned char inter_ttf[] = {
#embed "../assets/fonts/Inter-Regular.ttf"
};
const unsigned char inter_medium_ttf[] = {
#embed "../assets/fonts/Inter-Medium.ttf"
};

struct VecFont {
    stbtt_fontinfo info{};
    double ascent = 0; // per px of font size
};

int nvg_font[3] = {-1, -1, -1};
VecFont vec_font[3];

bool load(VecFont& f, const unsigned char* data) {
    if (!stbtt_InitFont(&f.info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
        return false;
    }
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f.info, &asc, &desc, &gap);
    f.ascent = asc * stbtt_ScaleForMappingEmToPixels(&f.info, 1);
    return true;
}

// UTF-8 decode one codepoint, advancing `i`.
unsigned next_codepoint(const std::string& s, size_t& i) {
    unsigned char c = s[i++];
    if (c < 0x80) {
        return c;
    }
    int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
    unsigned cp = c & (0x3f >> extra);
    while (extra-- && i < s.size()) {
        cp = (cp << 6) | (s[i++] & 0x3f);
    }
    return cp;
}

void draw_vector(NVGcontext* vg, Font f, const std::string& s, double x,
                 double baseline, double px, NVGcolor color, Rect viewport) {
    nvgBeginPath(vg);
    double pen = x;
    int prev_glyph = 0;
    const VecFont* prev_font = nullptr;
    for (size_t i = 0; i < s.size();) {
        unsigned cp = next_codepoint(s, i);
        const VecFont* vf = &vec_font[f];
        int g = stbtt_FindGlyphIndex(&vf->info, int(cp));
        if (g == 0 && f == Hand) { // fall back to Inter for missing glyphs
            vf = &vec_font[Ui];
            g = stbtt_FindGlyphIndex(&vf->info, int(cp));
        }
        double scale = stbtt_ScaleForMappingEmToPixels(&vf->info, float(px));
        if (prev_font == vf && prev_glyph) {
            pen += stbtt_GetGlyphKernAdvance(&vf->info, prev_glyph, g) * scale;
        }
        int adv, lsb;
        stbtt_GetGlyphHMetrics(&vf->info, g, &adv, &lsb);

        int x0, y0, x1, y1;
        Rect box;
        if (stbtt_GetGlyphBox(&vf->info, g, &x0, &y0, &x1, &y1)) {
            box = {pen + x0 * scale, baseline - y1 * scale, (x1 - x0) * scale,
                   (y1 - y0) * scale};
        }
        if (box.w > 0 && viewport.intersects(box)) {
            stbtt_vertex* v;
            int n = stbtt_GetGlyphShape(&vf->info, g, &v);
            // Group into contours; the biggest decides which winding is
            // "solid", the opposite winding becomes a hole.
            std::vector<std::pair<int, int>> contours; // [start, end)
            std::vector<double> areas;
            for (int k = 0; k < n; ++k) {
                if (v[k].type == STBTT_vmove) {
                    contours.push_back({k, k + 1});
                    areas.push_back(0);
                } else if (!contours.empty()) {
                    contours.back().second = k + 1;
                    const auto& p = v[k - 1];
                    areas.back() += double(p.x) * v[k].y - double(v[k].x) * p.y;
                }
            }
            double biggest = 0;
            for (double a : areas) {
                if (std::abs(a) > std::abs(biggest)) {
                    biggest = a;
                }
            }
            auto X = [&](double fx) { return float(pen + fx * scale); };
            auto Y = [&](double fy) { return float(baseline - fy * scale); };
            for (size_t c = 0; c < contours.size(); ++c) {
                for (int k = contours[c].first; k < contours[c].second; ++k) {
                    const auto& p = v[k];
                    switch (p.type) {
                    case STBTT_vmove:
                        nvgMoveTo(vg, X(p.x), Y(p.y));
                        break;
                    case STBTT_vline:
                        nvgLineTo(vg, X(p.x), Y(p.y));
                        break;
                    case STBTT_vcurve:
                        nvgQuadTo(vg, X(p.cx), Y(p.cy), X(p.x), Y(p.y));
                        break;
                    case STBTT_vcubic:
                        nvgBezierTo(vg, X(p.cx), Y(p.cy), X(p.cx1), Y(p.cy1),
                                    X(p.x), Y(p.y));
                        break;
                    }
                }
                nvgClosePath(vg);
                bool solid = (areas[c] >= 0) == (biggest >= 0);
                nvgPathWinding(vg, solid ? NVG_SOLID : NVG_HOLE);
            }
            stbtt_FreeShape(&vf->info, v);
        }
        pen += adv * scale;
        prev_glyph = g;
        prev_font = vf;
        if (pen > viewport.right()) {
            break;
        }
    }
    nvgFillColor(vg, color);
    nvgFill(vg);
}

} // namespace

bool init(NVGcontext* vg) {
    struct {
        const char* name;
        const unsigned char* data;
        int size;
    } fonts[3] = {{"hand", virgil_ttf, sizeof virgil_ttf},
                  {"ui", inter_ttf, sizeof inter_ttf},
                  {"ui-medium", inter_medium_ttf, sizeof inter_medium_ttf}};
    for (int i = 0; i < 3; ++i) {
        // freeData = 0: NanoVG only reads the static array.
        nvg_font[i] = nvgCreateFontMem(vg, fonts[i].name,
                                       const_cast<unsigned char*>(fonts[i].data),
                                       fonts[i].size, 0);
        if (nvg_font[i] < 0 || !load(vec_font[i], fonts[i].data)) {
            return false;
        }
    }
    nvgAddFallbackFontId(vg, nvg_font[Hand], nvg_font[Ui]);
    return true;
}

double measure(NVGcontext* vg, Font f, const std::string& s, double px) {
    if (s.empty()) {
        return 0;
    }
    nvgFontFaceId(vg, nvg_font[f]);
    nvgFontSize(vg, float(ref_px));
    float adv = nvgTextBounds(vg, 0, 0, s.data(), s.data() + s.size(), nullptr);
    return adv * px / ref_px;
}

std::vector<Span> wrap(NVGcontext* vg, Font f, const std::string& s, double px,
                       double max_w) {
    std::vector<Span> out;
    size_t start = 0;
    while (true) {
        size_t nl = s.find('\n', start);
        size_t para_end = nl == std::string::npos ? s.size() : nl;
        auto width = [&](size_t b, size_t e) {
            return measure(vg, f, s.substr(b, e - b), px);
        };
        if (max_w <= 0 || width(start, para_end) <= max_w) {
            out.push_back({start, para_end});
        } else {
            // Greedy word wrap; the space at a break belongs to no line.
            size_t line_begin = start, line_end = start;
            size_t i = start;
            while (true) {
                size_t sp = s.find(' ', i);
                size_t word_end = (sp == std::string::npos || sp > para_end)
                                      ? para_end
                                      : sp;
                if (line_end > line_begin &&
                    width(line_begin, word_end) > max_w) {
                    out.push_back({line_begin, line_end});
                    line_begin = i;
                }
                line_end = word_end;
                if (word_end == para_end) {
                    break;
                }
                i = word_end + 1;
            }
            out.push_back({line_begin, line_end});
        }
        if (nl == std::string::npos) {
            break;
        }
        start = nl + 1;
    }
    return out;
}

void draw_line(NVGcontext* vg, Font f, const std::string& s, double x,
               double y_top, double px, NVGcolor color, Rect viewport) {
    if (s.empty()) {
        return;
    }
    Rect box{x, y_top, measure(vg, f, s, px), px * line_height};
    if (!viewport.intersects(box)) {
        return;
    }
    double baseline = y_top + (line_height - 1) / 2 * px +
                      vec_font[f].ascent * px;
    if (px < 3) { // too small to read: a faint bar keeps the layout visible
        nvgBeginPath(vg);
        nvgRect(vg, float(x), float(y_top + px * 0.4), float(box.w),
                float(std::max(px * 0.4, 0.5)));
        nvgFillColor(vg, nvgTransRGBAf(color, 0.35f));
        nvgFill(vg);
    } else if (px <= vector_px) {
        nvgFontFaceId(vg, nvg_font[f]);
        nvgFontSize(vg, float(px));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        nvgFillColor(vg, color);
        nvgText(vg, float(x), float(baseline), s.data(), s.data() + s.size());
    } else {
        draw_vector(vg, f, s, x, baseline, px, color, viewport);
    }
}

} // namespace text

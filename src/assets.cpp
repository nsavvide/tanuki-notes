#include "assets.hpp"
#include "node.hpp"

#include <nanovg.h>
#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>

#include <fstream>
#include <vector>

namespace fs = std::filesystem;

constexpr long evict_after_frames = 600;

Assets::Assets(NVGcontext* vg, fs::path map_dir)
    : vg_(vg), dir_(std::move(map_dir)) {}

Assets::~Assets() {
    for (auto& [k, t] : images_) {
        nvgDeleteImage(vg_, t.handle);
    }
    for (auto& [k, t] : pages_) {
        nvgDeleteImage(vg_, t.handle);
    }
}

const Texture* Assets::image(const std::string& asset) {
    auto it = images_.find(asset);
    if (it == images_.end()) {
        Texture t;
        t.handle = nvgCreateImage(vg_, (dir_ / asset).c_str(),
                                  NVG_IMAGE_GENERATE_MIPMAPS);
        if (t.handle) {
            nvgImageSize(vg_, t.handle, &t.w, &t.h);
        }
        it = images_.emplace(asset, t).first;
    }
    it->second.last_used = frame_;
    return it->second.handle ? &it->second : nullptr;
}

poppler::document* Assets::doc(const std::string& asset) {
    auto it = docs_.find(asset);
    if (it == docs_.end()) {
        std::unique_ptr<poppler::document> d(
            poppler::document::load_from_file((dir_ / asset).string()));
        it = docs_.emplace(asset, std::move(d)).first;
    }
    return it->second.get();
}

int Assets::pdf_page_count(const std::string& asset) {
    poppler::document* d = doc(asset);
    return d ? d->pages() : 0;
}

const std::string& Assets::pdf_page_text(const std::string& asset, int page) {
    auto key = std::make_pair(asset, page);
    auto it = page_text_.find(key);
    if (it == page_text_.end()) {
        std::string text;
        poppler::document* d = doc(asset);
        if (d && page >= 0 && page < d->pages()) {
            std::unique_ptr<poppler::page> p(d->create_page(page));
            auto bytes = p->text().to_utf8();
            text.assign(bytes.begin(), bytes.end());
        }
        it = page_text_.emplace(key, std::move(text)).first;
    }
    return it->second;
}

std::pair<double, double> Assets::pdf_page_size(const std::string& asset,
                                                int page) {
    poppler::document* d = doc(asset);
    if (!d || page < 0 || page >= d->pages()) {
        return {0, 0};
    }
    std::unique_ptr<poppler::page> p(d->create_page(page));
    poppler::rectf r = p->page_rect();
    return {r.width(), r.height()};
}

const Texture* Assets::pdf_page(const std::string& asset, int page,
                                double want_px) {
    // Resolution buckets: 256, 512, ... 4096 px wide.
    int bucket = 256;
    while (bucket < want_px && bucket < 4096) {
        bucket *= 2;
    }
    auto key = std::make_tuple(asset, page, bucket);
    auto it = pages_.find(key);
    if (it == pages_.end() && rendered_this_frame_) {
        deferred_next_ = true;
    }
    if (it == pages_.end() && !rendered_this_frame_) {
        rendered_this_frame_ = true;
        Texture t;
        poppler::document* d = doc(asset);
        if (d && page < d->pages()) {
            std::unique_ptr<poppler::page> p(d->create_page(page));
            double dpi = bucket / (p->page_rect().width() / 72.0);
            poppler::page_renderer r;
            r.set_render_hint(poppler::page_renderer::antialiasing, true);
            r.set_render_hint(poppler::page_renderer::text_antialiasing, true);
            r.set_paper_color(0xffffffff);
            poppler::image img = r.render_page(p.get(), dpi, dpi);
            if (img.is_valid() && img.format() == poppler::image::format_argb32) {
                // ARGB32 in memory is B,G,R,A (little endian) -> RGBA.
                int w = img.width(), h = img.height();
                std::vector<unsigned char> rgba(size_t(w) * h * 4);
                for (int y = 0; y < h; ++y) {
                    const unsigned char* row =
                        (const unsigned char*)img.const_data() +
                        size_t(y) * img.bytes_per_row();
                    for (int x = 0; x < w; ++x) {
                        unsigned char* o = &rgba[(size_t(y) * w + x) * 4];
                        o[0] = row[x * 4 + 2];
                        o[1] = row[x * 4 + 1];
                        o[2] = row[x * 4 + 0];
                        o[3] = row[x * 4 + 3];
                    }
                }
                t.handle = nvgCreateImageRGBA(vg_, w, h,
                                              NVG_IMAGE_GENERATE_MIPMAPS |
                                                  NVG_IMAGE_PREMULTIPLIED,
                                              rgba.data());
                t.w = w;
                t.h = h;
            }
        }
        it = pages_.emplace(key, t).first;
    }
    if (it != pages_.end() && it->second.handle) {
        it->second.last_used = frame_;
        return &it->second;
    }
    // Not rendered yet: fall back to the closest lower resolution.
    for (int b = bucket / 2; b >= 256; b /= 2) {
        auto lower = pages_.find(std::make_tuple(asset, page, b));
        if (lower != pages_.end() && lower->second.handle) {
            lower->second.last_used = frame_;
            return &lower->second;
        }
    }
    return nullptr;
}

std::string Assets::import(const fs::path& source) {
    // Keep a readable, searchable name: assets/<name>-<short id>.<ext>
    std::string stem;
    for (char c : source.stem().string()) {
        stem += std::isalnum((unsigned char)c) || c == '-' || c == '_' ? c : '-';
    }
    std::string ext = source.extension().string();
    std::string rel = "assets/" + stem.substr(0, 40) + "-" + new_id().substr(0, 6) + ext;
    fs::create_directories(dir_ / "assets");
    fs::copy_file(source, dir_ / rel);
    return rel;
}

std::string Assets::import_bytes(const void* data, size_t size,
                                 const std::string& ext) {
    std::string rel = "assets/" + new_id() + ext;
    fs::create_directories(dir_ / "assets");
    std::ofstream out(dir_ / rel, std::ios::binary);
    out.write((const char*)data, std::streamsize(size));
    return rel;
}

void Assets::end_frame() {
    ++frame_;
    rendered_this_frame_ = false;
    deferred_ = deferred_next_;
    deferred_next_ = false;
    std::erase_if(images_, [&](auto& kv) {
        if (frame_ - kv.second.last_used > evict_after_frames) {
            if (kv.second.handle) {
                nvgDeleteImage(vg_, kv.second.handle);
            }
            return true;
        }
        return false;
    });
    std::erase_if(pages_, [&](auto& kv) {
        if (frame_ - kv.second.last_used > evict_after_frames) {
            if (kv.second.handle) {
                nvgDeleteImage(vg_, kv.second.handle);
            }
            return true;
        }
        return false;
    });
}

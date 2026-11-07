#pragma once

// GPU textures for images and PDF pages, loaded on demand and evicted when
// unused. PDF pages are re-rendered at higher resolution as you zoom in.

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>

struct NVGcontext;
namespace poppler {
class document;
}

struct Texture {
    int handle = 0;
    int w = 0, h = 0;
    long last_used = 0;
};

class Assets {
  public:
    Assets(NVGcontext* vg, std::filesystem::path map_dir);
    ~Assets();

    // nullptr if the file can't be loaded.
    const Texture* image(const std::string& asset);

    // Best available texture for drawing the page `want_px` wide. Renders at
    // most one new resolution per frame; until then returns a lower one.
    const Texture* pdf_page(const std::string& asset, int page, double want_px);

    int pdf_page_count(const std::string& asset);
    // Page size in points; {0,0} if unknown.
    std::pair<double, double> pdf_page_size(const std::string& asset, int page);

    // Copies `source` into <map>/assets/ and returns the relative path.
    std::string import(const std::filesystem::path& source);
    // Stores raw bytes (e.g. a pasted PNG) the same way.
    std::string import_bytes(const void* data, size_t size,
                             const std::string& ext);

    void end_frame(); // evicts textures unused for a while

    // A PDF page is waiting for its turn to render.
    bool busy() const { return deferred_; }

  private:
    poppler::document* doc(const std::string& asset);

    NVGcontext* vg_;
    std::filesystem::path dir_;
    long frame_ = 0;
    bool rendered_this_frame_ = false;
    bool deferred_ = false, deferred_next_ = false;
    std::map<std::string, Texture> images_;
    std::map<std::tuple<std::string, int, int>, Texture> pages_;
    std::map<std::string, std::unique_ptr<poppler::document>> docs_;
};

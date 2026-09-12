#pragma once

#include <cstdint>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

struct Color {
    std::uint8_t r = 0, g = 0, b = 0, a = 255;
    constexpr Color() = default;
    constexpr Color(std::uint8_t red, std::uint8_t green, std::uint8_t blue, std::uint8_t alpha = 255)
        : r(red), g(green), b(blue), a(alpha) {}
};

// Retained path object, equivalent to the browser's Path2D.
class Path2D {
public:
    enum class Command : unsigned char { Move, Line, Quadratic, Bezier, Arc, ArcTo, Ellipse, Rect, RoundRect, Close };
    struct Segment { Command command; float v[8]{}; bool counterClockwise = false; };
    void moveTo(float x, float y); void lineTo(float x, float y); void closePath();
    void quadraticCurveTo(float cpx, float cpy, float x, float y);
    void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void arc(float x, float y, float radius, float startAngle, float endAngle, bool counterClockwise = false);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void ellipse(float x, float y, float radiusX, float radiusY, float rotation, float startAngle, float endAngle, bool counterClockwise = false);
    void rect(float x, float y, float width, float height); void roundRect(float x, float y, float width, float height, float radius);
    void addPath(const Path2D& other);
    void clear() { segments_.clear(); glyphOutlines_ = false; touch(); }
    bool empty() const { return segments_.empty(); }
    const std::vector<Segment>& segments() const { return segments_; }
    // Mutable access, so the revision moves whether or not the caller writes.
    // Over-invalidating costs a rebuild; under-invalidating draws stale
    // geometry, which is not a trade worth making for one increment.
    std::vector<Segment>& segments() { touch(); return segments_; }

    // Identity for the browser build's retained-path cache. A path that has
    // not changed since it was last drawn is named to the page by key alone,
    // so neither side rebuilds it. Keys are unique for the life of the process
    // and never reused, so a cache entry outliving its path is only ever
    // wasted memory -- never another path's geometry.
    std::uint32_t cacheKey() const;
    std::uint32_t revision() const { return revision_; }

    Path2D() = default;
    ~Path2D() = default;
    // A copy is a different path and takes its own key; a moved-from path is
    // emptied, so its revision moves too or its old key would still name the
    // geometry it no longer has.
    Path2D(const Path2D& other) : segments_(other.segments_), glyphOutlines_(other.glyphOutlines_) {}
    Path2D(Path2D&& other) noexcept
        : segments_(std::move(other.segments_)), glyphOutlines_(other.glyphOutlines_) { other.touch(); }
    Path2D& operator=(const Path2D& other) {
        if (this != &other) { segments_ = other.segments_; glyphOutlines_ = other.glyphOutlines_; touch(); }
        return *this;
    }
    Path2D& operator=(Path2D&& other) noexcept {
        if (this != &other) {
            segments_ = std::move(other.segments_); glyphOutlines_ = other.glyphOutlines_;
            touch(); other.touch();
        }
        return *this;
    }
    // True once Font::appendText has put glyph outlines in here. The browser
    // does not rasterize text the way it rasterizes a shape -- glyph coverage
    // goes up a gamma ramp, shape coverage does not -- and this is how the
    // rasterizer tells the two apart, since a glyph here IS an ordinary path.
    bool glyphOutlines() const { return glyphOutlines_; }
    void markGlyphOutlines() { glyphOutlines_ = true; }
private:
    void touch() { ++revision_; }
    std::vector<Segment> segments_;
    mutable std::uint32_t key_ = 0;
    std::uint32_t revision_ = 1;
    bool glyphOutlines_ = false;
};

// CanvasRenderingContext2D-style API. Emscripten calls the real browser context
// for every operation. createVirtual() uses an OffscreenCanvas (with a detached
// HTMLCanvasElement fallback) and can be composited with drawCanvas().
class Canvas {
public:
    Canvas(int width, int height, std::string elementId = "canvas");
    static Canvas createVirtual(int width, int height);
    Canvas(const Canvas&) = delete; Canvas& operator=(const Canvas&) = delete;
    Canvas(Canvas&& other) noexcept; Canvas& operator=(Canvas&& other) noexcept;
    ~Canvas();
    // Size in USER units -- the coordinate space every draw call is expressed
    // in. Equal to the backing store unless setLogicalSize() has divorced the
    // two, which is what the browser's CSS size vs canvas.width split does and
    // what lets one canvas be rasterised at a display's real pixel density
    // without every layout in the program having to know about it.
    int width() const { return logicalWidth_; } int height() const { return logicalHeight_; }
    // The backing store's real size in pixels. Only presentation code -- the
    // texture upload, savePPM, getImageData -- has any business with these.
    int pixelWidth() const { return width_; } int pixelHeight() const { return height_; }
    // Declares the user-space size the caller draws in. The caller is
    // responsible for the matching base transform (scale(pixelWidth/width));
    // this only changes what width()/height() report. Zero or negative resets
    // to the backing store's size.
    void setLogicalSize(int width, int height);
    bool isVirtual() const { return virtual_; }
    void present(const std::string& elementId);

    void save(); void restore(); void reset();
    void scale(float x, float y); void rotate(float radians); void translate(float x, float y);
    void transform(float a, float b, float c, float d, float e, float f);
    void setTransform(float a = 1, float b = 0, float c = 0, float d = 1, float e = 0, float f = 0); void resetTransform();
    void setFillStyle(Color color); void setStrokeStyle(Color color);
    void setFillStyle(const std::string& cssColor); void setStrokeStyle(const std::string& cssColor);
    void setGlobalAlpha(float alpha); void setGlobalCompositeOperation(const std::string& operation); void setFilter(const std::string& filter);
    void setLineWidth(float width); void setLineCap(const std::string& cap); void setLineJoin(const std::string& join); void setMiterLimit(float limit);
    void setLineDash(const std::vector<float>& segments); void setLineDashOffset(float offset);
    void setShadow(Color color, float blur, float offsetX = 0, float offsetY = 0);
    void setFont(const std::string& font); void setTextAlign(const std::string& align); void setTextBaseline(const std::string& baseline); void setDirection(const std::string& direction);
    void setImageSmoothingEnabled(bool enabled); void setImageSmoothingQuality(const std::string& quality);
#ifndef __EMSCRIPTEN__
    // The current transform, [a b c d e f], mapping user space to device
    // pixels. Native only, and deliberately so: it exists for callers that
    // cache RASTERIZED output, which have to know what one user unit is worth
    // in pixels before they can bake anything at the right size. The browser
    // build keeps its own glyph cache and needs no such thing.
    std::array<float, 6> currentTransform() const;
    // Source-over of tightly-packed 8-bit RGBA onto whole DEVICE pixels, one
    // texel to one pixel. It honours the clip and globalAlpha but deliberately
    // not the transform: the caller has already worked out which pixels these
    // are, which is the only way a blit is cheaper than redrawing the artwork.
    //
    // drawImage is the general form and stays the general form -- it
    // inverse-maps and filters every pixel so that a rotated or rescaled image
    // still looks like one. Asking it to copy a bitmap onto its own pixels
    // costs several times what rasterising the artwork would have, which is
    // what makes this the narrow one worth having.
    void blitDevice(const std::uint8_t* rgba, int imageWidth, int imageHeight, int deviceX,
                    int deviceY);
#endif

    void clear(Color color = Color{255, 255, 255}); void clearRect(float x, float y, float width, float height);
    void fillRect(float x, float y, float width, float height); void strokeRect(float x, float y, float width, float height);
    void beginPath(); void closePath(); void moveTo(float x, float y); void lineTo(float x, float y);
    void quadraticCurveTo(float cpx, float cpy, float x, float y); void bezierCurveTo(float cp1x, float cp1y, float cp2x, float cp2y, float x, float y);
    void arc(float x, float y, float radius, float startAngle, float endAngle, bool counterClockwise = false);
    void arcTo(float x1, float y1, float x2, float y2, float radius);
    void ellipse(float x, float y, float radiusX, float radiusY, float rotation, float startAngle, float endAngle, bool counterClockwise = false);
    void rect(float x, float y, float width, float height); void roundRect(float x, float y, float width, float height, float radius);
    void fill(const std::string& rule = "nonzero"); void stroke(); void clip(const std::string& rule = "nonzero");
    void fill(const Path2D& path, const std::string& rule = "nonzero"); void stroke(const Path2D& path); void clip(const Path2D& path, const std::string& rule = "nonzero");
    bool isPointInPath(float x, float y, const std::string& rule = "nonzero") const; bool isPointInStroke(float x, float y) const;

    void fillText(const std::string& text, float x, float y, float maxWidth = -1); void strokeText(const std::string& text, float x, float y, float maxWidth = -1); float measureText(const std::string& text) const;
    void drawCanvas(const Canvas& source, float dx, float dy); void drawCanvas(const Canvas& source, float dx, float dy, float dw, float dh);
    // Blits one RECTANGLE of `source` into one rectangle of this canvas: the
    // nine-argument drawImage form. What a caller wants when the scratch
    // surface it drew into is bigger than the part it needs, which is the
    // alternative to clipping the whole surface down to a strip and blitting
    // all of it nine times over.
    void drawCanvas(const Canvas& source, float sx, float sy, float sw, float sh,
                    float dx, float dy, float dw, float dh);
#ifndef __EMSCRIPTEN__
    // The same one-to-one blit, with every source colour multiplied by `tint`
    // (the source keeps its own alpha; the tint's is ignored).
    //
    // Native only, and deliberately: the browser build has real compositing
    // operations and builds a tinted copy with a multiply and a
    // destination-in, while the software backend has none and would otherwise
    // have to read its own pixels back into a buffer, mask them by hand and
    // put them back -- six passes over a surface to do what one blit can.
    void drawCanvasTinted(const Canvas& source, float sx, float sy, float sw, float sh,
                          float dx, float dy, Color tint);
#endif
    // Draws tightly-packed 8-bit RGBA pixels into the user-space box
    // (dx, dy, dw, dh), through the current transform, clip and globalAlpha.
    // This is the real drawImage, not drawCanvas: the destination is sampled by
    // inverse-mapping every covered device pixel, so an image under a rotation
    // stays an image instead of the axis-aligned smear a corner-mapped blit
    // would give. Minification box-filters, so downscaled artwork does not
    // alias into noise.
    // `alpha` multiplies the source, the way a fill folds a node's opacity
    // into its colour; it composes with globalAlpha rather than replacing it.
    //
    // `cacheKey` names these exact pixels, and must be unique to them for the
    // life of the process -- zero means "no name", and costs the backend a
    // fresh upload of the whole image on every call. It matters only on the
    // web, where a browser cannot draw a heap pointer: the pixels have to
    // become a surface first, and doing that per call means a copy of the
    // whole image out of the wasm heap and a brand new canvas, every frame,
    // for a sprite that has not changed since it was decoded.
    void drawImage(const std::uint8_t* rgba, int imageWidth, int imageHeight,
                   float dx, float dy, float dw, float dh, float alpha = 1.0f,
                   std::uint32_t cacheKey = 0);
    // One level of a prefiltered image pyramid: level 0 is the image itself and
    // each one after it is the previous halved with a box filter.
    struct ImageLevel { const std::uint8_t* rgba; int width; int height; };
    // The same draw, choosing the level closest to the size actually being
    // drawn. Without one, minifying is paid for by supersampling: a 124-pixel
    // sprite fitted into sixteen device pixels costs sixteen samples of four
    // texels each, per pixel, every frame. With one the sampler starts from a
    // level already close to the target and spends one or two.
    //
    // Level selection is native-only; the web backend hands the browser level
    // zero and lets it filter, which is what its own mipmapping is for.
    void drawImage(const ImageLevel* levels, int levelCount, float dx, float dy, float dw,
                   float dh, float alpha = 1.0f, std::uint32_t cacheKey = 0);
    std::vector<std::uint8_t> getImageData(int x, int y, int width, int height) const;
    void putImageData(const std::vector<std::uint8_t>& rgba, int sourceWidth, int sourceHeight, int dx, int dy);
    void fillCircle(float centerX, float centerY, float radius); void strokeCircle(float centerX, float centerY, float radius);
    bool savePPM(const std::string& path) const;
    // Native-only interactive presentation. Blocks until the user closes the window.
    bool showWindow(const std::string& title = "C++ Canvas",
                    const std::function<void(Canvas&, float)>& drawFrame = {});

private:
    Canvas(int width, int height, bool isVirtual);
    int width_, height_, contextId_ = -1; bool virtual_ = false; std::string elementId_;
    int logicalWidth_ = 0, logicalHeight_ = 0;
    Color fill_{0, 0, 0}, stroke_{0, 0, 0}; float lineWidth_ = 1.0f; Path2D currentPath_;
#ifdef __EMSCRIPTEN__
    // A mirror of the browser context's own state, so that setting a value it
    // already holds costs nothing. The UI redraws the same few colours and
    // stroke styles over and over -- on the title screen, 124 stroked boxes a
    // frame each set lineCap, lineJoin, lineWidth and strokeStyle to what they
    // were already set to -- and every one of those is a call out of wasm and
    // into the browser's state machine.
    //
    // Defaults are the context's own defaults, which is what makes the mirror
    // valid from the first call rather than from the first write of each field.
    struct WebState {
        std::string fill = "#000000", stroke = "#000000", font = "10px sans-serif";
        std::string lineCap = "butt", lineJoin = "miter";
        std::string textAlign = "start", textBaseline = "alphabetic";
        std::string composite = "source-over", filter = "none", direction = "inherit";
        std::string smoothingQuality = "low", shadowColour = "#00000000";
        std::vector<float> dash;
        float lineWidth = 1, alpha = 1, miterLimit = 10, dashOffset = 0;
        float shadowBlur = 0, shadowOffsetX = 0, shadowOffsetY = 0;
        bool smoothing = true;
    };
    WebState web_;
    std::vector<WebState> webStack_;
    // save() is deferred: a save/restore pair whose body only sets values that
    // were already current has nothing to undo, so neither op is emitted. The
    // pair is materialised by the first change that would really need
    // unwinding -- which is what flushSaves() is called before.
    int pendingSaves_ = 0;
    void flushSaves();
#endif
#ifndef __EMSCRIPTEN__
    // The software framebuffer does not exist in an Emscripten object. This
    // makes accidentally sending a browser draw through the CPU rasterizer a
    // compile-time error rather than a slower path that can silently ship.
    std::vector<Color> pixels_;
    // Software backend state. Everything the browser context tracks per save()
    // lives here; clip is a shared coverage mask so restore() is a pointer swap.
    struct ClipMask { int x0 = 0, y0 = 0, x1 = 0, y1 = 0; std::vector<std::uint8_t> alpha; };
    struct State {
        std::array<float, 6> matrix{1,0,0,1,0,0};
        Color fill{0,0,0}, stroke{0,0,0};
        float lineWidth = 1, alpha = 1, miterLimit = 10, dashOffset = 0, fontSize = 10;
        unsigned char lineCap = 0, lineJoin = 0, textAlign = 0, textBaseline = 3, fontFamily = 0;
        std::vector<float> dash;
        std::shared_ptr<const ClipMask> clip;
    };
    State state_; std::vector<State> stack_;
    void blendPixel(int x, int y, Color color); void paint(int x, int y, Color color, float coverage);
    float clipAt(int x, int y) const;
    std::pair<float,float> mapPoint(float x, float y) const;
    void drawBounds(int& x0, int& y0, int& x1, int& y1) const;
    void blitRegion(const Canvas& source, float sx, float sy, float sw, float sh,
                    float dx, float dy, float dw, float dh, const Color* tint);
    void fillDevice(const Path2D& path, bool evenOdd, Color color);
    void strokeDevice(const Path2D& path);
    void glyphs(const std::string& text, float x, float y, float maxWidth, Color color);
#endif
};

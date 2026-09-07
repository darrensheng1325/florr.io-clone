#pragma once

#include "canvas.h"

#include <memory>
#include <string>
#include <vector>

// A small SVG scene compiler. A document is parsed once into a retained tree of
// baked Path2D geometry; render() evaluates SVG animations at timeSeconds and
// emits only Canvas/Path2D commands, never asking the browser to render SVG.
class SvgDocument {
public:
    ~SvgDocument();
    static SvgDocument fromString(const std::string& source);
    static SvgDocument fromFile(const std::string& path);

    float width() const { return width_; }
    float height() const { return height_; }
    // The author-space box the artwork was drawn in: the viewBox when one is
    // declared, otherwise 0 0 width height.
    float viewBoxX() const { return viewX_; }
    float viewBoxY() const { return viewY_; }
    float viewBoxWidth() const { return viewW_; }
    float viewBoxHeight() const { return viewH_; }
    bool empty() const;
    /// True when any node in the document carries a SMIL timeline, so its ink
    /// depends on `timeSeconds`. False means every frame of this document is
    /// the same picture -- which is what makes rasterising it once and reusing
    /// the pixels a legal substitution rather than a guess about the data.
    bool animated() const;

    // Draws into the document's own width x height viewport at the origin, with
    // the viewBox mapped onto it exactly as an SVG viewer would.
    bool render(Canvas& canvas, float timeSeconds = 0.0f) const;
    // Maps the viewBox into an arbitrary target box, honouring preserveAspectRatio.
    // Callers that scale by width() alone get every viewBox document wrong.
    // The box form takes timeSeconds explicitly: defaulting it would make every
    // five-argument call ambiguous against the square form below.
    bool renderFitted(Canvas& canvas, float x, float y, float size, float timeSeconds = 0.0f) const;
    bool renderFitted(Canvas& canvas, float x, float y, float width, float height, float timeSeconds) const;

    const std::vector<std::string>& warnings() const { return warnings_; }

private:
    struct Scene;
    float width_ = 0, height_ = 0, viewX_ = 0, viewY_ = 0, viewW_ = 0, viewH_ = 0;
    bool hasViewBox_ = false;
    unsigned char align_ = 4, meet_ = 1;   // xMidYMid meet
    std::shared_ptr<const Scene> scene_;
    std::vector<std::string> warnings_;
};

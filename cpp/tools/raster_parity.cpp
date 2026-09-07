// Renders one fixed scene through the software rasterizer and writes it as a
// PPM. Nothing here is about how the scene LOOKS -- it is a fingerprint of the
// rasterizer, so that a change to canvas.cpp can be proved to move no pixels.
//
// Use it by building it once with the committed canvas.cpp and once with the
// working copy, then diffing the two PPMs byte for byte. A correct
// optimisation lands at zero differing bytes; the coverage maths is untouched,
// so anything else is a real regression.
//
// The scene deliberately covers every path the rasterizer has: big fills (the
// ones a scanline-band split threads) and small ones (the ones it must leave
// alone), nonzero and even-odd winding, strokes with every join and cap, dash
// patterns, nested clips, glyph runs at several sizes, and self-intersecting
// outlines whose crossings tie on x.

#include "canvas.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 1280;
constexpr int kHeight = 960;

// A fixed generator rather than <random>: the sequence has to be identical on
// every platform and every standard library, which <random>'s engines are but
// its distributions are not.
struct Rng {
    std::uint32_t state = 0x9e3779b9u;
    std::uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    float unit() { return static_cast<float>(next() & 0xffffff) / 16777216.0f; }
    float range(float lo, float hi) { return lo + unit() * (hi - lo); }
};

Color rgba(Rng& rng) {
    return Color{static_cast<std::uint8_t>(rng.next() & 0xff),
                 static_cast<std::uint8_t>(rng.next() & 0xff),
                 static_cast<std::uint8_t>(rng.next() & 0xff),
                 static_cast<std::uint8_t>(128 + (rng.next() & 0x7f))};
}

// Big translucent shapes stacked over each other: this is what a band split
// has to get right, and overlapping them means every pixel is blended more
// than once, so an ordering mistake shows up as colour rather than shape.
void bigFills(Canvas& canvas, Rng& rng) {
    for (int i = 0; i < 12; ++i) {
        Path2D path;
        const float cx = rng.range(0, kWidth), cy = rng.range(0, kHeight);
        const float radius = rng.range(120, 420);
        const int points = 5 + static_cast<int>(rng.next() % 9);
        for (int p = 0; p < points; ++p) {
            const float angle = 6.2831853f * p / points + rng.unit();
            const float r = radius * (p % 2 ? 0.45f : 1.0f);
            const float x = cx + std::cos(angle) * r, y = cy + std::sin(angle) * r;
            if (p == 0) path.moveTo(x, y); else path.lineTo(x, y);
        }
        path.closePath();
        canvas.setFillStyle(rgba(rng));
        // Alternated so both winding rules run over a self-intersecting star.
        canvas.fill(path, i % 2 ? "evenodd" : "nonzero");
    }

    // A full-surface rect, which is the widest span the difference-array
    // interior pass ever carries.
    canvas.setFillStyle(Color{20, 30, 40, 40});
    canvas.fillRect(0, 0, kWidth, kHeight);
}

// Hundreds of small shapes -- below the threshold that threads them. They are
// most of the CALLS in a real frame, so a split that mishandled the small case
// would show up here rather than in the big fills.
void smallFills(Canvas& canvas, Rng& rng) {
    for (int i = 0; i < 400; ++i) {
        const float x = rng.range(0, kWidth - 40), y = rng.range(0, kHeight - 40);
        canvas.setFillStyle(rgba(rng));
        if (i % 3 == 0) {
            canvas.fillCircle(x, y, rng.range(2, 18));
        } else if (i % 3 == 1) {
            canvas.fillRect(x, y, rng.range(1, 30), rng.range(1, 30));
        } else {
            Path2D path;
            path.roundRect(x, y, rng.range(6, 36), rng.range(6, 36), 3.0f);
            canvas.fill(path);
        }
    }
}

// Every join, cap and dash pattern, at widths from hairline to fat. A stroke
// is expanded to an outline with thousands of edges, so this is also the
// densest active-edge list the scanline walk ever holds.
void strokes(Canvas& canvas, Rng& rng) {
    const char* joins[3] = {"miter", "round", "bevel"};
    const char* caps[3] = {"butt", "round", "square"};
    for (int i = 0; i < 40; ++i) {
        Path2D path;
        const float x0 = rng.range(0, kWidth), y0 = rng.range(0, kHeight);
        path.moveTo(x0, y0);
        for (int p = 0; p < 6; ++p) {
            path.bezierCurveTo(rng.range(0, kWidth), rng.range(0, kHeight),
                               rng.range(0, kWidth), rng.range(0, kHeight),
                               rng.range(0, kWidth), rng.range(0, kHeight));
        }
        canvas.save();
        canvas.setLineJoin(joins[i % 3]);
        canvas.setLineCap(caps[(i / 3) % 3]);
        canvas.setLineWidth(rng.range(0.5f, 22.0f));
        canvas.setMiterLimit(rng.range(1.0f, 12.0f));
        if (i % 4 == 0) {
            canvas.setLineDash({rng.range(2, 20), rng.range(2, 14), rng.range(1, 6)});
            canvas.setLineDashOffset(rng.range(0, 30));
        }
        canvas.setStrokeStyle(rgba(rng));
        canvas.stroke(path);
        canvas.restore();
    }
}

// Clips are built by the same scanline walk, into a coverage mask rather than
// onto pixels -- a second emit callback with its own indexing, so it is worth
// exercising nested.
void clipped(Canvas& canvas, Rng& rng) {
    for (int i = 0; i < 10; ++i) {
        canvas.save();
        Path2D outer;
        outer.roundRect(rng.range(0, kWidth - 400), rng.range(0, kHeight - 300),
                        rng.range(200, 400), rng.range(150, 300), 24.0f);
        canvas.clip(outer);
        if (i % 2) {
            Path2D inner;
            inner.arc(rng.range(0, kWidth), rng.range(0, kHeight), rng.range(60, 220), 0,
                      6.2831853f);
            canvas.clip(inner);
        }
        canvas.setFillStyle(rgba(rng));
        canvas.fillRect(0, 0, kWidth, kHeight);
        canvas.setLineWidth(6.0f);
        canvas.setStrokeStyle(rgba(rng));
        canvas.strokeCircle(rng.range(0, kWidth), rng.range(0, kHeight), rng.range(40, 200));
        canvas.restore();
    }
}

// Filled and stroked text. Glyph coverage goes up a gamma ramp that shape
// coverage does not, and the ramp is applied inside the emit callback, so it
// has to survive being called from a band.
void text(Canvas& canvas, Rng& rng) {
    const char* words[6] = {"Rasterizer", "parity", "AVWjgq", "0123456789", "Hamburgefonstiv",
                            "the quick brown fox"};
    for (int i = 0; i < 60; ++i) {
        const float size = rng.range(8, 54);
        canvas.setFont(std::to_string(static_cast<int>(size)) + "px sans-serif");
        canvas.setFillStyle(rgba(rng));
        const float x = rng.range(0, kWidth - 100), y = rng.range(20, kHeight);
        canvas.fillText(words[i % 6], x, y);
        if (i % 3 == 0) {
            canvas.setLineWidth(rng.range(1, 5));
            canvas.setStrokeStyle(rgba(rng));
            canvas.strokeText(words[(i + 2) % 6], x, y + size);
        }
    }
}

}   // namespace

void scene(Canvas& canvas) {
    canvas.clear(Color{12, 14, 18});

    // One generator threaded through every stage, so adding a stage changes
    // every stage after it -- which is what makes the whole image one
    // fingerprint rather than a set of independent tiles.
    Rng rng;
    bigFills(canvas, rng);
    strokes(canvas, rng);
    clipped(canvas, rng);
    smallFills(canvas, rng);
    text(canvas, rng);

    // A transform under the whole thing: device coordinates are what the
    // rasterizer bands over, and a rotation makes every edge diagonal.
    canvas.save();
    canvas.translate(kWidth * 0.5f, kHeight * 0.5f);
    canvas.rotate(0.37f);
    canvas.scale(0.8f, 1.15f);
    canvas.translate(-kWidth * 0.5f, -kHeight * 0.5f);
    bigFills(canvas, rng);
    strokes(canvas, rng);
    smallFills(canvas, rng);
    canvas.restore();
}

int main(int argc, char** argv) {
    // raster_parity [out.ppm] [repeats]
    //
    // The repeat count turns the same fixed scene into a benchmark. It is the
    // rasterizer alone -- no window, no vsync, no compositor -- so it is the
    // one timing on this client that is repeatable to the millisecond.
    const std::string out = argc > 1 ? argv[1] : "raster_parity.ppm";
    const int repeats = argc > 2 ? std::atoi(argv[2]) : 1;

    Canvas canvas(kWidth, kHeight);

    scene(canvas);   // warm: first call loads the font and grows every scratch buffer
    if (repeats > 1) {
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < repeats; ++i) scene(canvas);
        const double millis = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
        std::fprintf(stderr, "%d scenes in %.1fms -> %.2fms/scene\n", repeats, millis,
                     millis / repeats);
    }

    if (!canvas.savePPM(out)) {
        std::fprintf(stderr, "could not write %s\n", out.c_str());
        return 1;
    }
    std::fprintf(stderr, "wrote %s (%dx%d)\n", out.c_str(), kWidth, kHeight);
    return 0;
}

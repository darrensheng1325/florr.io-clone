#include "client/render/sprites.h"

#include <cctype>
#include <cmath>

#include "client/ui/draw.h"
#include "shared/game/config.h"

namespace flix {

namespace {

/// The fallback fill for a sprite that could not be compiled. Config colours
/// are stored as 0xRRGGBBAA by the loader; the drawing layer works in RGB.
std::uint32_t rgbOf(std::uint32_t rgba) { return rgba >> 8; }

/// The ground artwork of each ground type, in ground-id order.
///
/// These are the nine the map's 3x3 section grid used to hard-code, and they
/// stay in that order so a ground id reads as the section it came from. What
/// changed is who chooses: the map's `background` layer names one per cell, so
/// the nine are a palette rather than a geography. The files are staged from
/// `maps/ground/`, beside the map that names them.
constexpr std::array<const char*, kSectionCount> kGroundArt = {
    "land.svg", "desert.svg", "hel.svg",
    "ocean.svg", "ant_hell.svg", "jungle.svg",
    "sewers.svg", "computer.svg", "unknown.svg",
};

/// The bridge tile's texture, transcribed from MAP_CUSTOM_TILE_TYPES in
/// map_bundle.ts. The source writes its seven planks as CSS matrices about a
/// `transform-origin`, which is a browser-only composition; each is folded
/// here into the equivalent rotation about the plank's own centre.
constexpr const char* kBridgeArt = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="400" height="400" viewBox="0 0 400 400">
<rect x="0" y="0" width="400" height="400" fill="#bbbbbb"/>
<rect x="2.5" y="2.5" width="395" height="50" fill="#999999" stroke="#666666" stroke-width="5"/>
<rect x="2.5" y="347.5" width="395" height="50" fill="#999999" stroke="#666666" stroke-width="5"/>
<rect x="40.016" y="99.169" width="30" height="30" fill="#999999" transform="rotate(15 55.016 114.169)"/>
<rect x="32.593" y="191.994" width="30" height="30" fill="#999999" transform="rotate(-15 47.593 206.994)"/>
<rect x="253.994" y="96.011" width="30" height="30" fill="#999999" transform="rotate(30 268.994 111.011)"/>
<rect x="325.642" y="197.548" width="30" height="30" fill="#999999" transform="rotate(30 340.642 212.548)"/>
<rect x="123.066" y="262.699" width="30" height="30" fill="#999999" transform="rotate(30 138.066 277.699)"/>
<rect x="122.799" y="132.691" width="30" height="30" fill="#999999" transform="rotate(-15 137.799 147.691)"/>
<rect x="228.508" y="288.600" width="30" height="30" fill="#999999" transform="rotate(-15 243.508 303.600)"/>
</svg>)SVG";

/// The sponge artwork, transcribed verbatim from `src/sponge_svg.ts`.
///
/// The sponge petal and both sponge mobs are the same 300-line vector and
/// differ only in two fill colours, so mobs.json and petals.json carry a
/// `$sponge:<body>,<detail>[,<id>]` palette marker instead of three copies of
/// the path data -- the browser build expands it in resolveSpongeImage() as it
/// loads the configs. Handing that marker straight to the SVG compiler is what
/// left all three sponges painting as flat discs.
constexpr const char* kSpongeArt = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32" viewBox="-40 -40 80 80"${idAttr}>
  
  <path fill="${bodyColor}" d="
    M 30 0 
    Q 32.53 0.54 33.93 2.7 
    Q 35.34 4.87 34.8 7.4 
    Q 34.27 9.93 32.1 11.33 
    Q 29.93 12.74 27.41 12.2 
    Q 29.5 13.72 29.9 16.27 
    Q 30.3 18.82 28.79 20.91 
    Q 27.27 23 24.72 23.41 
    Q 22.16 23.81 20.07 22.29 
    Q 20.07 22.29 20.07 22.29 
    Q 21.37 24.53 20.7 27.03 
    Q 20.03 29.52 17.79 30.81 
    Q 15.55 32.11 13.06 31.44 
    Q 10.56 30.77 9.27 28.53 
    Q 9.54 31.1 7.91 33.11 
    Q 6.29 35.12 3.72 35.39 
    Q 1.15 35.66 -0.86 34.03 
    Q -2.87 32.41 -3.14 29.84 
    Q -3.93 32.29 -6.24 33.47 
    Q -8.54 34.64 -11 33.84 
    Q -13.45 33.04 -14.63 30.74 
    Q -15.8 28.44 -15 25.98 
    Q -16.73 27.9 -19.31 28.04 
    Q -21.89 28.17 -23.81 26.44 
    Q -25.73 24.71 -25.86 22.13 
    Q -26 19.55 -24.27 17.63 
    Q -26.63 18.68 -29.04 17.76 
    Q -31.45 16.83 -32.51 14.47 
    Q -33.56 12.11 -32.63 9.7 
    Q -31.7 7.29 -29.34 6.24 
    Q -31.93 6.24 -33.75 4.41 
    Q -35.58 2.58 -35.58 0 
    Q -35.58 -2.58 -33.75 -4.41 
    Q -31.93 -6.24 -29.34 -6.24 
    Q -31.7 -7.29 -32.63 -9.7 
    Q -33.56 -12.11 -32.51 -14.47 
    Q -31.45 -16.83 -29.04 -17.76 
    Q -26.63 -18.68 -24.27 -17.63 
    Q -26 -19.55 -25.86 -22.13 
    Q -25.73 -24.71 -23.81 -26.44 
    Q -21.89 -28.17 -19.31 -28.04 
    Q -16.73 -27.9 -15 -25.98 
    Q -15.8 -28.44 -14.63 -30.74 
    Q -13.45 -33.04 -11 -33.84 
    Q -8.54 -34.64 -6.24 -33.47 
    Q -3.93 -32.29 -3.14 -29.84 
    Q -2.87 -32.41 -0.86 -34.03 
    Q 1.15 -35.66 3.72 -35.39 
    Q 6.29 -35.12 7.91 -33.11 
    Q 9.54 -31.1 9.27 -28.53 
    Q 10.56 -30.77 13.06 -31.44 
    Q 15.55 -32.11 17.79 -30.81 
    Q 20.03 -29.52 20.7 -27.03 
    Q 21.37 -24.53 20.07 -22.29 
    Q 22.16 -23.81 24.72 -23.41 
    Q 27.27 -23 28.79 -20.91 
    Q 30.3 -18.82 29.9 -16.27 
    Q 29.5 -13.72 27.41 -12.2 
    Q 27.41 -12.2 27.41 -12.2 
    Q 29.93 -12.74 32.1 -11.33 
    Q 34.27 -9.92 34.8 -7.4 
    Q 35.34 -4.87 33.93 -2.7 
    Q 32.53 -0.54 30 0 
    L 30 0 Z" />

  <path fill="${detailColor}" d="
    M 30.42 -1.96 
    Q 33.75 -1.25 35.61 1.61 
    Q 37.47 4.48 36.76 7.81 
    Q 36.05 11.15 33.19 13.01 
    Q 30.33 14.87 26.99 14.16 
    L 27.41 12.2 
    L 28.58 10.58 
    Q 31.34 12.59 31.88 15.96 
    Q 32.41 19.33 30.4 22.09 
    Q 28.4 24.85 25.03 25.38 
    Q 21.66 25.92 18.9 23.91 
    L 20.07 22.29 
    L 21.81 21.29 
    Q 23.51 24.25 22.63 27.55 
    Q 21.75 30.84 18.79 32.55 
    Q 15.84 34.25 12.54 33.37 
    Q 9.24 32.49 7.54 29.53 
    L 9.27 28.53 
    L 11.26 28.32 
    Q 11.62 31.72 9.47 34.37 
    Q 7.32 37.02 3.93 37.38 
    Q 0.54 37.73 -2.12 35.59 
    Q -4.77 33.44 -5.12 30.04 
    L -3.14 29.84 
    L -1.23 30.45 
    Q -2.29 33.7 -5.33 35.25 
    Q -8.37 36.8 -11.61 35.74 
    Q -14.86 34.69 -16.41 31.65 
    Q -17.96 28.61 -16.9 25.36 
    L -15 25.98 
    L -13.51 27.32 
    Q -15.8 29.85 -19.2 30.03 
    Q -22.61 30.21 -25.15 27.93 
    Q -27.68 25.65 -27.86 22.24 
    Q -28.04 18.83 -25.76 16.3 
    L -24.27 17.63 
    L -23.46 19.46 
    Q -26.57 20.85 -29.76 19.63 
    Q -32.94 18.4 -34.33 15.29 
    Q -35.72 12.17 -34.5 8.98 
    Q -33.27 5.8 -30.16 4.41 
    L -29.34 6.24 
    L -29.34 8.24 
    Q -32.76 8.24 -35.17 5.82 
    Q -37.58 3.41 -37.58 0 
    Q -37.58 -3.41 -35.17 -5.82 
    Q -32.76 -8.24 -29.34 -8.24 
    L -29.34 -6.24 
    L -30.16 -4.41 
    Q -33.27 -5.8 -34.5 -8.98 
    Q -35.72 -12.17 -34.33 -15.29 
    Q -32.94 -18.4 -29.76 -19.63 
    Q -26.57 -20.85 -23.46 -19.46 
    L -24.27 -17.63 
    L -25.76 -16.3 
    Q -28.04 -18.83 -27.86 -22.24 
    Q -27.68 -25.65 -25.15 -27.93 
    Q -22.61 -30.21 -19.2 -30.03 
    Q -15.8 -29.85 -13.51 -27.32 
    L -15 -25.98 
    L -16.9 -25.36 
    Q -17.96 -28.61 -16.41 -31.65 
    Q -14.86 -34.69 -11.61 -35.74 
    Q -8.37 -36.8 -5.33 -35.25 
    Q -2.29 -33.7 -1.23 -30.45 
    L -3.14 -29.84 
    L -5.12 -30.04 
    Q -4.77 -33.44 -2.12 -35.59 
    Q 0.54 -37.73 3.93 -37.38 
    Q 7.32 -37.02 9.47 -34.37 
    Q 11.62 -31.72 11.26 -28.32 
    L 9.27 -28.53 
    L 7.54 -29.53 
    Q 9.24 -32.49 12.54 -33.37 
    Q 15.84 -34.25 18.79 -32.55 
    Q 21.75 -30.84 22.63 -27.54 
    Q 23.51 -24.25 21.81 -21.29 
    L 20.07 -22.29 
    L 18.9 -23.91 
    Q 21.66 -25.92 25.03 -25.38 
    Q 28.4 -24.85 30.4 -22.09 
    Q 32.41 -19.33 31.88 -15.96 
    Q 31.34 -12.59 28.58 -10.58 
    L 27.41 -12.2 
    L 26.99 -14.16 
    Q 30.33 -14.87 33.19 -13.01 
    Q 36.05 -11.15 36.76 -7.81 
    Q 37.47 -4.48 35.61 -1.61 
    Q 33.75 1.25 30.42 1.96 
    L 30 0 
    L 30.42 -1.96 
    L 30.42 -1.96 Z 
    M 29.58 1.96 
    Q 29 1.83 28.59 1.41 
    Q 28.17 1 28.04 0.42 
    Q 27.87 -0.39 28.32 -1.09 
    Q 28.77 -1.78 29.58 -1.96 
    Q 31.3 -2.32 32.26 -3.79 
    Q 33.21 -5.27 32.85 -6.98 
    Q 32.48 -8.7 31.01 -9.65 
    Q 29.54 -10.61 27.82 -10.25 
    Q 27.24 -10.12 26.69 -10.33 
    Q 26.14 -10.55 25.79 -11.03 
    Q 25.3 -11.7 25.43 -12.51 
    Q 25.56 -13.33 26.23 -13.82 
    Q 27.65 -14.85 27.93 -16.59 
    Q 28.2 -18.32 27.17 -19.74 
    Q 26.14 -21.16 24.4 -21.43 
    Q 22.67 -21.71 21.25 -20.68 
    Q 20.77 -20.33 20.18 -20.3 
    Q 19.59 -20.27 19.07 -20.56 
    Q 18.36 -20.98 18.14 -21.78 
    Q 17.93 -22.58 18.34 -23.29 
    Q 19.22 -24.81 18.77 -26.51 
    Q 18.31 -28.21 16.79 -29.08 
    Q 15.27 -29.96 13.58 -29.51 
    Q 11.88 -29.05 11 -27.53 
    Q 10.71 -27.02 10.18 -26.75 
    Q 9.65 -26.48 9.06 -26.54 
    Q 8.24 -26.63 7.72 -27.27 
    Q 7.19 -27.92 7.28 -28.74 
    Q 7.46 -30.49 6.36 -31.85 
    Q 5.26 -33.21 3.51 -33.4 
    Q 1.76 -33.58 0.4 -32.48 
    Q -0.96 -31.37 -1.15 -29.63 
    Q -1.21 -29.04 -1.58 -28.58 
    Q -1.95 -28.12 -2.52 -27.93 
    Q -3.31 -27.68 -4.04 -28.05 
    Q -4.78 -28.43 -5.04 -29.22 
    Q -5.58 -30.89 -7.14 -31.68 
    Q -8.71 -32.48 -10.38 -31.94 
    Q -12.05 -31.4 -12.84 -29.83 
    Q -13.64 -28.27 -13.1 -26.6 
    Q -12.91 -26.04 -13.07 -25.46 
    Q -13.22 -24.89 -13.66 -24.49 
    Q -14.28 -23.94 -15.1 -23.98 
    Q -15.93 -24.03 -16.49 -24.64 
    Q -17.66 -25.95 -19.41 -26.04 
    Q -21.17 -26.13 -22.47 -24.96 
    Q -23.77 -23.78 -23.87 -22.03 
    Q -23.96 -20.28 -22.78 -18.97 
    Q -22.39 -18.53 -22.3 -17.95 
    Q -22.2 -17.36 -22.44 -16.82 
    Q -22.78 -16.06 -23.55 -15.77 
    Q -24.33 -15.47 -25.08 -15.81 
    Q -26.69 -16.52 -28.33 -15.89 
    Q -29.96 -15.26 -30.68 -13.66 
    Q -31.39 -12.06 -30.76 -10.42 
    Q -30.13 -8.78 -28.53 -8.06 
    Q -27.99 -7.82 -27.67 -7.33 
    Q -27.34 -6.83 -27.34 -6.24 
    Q -27.34 -5.41 -27.93 -4.82 
    Q -28.52 -4.24 -29.34 -4.24 
    Q -31.1 -4.24 -32.34 -3 
    Q -33.58 -1.76 -33.58 0 
    Q -33.58 1.76 -32.34 3 
    Q -31.1 4.24 -29.34 4.24 
    Q -28.75 4.24 -28.26 4.56 
    Q -27.76 4.88 -27.52 5.42 
    Q -27.18 6.18 -27.48 6.95 
    Q -27.77 7.73 -28.53 8.06 
    Q -30.13 8.78 -30.76 10.42 
    Q -31.39 12.06 -30.68 13.66 
    Q -29.96 15.26 -28.33 15.89 
    Q -26.69 16.52 -25.08 15.81 
    Q -24.54 15.57 -23.96 15.66 
    Q -23.37 15.75 -22.93 16.15 
    Q -22.32 16.7 -22.27 17.53 
    Q -22.23 18.36 -22.78 18.97 
    Q -23.96 20.28 -23.87 22.03 
    Q -23.77 23.78 -22.47 24.96 
    Q -21.17 26.13 -19.41 26.04 
    Q -17.66 25.95 -16.49 24.64 
    Q -16.09 24.2 -15.52 24.05 
    Q -14.95 23.9 -14.38 24.08 
    Q -13.59 24.33 -13.22 25.07 
    Q -12.84 25.81 -13.1 26.6 
    Q -13.64 28.27 -12.84 29.83 
    Q -12.05 31.4 -10.38 31.94 
    Q -8.71 32.48 -7.14 31.68 
    Q -5.58 30.89 -5.04 29.22 
    Q -4.85 28.65 -4.39 28.28 
    Q -3.93 27.91 -3.34 27.85 
    Q -2.52 27.76 -1.88 28.28 
    Q -1.23 28.8 -1.15 29.63 
    Q -0.96 31.37 0.4 32.48 
    Q 1.76 33.58 3.51 33.4 
    Q 5.26 33.21 6.36 31.85 
    Q 7.46 30.49 7.28 28.74 
    Q 7.22 28.15 7.49 27.62 
    Q 7.76 27.1 8.27 26.8 
    Q 8.99 26.39 9.79 26.6 
    Q 10.59 26.81 11 27.53 
    Q 11.88 29.05 13.58 29.51 
    Q 15.27 29.96 16.79 29.08 
    Q 18.31 28.21 18.77 26.51 
    Q 19.22 24.81 18.34 23.29 
    Q 18.05 22.78 18.08 22.19 
    Q 18.11 21.6 18.46 21.12 
    Q 18.94 20.45 19.76 20.32 
    Q 20.58 20.19 21.25 20.68 
    Q 22.67 21.71 24.4 21.43 
    Q 26.14 21.16 27.17 19.74 
    Q 28.2 18.32 27.93 16.59 
    Q 27.65 14.85 26.23 13.82 
    Q 25.75 13.47 25.54 12.92 
    Q 25.33 12.37 25.45 11.79 
    Q 25.62 10.98 26.32 10.52 
    Q 27.01 10.07 27.82 10.25 
    Q 29.54 10.61 31.01 9.65 
    Q 32.48 8.7 32.85 6.98 
    Q 33.21 5.27 32.26 3.79 
    Q 31.3 2.32 29.58 1.96 
    L 29.58 1.96 Z" />

  <path fill="${detailColor}" d="
    M 5 0 L 8 0 Q 8 1.24 7.12 2.12 Q 6.24 3 5 3 Q 3.76 3 2.88 2.12 Q 2 1.24 2 0 Q 2 -1.24 2.88 -2.12 Q 3.76 -3 5 -3 Q 6.24 -3 7.12 -2.12 Q 8 -1.24 8 0 
    M 12 0 L 16 0 Q 16 1.66 14.83 2.83 Q 13.66 4 12 4 Q 10.34 4 9.17 2.83 Q 8 1.66 8 0 Q 8 -1.66 9.17 -2.83 Q 10.34 -4 12 -4 Q 13.66 -4 14.83 -2.83 Q 16 -1.66 16 0 
    M 22 0 L 27 0 Q 27 2.07 25.54 3.54 Q 24.07 5 22 5 Q 19.93 5 18.46 3.54 Q 17 2.07 17 0 Q 17 -2.07 18.46 -3.54 Q 19.93 -5 22 -5 Q 24.07 -5 25.54 -3.54 Q 27 -2.07 27 0 
    M 1.55 4.76 L 4.55 4.76 Q 4.55 6 3.67 6.88 Q 2.79 7.76 1.55 7.76 Q 0.3 7.76 -0.58 6.88 Q -1.45 6 -1.45 4.76 Q -1.45 3.51 -0.58 2.63 Q 0.3 1.76 1.55 1.76 Q 2.79 1.76 3.67 2.63 Q 4.55 3.51 4.55 4.76 
    M 3.71 11.41 L 7.71 11.41 Q 7.71 13.07 6.54 14.24 Q 5.37 15.41 3.71 15.41 Q 2.05 15.41 0.88 14.24 Q -0.29 13.07 -0.29 11.41 Q -0.29 9.76 0.88 8.58 Q 2.05 7.41 3.71 7.41 Q 5.37 7.41 6.54 8.58 Q 7.71 9.76 7.71 11.41 
    M 6.8 20.92 L 11.8 20.92 Q 11.8 22.99 10.33 24.46 Q 8.87 25.92 6.8 25.92 Q 4.73 25.92 3.26 24.46 Q 1.8 22.99 1.8 20.92 Q 1.8 18.85 3.26 17.39 Q 4.73 15.92 6.8 15.92 Q 8.87 15.92 10.33 17.39 Q 11.8 18.85 11.8 20.92 
    M -4.05 2.94 L -1.05 2.94 Q -1.05 4.18 -1.92 5.06 Q -2.8 5.94 -4.05 5.94 Q -5.29 5.94 -6.17 5.06 Q -7.05 4.18 -7.05 2.94 Q -7.05 1.7 -6.17 0.82 Q -5.29 -0.06 -4.05 -0.06 Q -2.8 -0.06 -1.92 0.82 Q -1.05 1.7 -1.05 2.94 
    M -9.71 7.05 L -5.71 7.05 Q -5.71 8.71 -6.88 9.88 Q -8.05 11.05 -9.71 11.05 Q -11.37 11.05 -12.54 9.88 Q -13.71 8.71 -13.71 7.05 Q -13.71 5.4 -12.54 4.22 Q -11.37 3.05 -9.71 3.05 Q -8.05 3.05 -6.88 4.22 Q -5.71 5.4 -5.71 7.05 
    M -17.8 12.93 L -12.8 12.93 Q -12.8 15 -14.26 16.47 Q -15.73 17.93 -17.8 17.93 Q -19.87 17.93 -21.33 16.47 Q -22.8 15 -22.8 12.93 Q -22.8 10.86 -21.33 9.4 Q -19.87 7.93 -17.8 7.93 Q -15.73 7.93 -14.26 9.4 Q -12.8 10.86 -12.8 12.93 
    M -4.05 -2.94 L -1.05 -2.94 Q -1.05 -1.7 -1.92 -0.82 Q -2.8 0.06 -4.05 0.06 Q -5.29 0.06 -6.17 -0.82 Q -7.05 -1.7 -7.05 -2.94 Q -7.05 -4.18 -6.17 -5.06 Q -5.29 -5.94 -4.05 -5.94 Q -2.8 -5.94 -1.92 -5.06 Q -1.05 -4.18 -1.05 -2.94 
    M -9.71 -7.05 L -5.71 -7.05 Q -5.71 -5.4 -6.88 -4.22 Q -8.05 -3.05 -9.71 -3.05 Q -11.37 -3.05 -12.54 -4.22 Q -13.71 -5.4 -13.71 -7.05 Q -13.71 -8.71 -12.54 -9.88 Q -11.37 -11.05 -9.71 -11.05 Q -8.05 -11.05 -6.88 -9.88 Q -5.71 -8.71 -5.71 -7.05 
    M -17.8 -12.93 L -12.8 -12.93 Q -12.8 -10.86 -14.26 -9.4 Q -15.73 -7.93 -17.8 -7.93 Q -19.87 -7.93 -21.33 -9.4 Q -22.8 -10.86 -22.8 -12.93 Q -22.8 -15 -21.33 -16.47 Q -19.87 -17.93 -17.8 -17.93 Q -15.73 -17.93 -14.26 -16.47 Q -12.8 -15 -12.8 -12.93 
    M 1.55 -4.76 L 4.55 -4.76 Q 4.55 -3.51 3.67 -2.63 Q 2.79 -1.76 1.55 -1.76 Q 0.3 -1.76 -0.58 -2.63 Q -1.45 -3.51 -1.45 -4.76 Q -1.45 -6 -0.58 -6.88 Q 0.3 -7.76 1.55 -7.76 Q 2.79 -7.76 3.67 -6.88 Q 4.55 -6 4.55 -4.76 
    M 3.71 -11.41 L 7.71 -11.41 Q 7.71 -9.76 6.54 -8.58 Q 5.37 -7.41 3.71 -7.41 Q 2.05 -7.41 0.88 -8.58 Q -0.29 -9.76 -0.29 -11.41 Q -0.29 -13.07 0.88 -14.24 Q 2.05 -15.41 3.71 -15.41 Q 5.37 -15.41 6.54 -14.24 Q 7.71 -13.07 7.71 -11.41 
    M 6.8 -20.92 L 11.8 -20.92 Q 11.8 -18.85 10.33 -17.39 Q 8.87 -15.92 6.8 -15.92 Q 4.73 -15.92 3.26 -17.39 Q 1.8 -18.85 1.8 -20.92 Q 1.8 -22.99 3.26 -24.46 Q 4.73 -25.92 6.8 -25.92 Q 8.87 -25.92 10.33 -24.46 Q 11.8 -22.99 11.8 -20.92" />
</svg>)SVG";

/// Expands a `$sponge:` palette marker into `kSpongeArt`; any other string is
/// returned unchanged. Mirrors the browser build's resolveSpongeImage().
std::string resolveArtwork(const std::string& source) {
    static const std::string kMarker = "$sponge:";
    if (source.compare(0, kMarker.size(), kMarker) != 0) return source;

    // `<body>,<detail>[,<id>]`. A missing id means no id attribute at all,
    // which is what the reference's template literal produces for it.
    std::string field[3];
    std::size_t at = kMarker.size();
    for (int i = 0; i < 3 && at <= source.size(); ++i) {
        const std::size_t comma = source.find(',', at);
        field[i] = source.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }

    const std::string token[3] = {"${idAttr}", "${bodyColor}", "${detailColor}"};
    const std::string value[3] = {field[2].empty() ? "" : " id=\"" + field[2] + "\"", field[0], field[1]};
    std::string out = kSpongeArt;
    for (int i = 0; i < 3; ++i)
        for (std::size_t found = out.find(token[i]); found != std::string::npos;
             found = out.find(token[i], found + value[i].size()))
            out.replace(found, token[i].size(), value[i]);
    return out;
}

/// True when `source` declares artwork at all: an element other than the <svg>
/// wrapper, or one of the '$' procedural markers.
///
/// This is what separates "the browser draws nothing here either" from "we
/// could not build what this declares". Several petals and mobs ship a
/// literally empty document (`air`, `bush`, `garbage`, `leafbug`, `mantis`),
/// and the reference rasterises those to a blank canvas -- painting a disc for
/// them put a solid black blob on every `air` card in the shop, since their
/// declared colour is #000000. A document that declares real artwork and still
/// fails to build keeps the coloured stand-in instead, because the alternative
/// is an invisible petal. The `$` case is the residue of that rule: a marker
/// this loader does not recognise names artwork it cannot expand.
bool declaresArtwork(const std::string& source) {
    if (!source.empty() && source[0] == '$') return true;
    for (std::size_t i = 0; i + 1 < source.size(); ++i) {
        if (source[i] != '<') continue;
        std::size_t at = i + 1;
        if (source[at] == '/') ++at;
        // Comments, declarations and processing instructions are not artwork.
        if (at >= source.size() || !std::isalpha(static_cast<unsigned char>(source[at]))) continue;
        if (source.compare(at, 3, "svg") == 0) continue;
        return true;
    }
    return false;
}

} // namespace

std::shared_ptr<SvgDocument> SpriteCache::compileArt(const std::string& source,
                                                     const std::string& label) {
    auto document = std::make_shared<SvgDocument>(SvgDocument::fromString(source));
    if (document->empty()) {
        warnings_.push_back(label + ": artwork produced no geometry");
        return nullptr;
    }
    for (const std::string& w : document->warnings()) warnings_.push_back(label + ": " + w);
    return document;
}

bool SpriteCache::build(const ContentRegistry& content, const std::string& dataDir) {
    warnings_.clear();
    mobs_.assign(content.mobCount(), Sprite{});
    petals_.assign(content.petalCount(), Sprite{});

    const auto compile = [this](const std::string& source, std::uint32_t colorRgba,
                                const std::string& label, Sprite& out) {
        out.fallbackColor = rgbOf(colorRgba);
        if (source.empty()) {
            warnings_.push_back(label + ": no artwork, drawing a plain disc");
            return;
        }
        const std::string art = resolveArtwork(source);
        auto document = compileArt(art, label);
        if (!document) {
            out.blank = !declaresArtwork(art);
            return;
        }
        out.document = std::move(document);
        out.usable = true;
    };

    for (std::size_t i = 0; i < mobs_.size(); ++i) {
        const MobConfig& config = content.mob(static_cast<std::uint16_t>(i));
        compile(config.image, config.colorRgba, "mob " + config.id, mobs_[i]);
    }
    for (std::size_t i = 0; i < petals_.size(); ++i) {
        const PetalConfig& config = content.petal(static_cast<std::uint16_t>(i));
        compile(config.image, config.colorRgba, "petal " + config.id, petals_[i]);
    }

    for (std::size_t i = 0; i < kGroundArt.size(); ++i) {
        if (!kGroundArt[i]) continue;
        const std::string path = dataDir + "/" + kGroundArt[i];
        auto document = std::make_shared<SvgDocument>(SvgDocument::fromFile(path));
        if (document->empty()) {
            // Ground artwork is optional: the flat biome colour still reads as
            // ground, so a missing file must not stop the client.
            warnings_.push_back("ground " + path + ": unreadable, falling back to a flat fill");
            continue;
        }
        ground_[i] = std::move(document);
    }
    bridge_ = compileArt(kBridgeArt, "tile bridge");

    return !mobs_.empty() && !petals_.empty();
}

bool SpriteCache::mobDrawable(std::uint16_t index) const {
    return index < mobs_.size() && mobs_[index].usable;
}

bool SpriteCache::petalDrawable(std::uint16_t index) const {
    return index < petals_.size() && petals_[index].usable;
}

const SvgDocument* SpriteCache::groundArt(int groundId) const {
    if (groundId < 0 || groundId >= kSectionCount) return nullptr;
    return ground_[static_cast<std::size_t>(groundId)].get();
}

const SvgDocument* SpriteCache::tileArt(Tile tile) const {
    // Only the bridge carries artwork the flat colour cannot express: sewage
    // is a solid fill in its own SVG, and block declares none at all.
    return tile == Tile::Sand ? bridge_.get() : nullptr;
}

void SpriteCache::draw(Canvas& canvas, const Sprite& sprite, double x, double y, double diameter,
                       double rotation, double timeSeconds, bool mirrored) const {
    if (diameter <= 0.5) return;   // sub-pixel; not worth the transform

    if (!sprite.usable) {
        // A document that declares nothing draws nothing, exactly as the
        // reference's blank rasterised canvas does.
        if (!sprite.blank) ui::disc(canvas, {x, y}, diameter * 0.5, sprite.fallbackColor);
        return;
    }

    canvas.save();
    canvas.translate(static_cast<float>(x), static_cast<float>(y));
    if (rotation != 0.0) canvas.rotate(static_cast<float>(rotation));
    if (mirrored) canvas.scale(-1.0f, 1.0f);
    // renderFitted maps the document's viewBox into the target box. Scaling by
    // width() instead is what made the artwork render at wildly wrong sizes --
    // these documents declare width="32" while drawing in a viewBox many times
    // that, and the two are unrelated.
    sprite.document->renderFitted(canvas,
                                  static_cast<float>(-diameter * 0.5),
                                  static_cast<float>(-diameter * 0.5),
                                  static_cast<float>(diameter),
                                  static_cast<float>(diameter),
                                  static_cast<float>(timeSeconds));
    canvas.restore();
}

void SpriteCache::drawMob(Canvas& canvas, std::uint16_t index, double x, double y, double diameter,
                          double rotation, double timeSeconds, bool mirrored) const {
    if (index >= mobs_.size()) return;
    draw(canvas, mobs_[index], x, y, diameter, rotation, timeSeconds, mirrored);
}

void SpriteCache::drawPetal(Canvas& canvas, std::uint16_t index, double x, double y,
                            double diameter, double rotation, double timeSeconds) const {
    if (index >= petals_.size()) return;
    draw(canvas, petals_[index], x, y, diameter, rotation, timeSeconds, false);
}

} // namespace flix

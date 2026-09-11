'use strict';
/**
 * Skin 0: the default wall and water -- exactly the art maps/tiles/wall.svg
 * and water.svg have always had. This module reproduces the thirty
 * wall_edge_* / water_edge_* SVGs byte for byte; the two base tiles
 * themselves are hand-made files that `--art` never rewrites.
 *
 * It is also the template every other skin copies: see scripts/lib/tileArt.js
 * (renderTile) for what the framework does with each field.
 */

const TILE = 300;

/** The five dots of maps/tiles/wall.svg (drawn there in a 124-unit box). */
const WALL_DOTS = [
    [25.2109, 51.5391], [105.5341, 25.5207], [51.5308, 85.3607],
    [64.5341, 15.5207], [103.5341, 102.5207],
].map(([cx, cy]) => [cx * TILE / 124, cy * TILE / 124]);
const WALL_DOT_RADIUS = 5.1641 * TILE / 124;

module.exports = {
    name: '',

    wall: {
        fill: '#99550c',
        band: '#783f01',
        line: '#6a3a05',
        lineWidth: 3,
        // Seven teeth between the two corner runs; the depths alternate around
        // the nominal 20 with a little wobble so the lip reads as rock, not a
        // saw. Both ends sit at the nominal depth.
        profile: { kind: 'teeth', depth: 20, teeth: [20, 28, 13, 25, 11, 27, 15, 20] },
        motif(g) {
            g.group({ fill: '#783f01' }, g => {
                for (const [cx, cy] of WALL_DOTS) g.circle(cx, cy, WALL_DOT_RADIUS);
            });
        },
    },

    water: {
        fill: '#4169E1',
        band: '#2A4FA0',
        foam: '#8fb3ff',
        foamWidth: 2,
        // Three full waves between the corner runs, swinging from depth 16
        // (at the corners) to 22.
        profile: { kind: 'wave', depth: 16, periods: 3, amplitude: 3 },
    },

    // The default skin has no floor decorations: the six builtin tiles are
    // the whole of its block.
    floors: null,
};

# florr.io clone

A browser-based multiplayer survival game inspired by [florr.io](https://florr.io).

> **License notice:** As of July 2026 this project is licensed under the
> [GNU General Public License v3.0 or later](LICENSE) (previously ISC).
> If you distribute this software or a modified version of it, you must do so
> under the same license and make the corresponding source code available.

## Quick start

Requires Node.js 22+

```bash
npm install
npm run build   # builds cpp, webpacks the client, compresses bundle
npm start       # compiles the server and runs dist/server.js
```

Open `https://localhost:3000`.

### Play offline

```bash
npm run build:offline   # -> dist/offline.html (needs emscripten on PATH)
```

`dist/offline.html` is the whole game in one file: the server and the client
compiled into one wasm and embedded in the page. Open it straight from disk —
no server, no network. Your account and progress are kept in that browser's
local storage. See `cpp/docs/ARCHITECTURE.md`, "The offline build".

## Scripts

| Script | Purpose |
| --- | --- |
| `npm run build` | Build cpp module, bundle client (production), compress bundle |
| `npm run build:offline` | Single-file offline build: server and client in one page, `dist/offline.html`, opens from disk with no server |
| `npm run build:server` | TypeScript compile of the server only |
| `npm run build:client` | TypeScript compile of the client only |
| `npm start` | Build server and run `dist/server.js` |
| `npm run dev` | Webpack dev watch |
| `npm run dev:server` | Run server under `ts-node-dev` |
| `npm run build:map` | Compile `maps/world.tmj` into `src/map_bundle.ts` after editing the map in Tiled |
| `npm run svg2skin -- art.svg` | Convert an SVG into custom-skin commands |

### Biomes

The world is divided into biomes (ocean, desert, jungle, etc.) with their own mobs, rarities, and scaling. The map is authored in [Tiled](https://www.mapeditor.org) and lives in `maps/world.tmj`, with the tile palette in `maps/terrain.tsj` — see [`maps/README.md`](maps/README.md).

### Custom skins from SVG

Player-made skins are a short list of canvas primitives (see `src/skin_format.ts`),
typed into **Skin Studio ▸ Create ▸ Text**. To start from artwork instead:

```bash
npm run svg2skin -- logo.svg                    # prints the command lines
npm run svg2skin -- logo.svg --max-shapes 16 --hole-fill '#ffe763'
```

Or open `SvgToSkin.html` for drag-and-drop with a live preview of the skin on a
flower. Both use the same converter, which flattens transforms, turns paths into
polygons/curves, fits the drawing to the flower body, and drops the smallest
shapes when the artwork exceeds the 24-shape limit.

### Rarities

`common`, `uncommon`, `rare`, `epic`, `legendary`, `mythic`, `ultra`, `super`, `unique` and `apex`.

## Features

- **Multiplayer** via raw WebSockets, with cross-server portals for transferring between server instances(server transfer disabled in default map)
- **Biome system** with per-biome mob pools, spawners, and scaling
- **Petal crafting** across the rarity ladder
- **Guilds & squads**, chat, and a leaderboard
- **PvP arena** with dedicated spawner and rendering
- **Bots** for populating servers
- **Daily streak** rewards and a tutorial flow for new players
- **Tiled map** (`maps/world.tmj`) for the world's terrain, spawn zones, biomes and teleporters
- **SVG → skin converter** (`SvgToSkin.html`, `scripts/svg-to-skin.js`) for turning artwork into custom-skin commands
- **Persistence** via custom JSON database
- **HTTPS** support (drop `cert.crt` / `cert.key` at the project root). If that
  certificate is a localhost one and has expired, the server generates a fresh
  short-lived `dev-cert.crt` at boot so `npm start` always serves a live
  certificate; a real certificate for a real hostname is never touched.
- **WebTransport (HTTP/3)** alongside WebSocket — the client probes
  `/transport-info` and picks one automatically, falling back to WebSocket
  whenever QUIC is unavailable. Each connection logs which it chose and why.

## Chat commands

Players:

- `/help` — list available commands
- `/list_all`, `/list_common`, `/list_uncommon`, `/list_rare`, `/list_epic`, `/list_legendary`, `/list_mythic`, `/list_ultra`, `/list_super`, `/list_unique` — list mobs of the given rarity

Chat accepts limited inline HTML: `<b>`, `<i>`, `<u>`, `<span style="color:…">`, `<blink>`.

## Admin

Flag a user as admin by setting `"admin": true` on their record in `dist/inventory.json` (or the SQLite row). Admins can run server commands from chat with `/admin <cmd>` or `/cmd <cmd>`. Non-admins attempting admin commands get "Command does not exist". All admin invocations are logged server-side.

### Server console / admin commands

Typed on the server's stdin, or via `/admin` in chat:

- `save` / `save <playerId>` — persist player progress
- `list-players`, `list-sockets` — enumerate connections
- `spawn <mobType> <rarity> [x y]` — spawn a mob (e.g. `spawn hornet legendary 1000 2000`)
- `spawn_special_mobs` — spawn ultra/super/unique tiers
- `set_max_enemies <n>` — cap concurrent enemy count

Mob types include: `bee`, `hornet`, `mantis`, `ladybug`, `soldier_ant`, `leafbug`, `bush`, `target_dummy`, `item_spawner`. See `src/mobs.ts` for the current list.

## Project layout

```
src/
  server.ts, server/        # server entry + managers (players, enemies, guilds, squads, bots, cross-server)
  game.ts, player.ts        # client game loop and player state
  graphics/                 # rendering (flower, enemies, panels, minimap, effects)
  petals.ts, petal_action/  # petal configs and per-petal behaviors
  mobs.ts, enemy.ts         # mob definitions and AI
  inventory.ts, shop.ts     # inventory, crafting, shop
  chat.ts, guildMenu.ts     # chat and guild UI
  map_data.ts               # biome/map loading
  database.ts               # JSON persistence
  ws_client.ts, ws_server.ts, signaling.ts  # networking
cpp/                        # native physics module (built with make)
assembly/                   # AssemblyScript sources
assets/                     # images and SVGs
scripts/                    # build helpers (e.g. encodeMap.js, compressbundle.js)
maps/                       # the world map, in Tiled format
MapEditor.html              # the retired map editor; Tiled replaced it
SvgToSkin.html              # standalone SVG → skin command converter
```

## Stack

TypeScript, Canvas 2D, Socket.IO + `ws`, Express, `better-sqlite3`, Webpack

## Set up production server

Make sure you have PM2 installed
Paste update_aws.sh contents into terminal

## Update production server

Run "/admin update" in server chat

## License

Copyright (c) 2026 sussybite8888

This program is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE) for the full text.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

Versions of this project prior to the relicense were distributed under the
ISC License.

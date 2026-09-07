cmake_minimum_required(VERSION 3.16)
project(flowrix_cpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# --- build flavours ----------------------------------------------------------
# Two, chosen with -DFLIX_BUILD=dev or -DFLIX_BUILD=release:
#
#   dev      what you build while working on the game. Still optimised: the
#            native client rasterises every pixel on the CPU, and an -O0 build
#            does not reach a frame rate you can judge anything by. The web
#            client always emits browser Canvas2D operations instead. Frame
#            pointers are kept so a profile has call stacks.
#   release  what ships. -O3, no debug info, asserts and the web build's
#            runtime checks compiled out.
#
# The flavour is the knob; CMAKE_BUILD_TYPE follows from it rather than being
# set alongside it, so there is only one way to say which build this is.
set(FLIX_BUILD "dev" CACHE STRING "Build flavour: dev or release")
set_property(CACHE FLIX_BUILD PROPERTY STRINGS dev release)

if(FLIX_BUILD STREQUAL "release")
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Follows FLIX_BUILD" FORCE)
elseif(FLIX_BUILD STREQUAL "dev")
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Follows FLIX_BUILD" FORCE)
  # RelWithDebInfo defines NDEBUG, which compiles out every assert -- the one
  # thing a development build is most for. Take it back off the flags.
  string(REPLACE "-DNDEBUG" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO
                 "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  add_compile_options(-fno-omit-frame-pointer)
else()
  message(FATAL_ERROR "FLIX_BUILD must be dev or release, not '${FLIX_BUILD}'")
endif()
message(STATUS "flowrix: ${FLIX_BUILD} build (${CMAKE_BUILD_TYPE})")

add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# --- what the web build is ---------------------------------------------------
# `emcmake cmake ..` sets EMSCRIPTEN and builds TWO targets: the client, as a
# page, and the server, as a Node program. A browser tab cannot listen on a
# port, but Node can, so the server's web build targets Node rather than the
# page -- see its link step below. The tools and the test binary are the only
# native-only targets.
#
# FLIX_WEB_BUILD is what the client compiles differently: the chat tags that
# need a document to mean anything are honoured only where there is one. See
# client/ui/markup.h.
if(NOT EMSCRIPTEN)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
endif()

# --- cpp_canvas: the vendored Canvas2D-alike renderer ------------------------
# Upstream ships it as a demo executable; built as a library here so both the
# client and the render tools can link it.
add_library(cpp_canvas STATIC
  third_party/cpp_canvas/canvas.cpp
  third_party/cpp_canvas/svg.cpp
  third_party/cpp_canvas/font.cpp
  third_party/cpp_canvas/window.cpp
)
target_include_directories(cpp_canvas PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp_canvas)
target_compile_options(cpp_canvas PRIVATE -w)   # third-party: not ours to lint
# SDL is desktop-only and attached here rather than to the whole directory.
# PUBLIC lets it reach the native client sources that open links through SDL.
# Browser rendering and input use Canvas2D and DOM APIs directly, so the web
# artifact does not link Emscripten's SDL/WebGL compatibility layer at all.
if(NOT EMSCRIPTEN)
  target_link_libraries(cpp_canvas PUBLIC PkgConfig::SDL2)
  # The software rasterizer splits a large fill across scanline bands on a
  # small thread pool. Only the native build has that code at all -- the web
  # build draws through the browser's own context -- so the dependency is
  # attached on the same terms SDL is.
  find_package(Threads REQUIRED)
  target_link_libraries(cpp_canvas PUBLIC Threads::Threads)
endif()

# --- shared: engine core linked by both binaries -----------------------------
add_library(flix_shared STATIC
  shared/core/world.cpp
  shared/core/json.cpp
  shared/core/process_stats.cpp
  shared/net/transport.cpp
  shared/net/web_channel.cpp
  shared/game/rarity.cpp
  shared/game/config.cpp
  shared/game/skin_format.cpp
  shared/game/map_elements.cpp
  shared/game/terrain.cpp
  shared/game/spatial.cpp
)
target_include_directories(flix_shared PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# --- server ------------------------------------------------------------------
add_library(flix_server STATIC
  server/game_server.cpp
  server/account_limits.cpp
  server/bot_ai.cpp
  server/auto_update.cpp
  server/chat_commands.cpp
  server/replication.cpp
  server/session.cpp
  server/squads.cpp
  server/db.cpp
  server/crypto.cpp
  server/systems/movement.cpp
  server/systems/mob_ai.cpp
  server/systems/petals.cpp
  server/systems/combat.cpp
  server/systems/spawning.cpp
  server/systems/loot.cpp
)
target_link_libraries(flix_server PUBLIC flix_shared)

add_executable(flowrix_server server/main.cpp)
target_link_libraries(flowrix_server PRIVATE flix_server)

# --- client ------------------------------------------------------------------
add_library(flix_client STATIC
  client/app.cpp
  client/net_client.cpp
  client/world_view.cpp
  client/ui/draw.cpp
  client/ui/text.cpp
  client/ui/text_cache.cpp
  client/ui/markup.cpp
  client/ui/menu_style.cpp
  client/ui/menu_widgets.cpp
  client/ui/item_tile.cpp
  client/ui/menus.cpp
  client/ui/menu_inventory.cpp
  client/ui/menu_crafting.cpp
  client/ui/menu_gallery.cpp
  client/ui/menu_guild.cpp
  client/ui/menu_talents.cpp
  client/ui/menu_shop.cpp
  client/ui/menu_skins.cpp
  client/ui/menu_leaderboard.cpp
  client/ui/menu_notifications.cpp
  client/ui/menu_changelog.cpp
  client/ui/menu_settings.cpp
  client/ui/tutorial.cpp
  client/render/art_cache.cpp
  client/render/sprites.cpp
  client/render/world_renderer.cpp
  client/web/reload.cpp
)
target_link_libraries(flix_client PUBLIC flix_shared cpp_canvas)

add_executable(flowrix_client client/main.cpp)
target_link_libraries(flowrix_client PRIVATE flix_client)

# Browser-only: the settings and session files are the client's whole writable
# state, and MEMFS does not survive a reload. See client/web/persist.cpp.
if(EMSCRIPTEN)
  target_sources(flix_client PRIVATE client/web/persist.cpp)
endif()

# --- runtime data ------------------------------------------------------------
# The browser build loads these exact faces from Google Fonts. Stage pinned
# copies for the native client so its typography does not depend on whatever
# sans-serif happens to be installed on the host OS.
set(FLIX_DATA_DIR ${CMAKE_CURRENT_BINARY_DIR}/data)
file(MAKE_DIRECTORY ${FLIX_DATA_DIR})
set(FLIX_GOOGLE_FONTS_REV ade3d1533e06b2b1462ffcde8e08b129627ca360)
set(FLIX_GOOGLE_FONTS_ROOT
    "https://raw.githubusercontent.com/google/fonts/${FLIX_GOOGLE_FONTS_REV}/ufl/ubuntu")

function(flix_stage_font filename sha256)
  set(destination "${FLIX_DATA_DIR}/${filename}")
  if(EXISTS "${destination}")
    file(SHA256 "${destination}" existing_hash)
    if(existing_hash STREQUAL sha256)
      return()
    endif()
  endif()
  file(DOWNLOAD "${FLIX_GOOGLE_FONTS_ROOT}/${filename}" "${destination}"
       EXPECTED_HASH "SHA256=${sha256}" TLS_VERIFY ON STATUS download_status)
  list(GET download_status 0 download_code)
  list(GET download_status 1 download_message)
  if(NOT download_code EQUAL 0)
    message(FATAL_ERROR "Could not stage ${filename}: ${download_message}")
  endif()
endfunction()

flix_stage_font(Ubuntu-Regular.ttf
               3128df86a31805618436d0ae5651ba4285d0c9de0a39057d025f64ee33bceb64)
flix_stage_font(Ubuntu-Bold.ttf
               679b5c1e09cab3156bb8ef529735f9382bf31ca7ac737382ab959297f8d82ad4)

# The src data stays the single source of truth; stage it beside the binaries
# so both programs find one content directory.
#
# Modelled as a custom COMMAND with real outputs rather than a custom TARGET
# that always runs, because the web link has to be able to DEPEND on the
# staged files. See FLIX_DATA_STAGED's use under EMSCRIPTEN below: the wasm
# embeds this directory at link time, and a target-level dependency only
# ORDERS the two, it does not make the linked wasm out of date when a data
# file changes. That is how an edited mobs.json could be staged correctly and
# still never reach the shipped build -- the link was skipped as up to date and
# the wasm kept the copy it was born with.
set(FLIX_DATA_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mobs.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/petals.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/mob_drops.json
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/map_bundle.ts
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/land.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/desert.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ocean.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/hel.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/ant_hell.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/sewers.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/jungle.svg
    ${CMAKE_CURRENT_SOURCE_DIR}/data/mob_xp.json)

set(FLIX_DATA_STAGED)
foreach(source IN LISTS FLIX_DATA_SOURCES)
  get_filename_component(name "${source}" NAME)
  list(APPEND FLIX_DATA_STAGED ${FLIX_DATA_DIR}/${name})
endforeach()

# A plain `copy`, not `copy_if_different`: the outputs must come out strictly
# newer than the inputs, or every build would see them as still out of date and
# relink the wasm for nothing. The DEPENDS list is what keeps this from running
# when no data changed.
add_custom_command(
  OUTPUT ${FLIX_DATA_STAGED}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${FLIX_DATA_DIR}
  COMMAND ${CMAKE_COMMAND} -E copy ${FLIX_DATA_SOURCES} ${FLIX_DATA_DIR}/
  DEPENDS ${FLIX_DATA_SOURCES}
  COMMENT "Staging game content")
add_custom_target(flix_data ALL DEPENDS ${FLIX_DATA_STAGED})
add_dependencies(flowrix_client flix_data)
add_dependencies(flowrix_server flix_data)

# --- the web build's link step -----------------------------------------------
if(EMSCRIPTEN)
  # Content goes INSIDE the wasm, not beside it. --embed-file rather than
  # --preload-file, for three reasons: the client reads mobs.json, petals.json,
  # the map bundle, the biome SVGs and the fonts synchronously during start(),
  # so there is nothing to gain from a separate fetch it would have to wait for
  # anyway; a single artifact cannot half-deploy, which a .wasm and a stale
  # .data next to it can; and the server build has no page to run a preload
  # from at all. The whole set is ~1MB, which is smaller than the wasm.
  set(FLIX_EMBED_DATA "--embed-file" "${FLIX_DATA_DIR}@data")

  # The embed happens at LINK time, so the linked output is stale the moment a
  # staged data file is newer than it. Without this, editing mobs.json or
  # petals.json changes nothing you can see: the staging succeeds, the link is
  # skipped because no object file moved, and the wasm keeps the data it was
  # linked with. Naming the staged files as link dependencies is what makes a
  # data-only edit force a relink.
  set_property(TARGET flowrix_client APPEND PROPERTY LINK_DEPENDS ${FLIX_DATA_STAGED})
  set_property(TARGET flowrix_server APPEND PROPERTY LINK_DEPENDS ${FLIX_DATA_STAGED})

  # Shared by both web targets. Memory grows because game state and transient
  # command data are not fixed-size; browser rendering itself stays in the
  # browser's Canvas2D surfaces rather than a Wasm software framebuffer.
  set(FLIX_WEB_LINK_OPTIONS
  "-sEXIT_RUNTIME=0"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sINITIAL_MEMORY=256MB"
  )

  # Emscripten's own checks are the web half of the flavour: out-of-bounds
  # heap access, a stack overflow and a bad pointer passed to the runtime are
  # all silent corruption without them and a named error with them. Stated
  # either way rather than left to whatever the optimisation level implies.
  #
  # The link-time -O is a separate question from the compile-time one: it is
  # what runs wasm-opt over the module and minifies the glue. The client is the
  # half of this build whose frame rate pays for that, so a release link is
  # pinned at -O3 whatever the configuration's own flags say, and a dev link is
  # left to take them -- wasm-opt at -O3 is most of the wait on an incremental
  # relink, and there is nothing to measure in a build being iterated on.
  if(FLIX_BUILD STREQUAL "release")
    list(APPEND FLIX_WEB_LINK_OPTIONS "-sASSERTIONS=0")
    set(FLIX_WEB_CLIENT_OPT "-O3")
  else()
    list(APPEND FLIX_WEB_LINK_OPTIONS "-sASSERTIONS=1" "-sSTACK_OVERFLOW_CHECK=1")
    set(FLIX_WEB_CLIENT_OPT "")
  endif()

  target_compile_definitions(flix_shared PUBLIC FLIX_WEB_BUILD)

  # The web build's artifacts are what dist/ ships, and dist/ is served and run
  # by name: the page is index.html, the script beside it bundle.js, and the
  # program `npm start` runs server.js -- the names the TypeScript build used
  # before the C++ client and server took over, so nothing downstream (nginx,
  # pm2, the autoupdate zipball) has to learn new ones. Emscripten writes the
  # .wasm filename into the glue and the glue's filename into the page, so the
  # rename belongs here, at the link, rather than in a copy step afterwards.
  # Only the web build is renamed: the native binaries stay flowrix_client and
  # flowrix_server, which is what the parity rigs and the docs invoke.
  set_target_properties(flowrix_client PROPERTIES OUTPUT_NAME bundle SUFFIX ".html")
  set_target_properties(flowrix_server PROPERTIES OUTPUT_NAME server)
  target_compile_options(flowrix_client PRIVATE
   "-flto"
   "-fno-rtti"
   "-fno-exceptions"
  )
  target_compile_options(flowrix_server PRIVATE "-flto")
  target_link_options(flowrix_client PRIVATE
    ${FLIX_WEB_LINK_OPTIONS}
    # Every draw goes through the page's own 2D context. There is deliberately
    # no SDL or WebGL compatibility backend in this target.
    "-sENVIRONMENT=web"
    # The newer filesystem, for its pluggable backends: the client mounts one
    # of its own over browser storage so that a reload does not forget the
    # settings and the session token. The legacy FS has no equivalent that
    # works here -- IDBFS is asynchronous and needs an explicit syncfs, and
    # WasmFS's own OPFS backend needs threads, which a client that draws every
    # frame through the page's 2D context cannot have.
    "-sWASMFS"
    "-flto"
    "-msimd128"
    "-sEVAL_CTORS=0"
    "-fno-rtti -fno-exceptions"
    ${FLIX_WEB_CLIENT_OPT}
    ${FLIX_EMBED_DATA}
    "--shell-file" "${CMAKE_CURRENT_SOURCE_DIR}/client/web/shell.html")
  set_property(TARGET flowrix_client APPEND PROPERTY LINK_DEPENDS
               "${CMAKE_CURRENT_SOURCE_DIR}/client/web/shell.html")

  # The server runs under Node, where it can listen. It is the same program as
  # the native one -- the same tick, the same systems, the same database -- and
  # differs only in what carries the bytes: net/web_channel.cpp accepts
  # WebSocket and, when a certificate is configured, WebTransport sessions.
  # .js, not .mjs: the emitted module has to be CommonJS, because the transport
  # reaches for Node's `http`, `crypto` and `fs` with require() from inside
  # EM_JS. Those are built in, and they are all it reaches for -- the WebSocket
  # server is written out in net/web_channel.cpp rather than taken from npm --
  # so what dist/ needs beside it to run is nothing at all.
  target_link_options(flowrix_server PRIVATE
    ${FLIX_WEB_LINK_OPTIONS}
    "-sENVIRONMENT=node"
    # Emscripten's default stack is 64KB, against the 8MB a native thread gets,
    # and the JSON parser and the Json destructor both recurse once per level
    # of the document. The account database nests deeply enough that the
    # default stack runs out part-way through loading a real one -- silently,
    # in a release build, as an out-of-bounds access with no name on it. The
    # parser's own kMaxDepth of 200 is the bound on how far this has to go.
    "-sSTACK_SIZE=4194304"
    # The database is a real file that has to survive a restart, so the one
    # directory it lives in is mounted from the host filesystem. Everything
    # else -- all of the embedded content above -- stays in memory.
    "-sFORCE_FILESYSTEM=1"
    "-flto"
    "-msimd128"
    "-sEVAL_CTORS=0"
    "-lnodefs.js"
    "-sEXPORTED_RUNTIME_METHODS=['FS']"
    ${FLIX_EMBED_DATA})
endif()

# --- tools -------------------------------------------------------------------
# Native only: two write a PPM from a headless rasterizer, the other runs the
# server's own systems.
if(NOT EMSCRIPTEN)
add_executable(svg_smoke tools/svg_smoke.cpp)
target_link_libraries(svg_smoke PRIVATE flix_shared cpp_canvas)

# The rasterizer's fingerprint. Build it either side of a canvas.cpp change and
# diff the two PPMs: the coverage maths is not what any of those changes are
# for, so a correct one moves no bytes at all. See the file's own header.
add_executable(raster_parity tools/raster_parity.cpp)
target_link_libraries(raster_parity PRIVATE cpp_canvas)

add_executable(behavior_oracle tools/behavior_oracle.cpp)
target_link_libraries(behavior_oracle PRIVATE flix_server)

# --- tests -------------------------------------------------------------------
enable_testing()
# Globbed deliberately: tests are added often and a stale list is a test that
# silently stops running. CONFIGURE_DEPENDS re-globs when the directory changes.
file(GLOB FLIX_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp)
add_executable(flix_tests ${FLIX_TEST_SOURCES})
# Tests cover both halves, including the end-to-end ones that run a real
# server and real clients over loopback.
target_link_libraries(flix_tests PRIVATE flix_server flix_client)
target_compile_definitions(flix_tests PRIVATE FLIX_TEST_DATA_DIR="${FLIX_DATA_DIR}")
add_dependencies(flix_tests flix_data)
add_test(NAME flix_tests COMMAND flix_tests)
endif()

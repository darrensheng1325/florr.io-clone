#include "client/web/persist.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <unordered_map>
#include <vector>

#include <emscripten.h>
#include <emscripten/wasmfs.h>
#include <sys/stat.h>
#include <unistd.h>

// $wasmFS$backends is the map WasmFS keeps of backend address -> the JS object
// implementing it. It is a JS library symbol, so it is only emitted if
// something asks for it, and EM_JS bodies are not scanned for dependencies.
EM_JS_DEPS(flix_persist, "$wasmFS$backends");

namespace flix::web {
namespace {

void (*g_flush)() = nullptr;

/// What mirrorFile() last saw under each key, or what restoreFile() put
/// there. A mirror whose bytes match this is skipped, which is what makes
/// calling it once a second free.
std::unordered_map<std::string, std::string>& mirrored() {
    static std::unordered_map<std::string, std::string> table;
    return table;
}

/// Installs Module.flixStorage, the one place localStorage is touched: the
/// base64 codec, and get/set of a byte array under a key. Both the WasmFS
/// backend below and the path-based mirror go through it, so the encoding is
/// stated once.
///
/// Returns 0 if the origin has no storage to give -- a private window, or
/// cookies blocked -- in which case nothing is installed and every later call
/// answers "no storage".
EM_JS(int, installStorage, (), {
    if (Module.flixStorage) return Module.flixStorage.available ? 1 : 0;

    // Touching localStorage is what throws when it is unavailable, not using
    // it, so the probe has to be a real write.
    let available = true;
    try {
        const probe = 'flowrix-probe';
        localStorage.setItem(probe, '1');
        localStorage.removeItem(probe);
    } catch (e) {
        available = false;
    }

    // localStorage holds strings, so the bytes go in base64. The files are
    // text today, but a backend that only survives ASCII is a trap for the
    // next thing stored here.
    const decode = (text) => {
        const binary = atob(text);
        const bytes = new Uint8Array(binary.length);
        for (let i = 0; i < binary.length; ++i) bytes[i] = binary.charCodeAt(i);
        return bytes;
    };
    const encode = (bytes) => {
        // Double quotes for the empty string: the C preprocessor sees this
        // body before JS does, and '' is an empty character constant to it.
        let binary = "";
        // In slices: String.fromCharCode.apply over a whole database would
        // overflow the argument list, and a character at a time is slow for
        // one.
        for (let at = 0; at < bytes.length; at += 8192) {
            binary += String.fromCharCode.apply(null, bytes.subarray(at, at + 8192));
        }
        return btoa(binary);
    };

    Module.flixStorage = {
        available: available,
        // Bytes decoded by size() and waiting for take(); see the C++ side.
        staged: null,
        get: (key) => {
            try {
                const saved = localStorage.getItem(key);
                return saved === null ? null : decode(saved);
            } catch (e) {
                console.warn('[persist] could not read ' + key + ': ' + e);
                return null;
            }
        },
        set: (key, bytes) => {
            try {
                localStorage.setItem(key, encode(bytes));
                return true;
            } catch (e) {
                // A full or read-only quota. The client is still playable, so
                // this is a warning rather than a failure the game has to
                // handle.
                console.warn('[persist] could not save ' + key + ': ' + e);
                return false;
            }
        },
    };
    return available ? 1 : 0;
});

/// Installs the JS half of the backend: WasmFS calls into these for every read
/// and write of a file that lives in it.
///
/// The bytes are kept in a typed array per open file and mirrored into
/// localStorage after each change, rather than being re-encoded out of storage
/// on every read. Both files are a few hundred bytes, so the copy is free and
/// reads stay a subarray away.
EM_JS(void, installBackend, (void* backend, const char* prefixPtr), {
    const prefix = UTF8ToString(prefixPtr);
    const storage = Module.flixStorage;

    // File address -> { name, bytes }. `name` is null for a file this backend
    // was never told the name of, which is stored in memory and nowhere else.
    const files = {};
    const save = (entry) => storage.set(prefix + entry.name, entry.bytes);

    const impl = {
        // The name the next file created in this backend is to be stored
        // under, set by expectNext() immediately before the file is made.
        pending: null,

        allocFile: (file) => {
            const name = impl.pending;
            impl.pending = null;
            let bytes = new Uint8Array(0);
            if (name !== null) bytes = storage.get(prefix + name) || bytes;
            files[file] = { name: name, bytes: bytes };
        },
        freeFile: (file) => { delete files[file]; },

        read: (file, buffer, length, offset) => {
            const entry = files[file];
            if (!entry) return -5;  // EIO
            const available = Math.max(0, entry.bytes.length - offset);
            length = Math.min(length, available);
            HEAPU8.set(entry.bytes.subarray(offset, offset + length), buffer);
            return length;
        },
        write: (file, buffer, length, offset) => {
            const entry = files[file];
            if (!entry) return -5;  // EIO
            if (offset + length > entry.bytes.length) {
                const grown = new Uint8Array(offset + length);
                grown.set(entry.bytes);
                entry.bytes = grown;
            }
            entry.bytes.set(HEAPU8.subarray(buffer, buffer + length), offset);
            if (entry.name !== null) save(entry);
            return length;
        },
        getSize: (file) => files[file] ? files[file].bytes.length : 0,
        setSize: (file, size) => {
            const entry = files[file];
            if (!entry) return -5;  // EIO
            const resized = new Uint8Array(size);
            resized.set(entry.bytes.subarray(0, Math.min(size, entry.bytes.length)));
            entry.bytes = resized;
            if (entry.name !== null) save(entry);
            return 0;
        },
    };

    wasmFS$backends[backend] = impl;
});

/// Names the storage key the next file created in this backend belongs to.
///
/// WasmFS hands a backend the address of a new file, never its path -- the
/// name belongs to the directory, which is filled in after the backend has
/// already been asked to make the file. So the name is passed sideways, set
/// here and picked up by the allocFile hook that the create below runs
/// synchronously.
EM_JS(void, expectNext, (void* backend, const char* namePtr), {
    wasmFS$backends[backend].pending = UTF8ToString(namePtr);
});

// The path-based half. A stored value comes back in two calls: size() decodes
// it and reports the length, the caller sizes a buffer, take() fills it. The
// obvious single call -- return a malloc'd buffer -- needs _malloc exported to
// JavaScript, which is a link setting nothing else here asks for.
EM_JS(int, storedSize, (const char* keyPtr), {
    const storage = Module.flixStorage;
    const bytes = storage.get(UTF8ToString(keyPtr));
    if (!bytes) return -1;
    storage.staged = bytes;
    return bytes.length;
});

EM_JS(void, storedTake, (char* out, int capacity), {
    const storage = Module.flixStorage;
    const bytes = storage.staged;
    storage.staged = null;
    if (bytes) HEAPU8.set(bytes.subarray(0, capacity), out);
});

EM_JS(int, storedPut, (const char* keyPtr, const char* data, int size), {
    return Module.flixStorage.set(UTF8ToString(keyPtr), HEAPU8.subarray(data, data + size)) ? 1 : 0;
});

EM_JS(void, installPageHide, (), {
    // pagehide covers a reload, a navigation and a close on the desktop.
    // Mobile browsers routinely kill a backgrounded tab without firing it, and
    // a hidden visibilitychange is the last event those do deliver, so both
    // are watched. Saving twice costs one localStorage write.
    const flush = () => _flix_persist_flush();
    addEventListener('pagehide', flush);
    addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'hidden') flush();
    });
});

} // namespace

const char* const kStorageDirectory = "/persist";

bool mountStorage(const std::vector<std::string>& names, const std::string& prefix) {
    if (!installStorage()) return false;
    backend_t backend = wasmfs_create_jsimpl_backend();
    installBackend(backend, prefix.c_str());

    if (wasmfs_create_directory(kStorageDirectory, 0777, backend) != 0) return false;

    // Created now, once, rather than left to the first save.
    //
    // A file made later -- by the ofstream in ClientSettings::save, say --
    // would still land in this backend, because a directory's backend is what
    // makes its children. But nothing would have told the backend its name, so
    // it would be stored in memory and lost, silently. Creating them here is
    // what pairs every file in this directory with a key, and it is why
    // App::logout truncates the session file rather than removing it: a
    // removed file takes its name binding with it.
    for (const std::string& name : names) {
        expectNext(backend, name.c_str());
        const int fd = wasmfs_create_file((std::string(kStorageDirectory) + "/" + name).c_str(),
                                          0666, backend);
        if (fd >= 0) ::close(fd);
    }
    return true;
}

bool restoreFile(const std::string& key, const std::string& path) {
    if (!installStorage()) return false;
    const int size = storedSize(key.c_str());
    if (size < 0) return false;
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size > 0) storedTake(bytes.data(), size);

    // The file's directory, made if missing. One level is enough: the caller
    // names a file in a directory of its own, not a tree.
    const std::size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0) ::mkdir(path.substr(0, slash).c_str(), 0777);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) return false;
    mirrored()[key] = std::move(bytes);
    return true;
}

bool mirrorFile(const std::string& key, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    auto& table = mirrored();
    const auto seen = table.find(key);
    if (seen != table.end() && seen->second == bytes) return true;

    if (!installStorage()) return false;
    if (!storedPut(key.c_str(), bytes.data(), static_cast<int>(bytes.size()))) return false;
    table[key] = std::move(bytes);
    return true;
}

void onPageHide(void (*flush)()) {
    g_flush = flush;
    installPageHide();
}

} // namespace flix::web

/// Called from the page's own unload handlers. Exported rather than passed as
/// a function pointer so the JS above can name it without a dyncall.
extern "C" EMSCRIPTEN_KEEPALIVE void flix_persist_flush() {
    if (flix::web::g_flush) flix::web::g_flush();
}

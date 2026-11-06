// The game's data as files in the game folder, on a virtual disc (disc.h).
//
// The game asks for a file's start with sceCdSearchFile and adds its own offsets, so a file can sit at any sector.
//
// With data/index.txt (tools/recomp/unpack_data.py) ZDIR.BIN and ZZDATA.BIN are not files any more: they are built here from data/, in
// the index's order and with each file's size now, so replacing a file is all it takes. HP2_DISCCHECK=1 compares what is built with
// the real two files in NFSHP2/ and exits. HP2_FILELOG=<file> writes down each file of data/ when the game first reads it.
#include "disc.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "host.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#else
#define fseek64 fseeko
#endif

static const u32 SECTOR = 2048;
static const u32 FIRST_LSN = 1000;      // not the retail disc's places: nothing may depend on them

// a file of data/, in the built ZZDATA
struct Part {
    std::string rel, host;
    u64 pos;
    u32 size;
    std::unique_ptr<std::atomic<bool>> seen;
};

struct Entry {
    std::string name;                   // the way the game names it: \NFSHP2\ZZDATA.BIN
    std::string host;                   // empty for the two built ones
    u32 lsn, size;
    const std::vector<u8> *memory;      // the built ZDIR
    bool parts;                         // the built ZZDATA
};
static std::vector<Entry> g_entries;    // by lsn
static std::vector<Part> g_parts;       // by pos
static std::vector<u8> g_zdir;
static FILE *g_file_log;
static std::chrono::steady_clock::time_point g_start;

static u32 sectors_of(u32 size) { return (u32)(((u64)size + SECTOR - 1) / SECTOR); }

// Each thread keeps its own handles, the EE reads while the IOP thread streams; a few, there can be hundreds of files.
struct OpenFile {
    const std::string *host;
    FILE *f;
    u64 used;
};
static FILE *open_file(const std::string &host)
{
    static thread_local OpenFile open[16];
    static thread_local u64 clock;
    OpenFile *oldest = &open[0];
    for (OpenFile &o : open) {
        if (o.f && o.host == &host) {
            o.used = ++clock;
            return o.f;
        }
        if (o.used < oldest->used) oldest = &o;
    }
    if (oldest->f) fclose(oldest->f);
    oldest->f = fopen(host.c_str(), "rb");
    oldest->host = &host;
    oldest->used = ++clock;
    return oldest->f;
}

static u32 read_host(const std::string &host, u64 pos, u32 bytes, void *dst)
{
    FILE *f = open_file(host);
    if (!f) return 0;
    fseek64(f, (long long)pos, SEEK_SET);
    return (u32)fread(dst, 1, bytes, f);
}

static std::string normalized(std::string path)
{
    size_t colon = path.find(':');
    if (colon != std::string::npos) path = path.substr(colon + 1);
    path = path.substr(0, path.find(';'));
    for (char &ch : path) ch = ch == '/' ? '\\' : (char)toupper((unsigned char)ch);
    return path[0] == '\\' ? path : "\\" + path;
}

// ZDIR and ZZDATA out of data/: {hash, sector, size} for each file, and each file from a new sector
static bool build_archive(const std::filesystem::path &data)
{
    namespace fs = std::filesystem;
    std::ifstream index(data / "index.txt");
    if (!index) return false;
    std::string line;
    u64 sector = 0;
    std::error_code ec;
    while (std::getline(index, line)) {
        if (line.size() < 10) continue;
        const u32 hash = (u32)strtoul(line.c_str(), nullptr, 16);
        const std::string rel = line.substr(9);
        const fs::path host = data / fs::path(rel);
        const u64 size = fs::file_size(host, ec);
        if (ec) {
            fprintf(stderr, "disc: data/%s is missing\n", rel.c_str());
            exit(1);
        }
        const u32 row[3] = {hash, (u32)sector, (u32)size};
        g_zdir.insert(g_zdir.end(), (const u8 *)row, (const u8 *)row + 12);
        Part p;
        p.rel = rel;
        p.host = host.string();
        p.pos = sector * SECTOR;
        p.size = (u32)size;
        p.seen = std::make_unique<std::atomic<bool>>(false);
        g_parts.push_back(std::move(p));
        sector += sectors_of((u32)size);
    }
    if (sector * SECTOR > 0xFFFFFFFFull) {
        fprintf(stderr, "disc: data/ has grown past 4 GB\n");
        exit(1);
    }
    g_entries.push_back({"\\NFSHP2\\ZDIR.BIN", "", 0, (u32)g_zdir.size(), &g_zdir, false});
    g_entries.push_back({"\\NFSHP2\\ZZDATA.BIN", "", 0, (u32)(sector * SECTOR), nullptr, true});
    if (const char *log = getenv("HP2_FILELOG")) g_file_log = fopen(log, "w");
    g_start = std::chrono::steady_clock::now();
    return true;
}

// from pos on in the built ZZDATA; between the files, zeros
static void read_parts(u64 pos, u32 bytes, u8 *dst)
{
    auto it = std::upper_bound(g_parts.begin(), g_parts.end(), pos, [](u64 at, const Part &p) { return at < p.pos; });
    if (it != g_parts.begin()) --it;
    memset(dst, 0, bytes);
    const u64 end = pos + bytes;
    for (; it != g_parts.end() && it->pos < end; ++it) {
        const u64 from = std::max(pos, it->pos), to = std::min(end, it->pos + it->size);
        if (from >= to) continue;
        read_host(it->host, from - it->pos, (u32)(to - from), dst + (from - pos));
        if (g_file_log && !it->seen->exchange(true)) {
            fprintf(g_file_log, "%7.1f %s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count(), it->rel.c_str());
            fflush(g_file_log);
        }
    }
}

// All of what is built against the real ZDIR.BIN and ZZDATA.BIN: the check of data/ and of this file.
static void check_archive(const std::filesystem::path &root)
{
    namespace fs = std::filesystem;
    std::vector<u8> zdir(g_zdir.size());
    const fs::path real = root / "NFSHP2";
    FILE *f = fopen((real / "ZDIR.BIN").string().c_str(), "rb");
    if (!f) {
        fprintf(stderr, "disccheck: no NFSHP2/ZDIR.BIN\n");
        exit(1);
    }
    const size_t got = fread(zdir.data(), 1, zdir.size(), f);
    fclose(f);
    bool ok = got == zdir.size() && zdir == g_zdir;
    fprintf(stderr, "disccheck: ZDIR %s, %zu bytes\n", ok ? "the same" : "DIFFERS", g_zdir.size());
    f = fopen((real / "ZZDATA.BIN").string().c_str(), "rb");
    if (!f) {
        fprintf(stderr, "disccheck: no NFSHP2/ZZDATA.BIN\n");
        exit(1);
    }
    u64 total = 0, differ = 0;
    std::vector<u8> a(1u << 22), b(1u << 22);
    const u64 size = g_parts.empty() ? 0 : ((g_parts.back().pos + g_parts.back().size + SECTOR - 1) / SECTOR) * SECTOR;
    for (u64 pos = 0; pos < size; pos += a.size()) {
        const u32 n = (u32)std::min<u64>(a.size(), size - pos);
        const size_t real_n = fread(a.data(), 1, n, f);
        read_parts(pos, n, b.data());
        total += n;
        if (real_n != n || memcmp(a.data(), b.data(), n)) {
            for (u32 i = 0; i < real_n; i++) differ += a[i] != b[i];
            differ += n - real_n;
        }
    }
    char extra;
    const bool longer = fread(&extra, 1, 1, f) == 1;
    fclose(f);
    fprintf(stderr, "disccheck: ZZDATA %llu bytes, %llu differ%s\n", (unsigned long long)total, (unsigned long long)differ, longer ? ", the real one is longer" : "");
    fflush(stderr);
    _Exit(ok && !differ && !longer ? 0 : 2);
}

bool disc_open()
{
    namespace fs = std::filesystem;
    const char *env = getenv("HP2_DATA");
    fs::path root = env ? fs::path(env) : fs::path(host_exe_dir());
    const bool built = build_archive(root / "data");
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root / "NFSHP2", ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec)) {
            std::string name = normalized(fs::relative(it->path(), root, ec).string());
            if (built && (name == "\\NFSHP2\\ZDIR.BIN" || name == "\\NFSHP2\\ZZDATA.BIN")) continue;
            g_entries.push_back({name, it->path().string(), 0, (u32)it->file_size(ec), nullptr, false});
        }
    // the IOP's sound module: data\IRX253\STREAM.IRX where there is no NFSHP2\ folder (with data\ the folder's two large files are not read)
    if (std::none_of(g_entries.begin(), g_entries.end(), [](const Entry &e) { return e.name == "\\NFSHP2\\IRX253\\STREAM.IRX"; })) {
        const fs::path irx = root / "data" / "IRX253" / "STREAM.IRX";
        if (fs::is_regular_file(irx, ec))
            g_entries.push_back({"\\NFSHP2\\IRX253\\STREAM.IRX", irx.string(), 0, (u32)fs::file_size(irx, ec), nullptr, false});
    }
    std::sort(g_entries.begin(), g_entries.end(), [](const Entry &a, const Entry &b) { return a.name < b.name; });
    u32 lsn = FIRST_LSN;
    for (Entry &e : g_entries) {
        e.lsn = lsn;
        lsn += sectors_of(e.size);
    }
    if (built) {
        fprintf(stderr, "disc: %zu files from data/\n", g_parts.size());
        if (getenv("HP2_DISCCHECK")) check_archive(root);
    }
    DiscFile f;
    return disc_find("\\NFSHP2\\ZDIR.BIN", f) && disc_find("\\NFSHP2\\ZZDATA.BIN", f) &&
           disc_find("\\NFSHP2\\IRX253\\STREAM.IRX", f);
}

bool disc_find(const std::string &path, DiscFile &out)
{
    std::string name = normalized(path);
    for (const Entry &e : g_entries)
        if (e.name == name) {
            out = {e.lsn, e.size};
            return true;
        }
    return false;
}

// from pos on, as far as the file goes
static u32 read_entry(size_t index, u64 pos, u32 bytes, void *dst)
{
    const Entry &e = g_entries[index];
    if (pos >= e.size) return 0;
    if (bytes > e.size - pos) bytes = (u32)(e.size - pos);
    if (e.memory) {
        memcpy(dst, e.memory->data() + pos, bytes);
        return bytes;
    }
    if (e.parts) {
        read_parts(pos, bytes, (u8 *)dst);
        return bytes;
    }
    return read_host(e.host, pos, bytes, dst);
}

bool disc_read(u32 lsn, u32 sectors, void *dst)
{
    u32 bytes = sectors * SECTOR;
    memset(dst, 0, bytes);                  // the end of a file's last sector
    auto it = std::upper_bound(g_entries.begin(), g_entries.end(), lsn,
                               [](u32 l, const Entry &e) { return l < e.lsn; });
    if (it == g_entries.begin()) return false;
    --it;
    if (lsn + sectors > it->lsn + sectors_of(it->size)) {
        fprintf(stderr, "disc: read past %s: lsn %u x%u\n", it->name.c_str(), lsn, sectors);
        return false;
    }
    u64 pos = (u64)(lsn - it->lsn) * SECTOR;
    u32 want = pos + bytes > it->size ? (u32)(it->size - pos) : bytes;
    return read_entry((size_t)(it - g_entries.begin()), pos, bytes, dst) == want;
}

u32 disc_read_file(const DiscFile &f, u32 pos, u32 bytes, void *dst)
{
    for (size_t i = 0; i < g_entries.size(); i++)
        if (g_entries[i].lsn == f.lsn) return read_entry(i, pos, bytes, dst);
    return 0;
}

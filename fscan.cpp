#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <future>
#include <glob.h>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// ---------------------------------------------------------------------------
//  Config
// ---------------------------------------------------------------------------
struct Config {
    std::vector<std::string> paths;

    bool show_extensions = false;
    bool show_size       = false;
    bool show_depth      = false;
    bool include_hidden   = false;
    bool follow_symlinks  = false;
    bool csv_output       = false;
    bool tree_output      = false;
    bool text_only        = false;
    bool align_output     = false;
    bool dirs_only        = false;
    bool find_mode        = false;  // list individual files with sizes

    // find mode unit: 0=auto, 1=B, 2=KB, 3=MB, 4=GB, 5=TB
    int find_unit           = 0;
    int find_unit_w          = 0;   // max width for the unit string

    int max_depth        = -1;
    int thread_count     = 0;
    std::time_t since    = 0;
    std::time_t until    = 0;
    std::uint64_t min_size = 0;
    std::uint64_t max_size = 0;  // 0 = unlimited

    std::vector<std::string> exclude_patterns;
};

// ---------------------------------------------------------------------------
//  Formatting helpers
// ---------------------------------------------------------------------------
static std::string fmt_num(std::uint64_t n) {
    return std::to_string(n);
}

static std::string fmt_size_str(std::uint64_t bytes) {
    static const char* units[] = {"B","KB","MB","GB","TB"};
    int u = 0;
    double d = (double)bytes;
    while (d >= 1024.0 && u < 4) { d /= 1024.0; ++u; }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof buf, "%lu B", (unsigned long)bytes);
    else        snprintf(buf, sizeof buf, "%.0f %s", d, units[u]);
    return buf;
}

// Format size for find mode: value right-aligned in given width, unit suffix
static std::string fmt_find_size(std::uint64_t bytes, int unit) {
    if (unit == 0) {
        // auto: pick best unit, show suffix, integer
        return fmt_size_str(bytes);
    }
    static const char* units[] = {"","B","KB","MB","GB","TB"};
    double d = (double)bytes;
    for (int i = 1; i < unit; ++i) d /= 1024.0;
    char buf[32];
    if (unit == 1) snprintf(buf, sizeof buf, "%lu %s", (unsigned long)bytes, units[unit]);
    else           snprintf(buf, sizeof buf, "%.0f %s", d, units[unit]);
    return buf;
}

// ---------------------------------------------------------------------------
//  FileInfo – aggregated stats for one path
// ---------------------------------------------------------------------------
struct FileInfo {
    std::uint64_t count      = 0;
    std::uint64_t total_size = 0;
    int max_depth            = 0;
    std::unordered_map<std::string, std::uint64_t> ext_cnt;
    std::unordered_map<std::string, std::uint64_t> ext_sz;
};

static FileInfo& merge(FileInfo& a, const FileInfo& b) {
    a.count      += b.count;
    a.total_size += b.total_size;
    if (b.max_depth > a.max_depth) a.max_depth = b.max_depth;
    for (auto& p : b.ext_cnt)  a.ext_cnt[p.first]  += p.second;
    for (auto& p : b.ext_sz)   a.ext_sz[p.first]   += p.second;
    return a;
}

// ---------------------------------------------------------------------------
//  Helpers: hidden, extension, excluded
// ---------------------------------------------------------------------------
static bool is_hidden(const std::string& name) {
    return !name.empty() && name[0] == '.';
}

static std::string file_ext(const std::string& name) {
    auto dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        return name.substr(dot);
    return "(none)";
}

// Simple pattern match: supports * and ? glob chars
static bool match_pattern(const char* pat, const char* str) {
    // naive recursive glob
    const char* s = str, *p = pat;
    for (;; ++s, ++p) {
        if (*p == '*') {
            while (*p == '*') ++p;
            if (!*p) return true;
            for (const char* ss = s; ; ++ss) {
                if (match_pattern(p, ss)) return true;
                if (!*ss) return false;
            }
        }
        if (*p != *s && *p != '?') return !*p && !*s;
        if (!*s) return !*p;
    }
}

static bool is_excluded(const std::string& name,
                        const std::vector<std::string>& pats) {
    for (auto& p : pats)
        if (match_pattern(p.c_str(), name.c_str())) return true;
    return false;
}

// ---------------------------------------------------------------------------
//  Time parsing
// ---------------------------------------------------------------------------
static std::time_t parse_time(const char* s) {
    struct tm tm{};
    if (strptime(s, "%Y-%m-%dT%H:%M:%S", &tm)) return mktime(&tm);
    if (strptime(s, "%Y-%m-%d", &tm)) return mktime(&tm);
    std::cerr << "fscan: invalid time '" << s
              << "' (use YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS)\n";
    return 0;
}

// Parse human-readable size: 1G, 500M, 100K, etc.
static std::uint64_t parse_size(const char* s) {
    char* end;
    double val = strtod(s, &end);
    if (end == s) {
        std::cerr << "fscan: invalid size '" << s << "'\n";
        return 0;
    }
    switch (*end) {
        case 'T': case 't': val *= 1024ULL * 1024 * 1024 * 1024; break;
        case 'G': case 'g': val *= 1024ULL * 1024 * 1024; break;
        case 'M': case 'm': val *= 1024ULL * 1024; break;
        case 'K': case 'k': val *= 1024ULL; break;
        case 'B': case 'b': break;
        case '\0': break;
        default:
            std::cerr << "fscan: invalid size unit '" << *end << "' (use B/K/M/G/T)\n";
            return 0;
    }
    return (std::uint64_t)val;
}

// Detect if a file is text by checking for null bytes in the first 8KB
static bool is_text_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    unsigned char buf[8192];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == 0) return true;  // empty file counts as text
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == '\0') return false;
        // also reject files with lots of high bytes that look binary
        // (but allow common encodings – only check for NUL)
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Walk a directory tree (called per-subdir by parallel dispatcher)
// ---------------------------------------------------------------------------
static void walk(const char* dir, int cur_depth, FileInfo& info,
                 const Config& cfg) {
    DIR* dp = opendir(dir);
    if (!dp) return;

    struct dirent* entry;
    while ((entry = readdir(dp))) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        std::string name(entry->d_name);
        if (!cfg.include_hidden && is_hidden(name)) continue;
        if (is_excluded(name, cfg.exclude_patterns)) continue;

        std::string full = std::string(dir) + "/" + name;

        struct stat st;
        if (cfg.follow_symlinks) {
            if (::stat(full.c_str(), &st) != 0) continue;
        } else {
            if (::lstat(full.c_str(), &st) != 0) continue;
        }

        if (cfg.since && st.st_mtime < cfg.since) continue;
        if (cfg.until && st.st_mtime > cfg.until) continue;

        if (S_ISDIR(st.st_mode)) {
            if (cfg.max_depth < 0 || cur_depth + 1 <= cfg.max_depth)
                walk(full.c_str(), cur_depth + 1, info, cfg);
        } else if (S_ISREG(st.st_mode)) {
            if (cfg.text_only && !is_text_file(full.c_str())) continue;
            if (cfg.min_size > 0 && (std::uint64_t)st.st_size < cfg.min_size) continue;
            if (cfg.max_size > 0 && (std::uint64_t)st.st_size > cfg.max_size) continue;
            info.count++;
            info.total_size += (std::uint64_t)st.st_size;
            std::string ext = file_ext(name);
            info.ext_cnt[ext]++;
            info.ext_sz[ext] += (std::uint64_t)st.st_size;
            if (cur_depth > info.max_depth) info.max_depth = cur_depth;
        }
    }
    closedir(dp);
}

// ---------------------------------------------------------------------------
//  Find mode: walk and print each file with its size
// ---------------------------------------------------------------------------
static std::mutex find_mtx;

static void find_walk(const char* dir, const Config& cfg) {
    DIR* dp = opendir(dir);
    if (!dp) return;

    struct dirent* entry;
    while ((entry = readdir(dp))) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0')))
            continue;

        std::string name(entry->d_name);
        if (!cfg.include_hidden && is_hidden(name)) continue;
        if (is_excluded(name, cfg.exclude_patterns)) continue;

        std::string full = std::string(dir) + "/" + name;
        struct stat st;
        if (cfg.follow_symlinks) {
            if (::stat(full.c_str(), &st) != 0) continue;
        } else {
            if (::lstat(full.c_str(), &st) != 0) continue;
        }

        if (cfg.since && st.st_mtime < cfg.since) continue;
        if (cfg.until && st.st_mtime > cfg.until) continue;

        if (S_ISDIR(st.st_mode)) {
            if (cfg.max_depth < 0 || true) // find always recurses into dirs
                find_walk(full.c_str(), cfg);
        } else if (S_ISREG(st.st_mode)) {
            if (cfg.text_only && !is_text_file(full.c_str())) continue;
            if (cfg.min_size > 0 && (std::uint64_t)st.st_size < cfg.min_size) continue;
            if (cfg.max_size > 0 && (std::uint64_t)st.st_size > cfg.max_size) continue;
            std::lock_guard<std::mutex> lk(find_mtx);
            printf("%*s  %s\n", cfg.find_unit_w,
                   fmt_find_size((std::uint64_t)st.st_size, cfg.find_unit).c_str(),
                   full.c_str());
            fflush(stdout);
        }
    }
    closedir(dp);
}

// Parallel find: split top-level subdirs across threads
static void find_path(const std::string& path, const Config& cfg) {
    struct stat st;
    if (cfg.follow_symlinks) {
        if (::stat(path.c_str(), &st) != 0) return;
    } else {
        if (::lstat(path.c_str(), &st) != 0) return;
    }

    if (S_ISREG(st.st_mode)) {
        if (cfg.text_only && !is_text_file(path.c_str())) return;
        if (cfg.min_size > 0 && (std::uint64_t)st.st_size < cfg.min_size) return;
        if (cfg.max_size > 0 && (std::uint64_t)st.st_size > cfg.max_size) return;
        printf("%*s  %s\n", cfg.find_unit_w,
               fmt_find_size((std::uint64_t)st.st_size, cfg.find_unit).c_str(),
               path.c_str());
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;

    // Collect top-level entries
    DIR* dp = opendir(path.c_str());
    if (!dp) return;

    struct dirent* ent;
    std::vector<std::string> subdirs;

    while ((ent = readdir(dp))) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        std::string name(ent->d_name);
        if (!cfg.include_hidden && is_hidden(name)) continue;
        if (is_excluded(name, cfg.exclude_patterns)) continue;

        std::string full = path + "/" + name;
        struct stat s;
        if (cfg.follow_symlinks) {
            if (::stat(full.c_str(), &s) != 0) continue;
        } else {
            if (::lstat(full.c_str(), &s) != 0) continue;
        }
        if (cfg.since && s.st_mtime < cfg.since) continue;
        if (cfg.until && s.st_mtime > cfg.until) continue;

        if (S_ISDIR(s.st_mode)) {
            subdirs.push_back(full);
        } else if (S_ISREG(s.st_mode)) {
            if (cfg.text_only && !is_text_file(full.c_str())) continue;
            if (cfg.min_size > 0 && (std::uint64_t)s.st_size < cfg.min_size) continue;
            if (cfg.max_size > 0 && (std::uint64_t)s.st_size > cfg.max_size) continue;
            printf("%*s  %s\n", cfg.find_unit_w,
                   fmt_find_size((std::uint64_t)s.st_size, cfg.find_unit).c_str(),
                   full.c_str());
            fflush(stdout);
        }
    }
    closedir(dp);

    if (subdirs.empty()) return;

    int nthreads = cfg.thread_count > 0
        ? cfg.thread_count : (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 4;
    nthreads = std::min(nthreads, (int)subdirs.size());

    std::vector<std::future<void>> futs;
    futs.reserve(nthreads);
    for (int t = 0; t < nthreads; ++t) {
        std::vector<std::string> batch;
        for (size_t i = (size_t)t; i < subdirs.size(); i += (size_t)nthreads)
            batch.push_back(subdirs[i]);
        if (batch.empty()) continue;
        futs.push_back(std::async(std::launch::async, [batch = std::move(batch), &cfg] {
            for (auto& d : batch)
                find_walk(d.c_str(), cfg);
        }));
    }
    for (auto& f : futs) f.get();
}

// ---------------------------------------------------------------------------
//  Count one path (parallel: splits top-level subdirs across threads)
// ---------------------------------------------------------------------------
static FileInfo count_path(const std::string& path, const Config& cfg) {
    FileInfo info;

    struct stat st;
    if (cfg.follow_symlinks) {
        if (::stat(path.c_str(), &st) != 0) return info;
    } else {
        if (::lstat(path.c_str(), &st) != 0) return info;
    }

    // Single regular file
    if (S_ISREG(st.st_mode)) {
        if (cfg.text_only && !is_text_file(path.c_str())) return info;
        if (cfg.min_size > 0 && (std::uint64_t)st.st_size < cfg.min_size) return info;
        if (cfg.max_size > 0 && (std::uint64_t)st.st_size > cfg.max_size) return info;
        info.count = 1;
        info.total_size = (std::uint64_t)st.st_size;
        std::string name = path;
        auto sl = path.find_last_of('/');
        if (sl != std::string::npos) name = path.substr(sl + 1);
        std::string ext = file_ext(name);
        info.ext_cnt[ext] = 1;
        info.ext_sz[ext] = info.total_size;
        return info;
    }
    if (!S_ISDIR(st.st_mode)) return info;

    // Collect top-level entries
    DIR* dp = opendir(path.c_str());
    if (!dp) return info;

    struct dirent* ent;
    std::vector<std::string> subdirs;

    while ((ent = readdir(dp))) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        std::string name(ent->d_name);
        if (!cfg.include_hidden && is_hidden(name)) continue;
        if (is_excluded(name, cfg.exclude_patterns)) continue;

        std::string full = path + "/" + name;
        struct stat s;
        if (cfg.follow_symlinks) {
            if (::stat(full.c_str(), &s) != 0) continue;
        } else {
            if (::lstat(full.c_str(), &s) != 0) continue;
        }
        if (cfg.since && s.st_mtime < cfg.since) continue;
        if (cfg.until && s.st_mtime > cfg.until) continue;

        if (S_ISDIR(s.st_mode)) {
            subdirs.push_back(full);
        } else if (S_ISREG(s.st_mode)) {
            if (cfg.text_only && !is_text_file(full.c_str())) continue;
            if (cfg.min_size > 0 && (std::uint64_t)s.st_size < cfg.min_size) continue;
            if (cfg.max_size > 0 && (std::uint64_t)s.st_size > cfg.max_size) continue;
            info.count++;
            info.total_size += (std::uint64_t)s.st_size;
            std::string ext = file_ext(name);
            info.ext_cnt[ext]++;
            info.ext_sz[ext] += (std::uint64_t)s.st_size;
        }
    }
    closedir(dp);

    if (subdirs.empty()) return info;

    // Parallel dispatch
    int nthreads = cfg.thread_count > 0
        ? cfg.thread_count : (int)std::thread::hardware_concurrency();
    if (nthreads <= 0) nthreads = 4;
    nthreads = std::min(nthreads, (int)subdirs.size());

    std::vector<FileInfo> tinfo(nthreads);
    std::vector<std::future<void>> futs;
    futs.reserve(nthreads);

    for (int t = 0; t < nthreads; ++t) {
        std::vector<std::string> batch;
        for (size_t i = (size_t)t; i < subdirs.size(); i += (size_t)nthreads)
            batch.push_back(subdirs[i]);
        if (batch.empty()) continue;

        futs.push_back(std::async(std::launch::async,
            [t, &tinfo, batch = std::move(batch), &cfg] {
                for (auto& d : batch)
                    walk(d.c_str(), 0, tinfo[t], cfg);
            }));
    }
    for (auto& f : futs) f.get();

    for (int t = 0; t < nthreads; ++t)
        merge(info, tinfo[t]);

    return info;
}

// ---------------------------------------------------------------------------
//  Path resolution (glob or literal)
// ---------------------------------------------------------------------------
static std::vector<std::string> resolve_arg(const char* arg) {
    std::string_view sv(arg);
    bool is_glob = sv.find_first_of("*?[{") != std::string_view::npos;

    if (!is_glob) {
        if (access(arg, F_OK) != 0) {
            std::cerr << "fscan: '" << arg << "': No such file or directory\n";
            return {};
        }
        return {arg};
    }

    ::glob_t g{};
    int rc = ::glob(arg, GLOB_ERR | GLOB_NOSORT, nullptr, &g);
    std::vector<std::string> result;
    if (rc == 0) {
        for (size_t i = 0; i < g.gl_pathc; ++i)
            result.emplace_back(g.gl_pathv[i]);
    } else if (rc != GLOB_NOMATCH) {
        std::cerr << "fscan: error expanding '" << arg << "'\n";
    }
    ::globfree(&g);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if (result.empty())
        std::cerr << "fscan: '" << arg << "': No match\n";
    return result;
}

// ---------------------------------------------------------------------------
//  Output: default
// ---------------------------------------------------------------------------
static void output_default(const std::string& name, const FileInfo& info,
                           const Config& cfg) {
    printf("%16s\t", fmt_num(info.count).c_str());
    if (cfg.show_size) {
        printf("%14s\t", fmt_size_str(info.total_size).c_str());
    }
    if (cfg.show_depth) {
        printf("%d\t", info.max_depth);
    }
    printf("%s\n", name.c_str());

    if (cfg.show_extensions && !info.ext_cnt.empty()) {
        printf("  extensions:\n");
        std::vector<std::pair<std::string, std::uint64_t>> sorted(
            info.ext_cnt.begin(), info.ext_cnt.end());
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b){ return a.second > b.second; });
        for (auto& p : sorted) {
            std::uint64_t sz = 0;
            auto it = info.ext_sz.find(p.first);
            if (it != info.ext_sz.end()) sz = it->second;
            printf("    %-14s %s  (%s)\n",
                   p.first.c_str(), fmt_num(p.second).c_str(),
                   fmt_size_str(sz).c_str());
        }
    }
}

// Truncate long names with ellipsis in the middle
static std::string ellipsize(const std::string& s, int max_w) {
    if ((int)s.size() <= max_w) return s;
    if (max_w < 5) return s.substr(0, max_w);
    int head = (max_w - 3) / 2;
    int tail = max_w - 3 - head;
    return s.substr(0, head) + "..." + s.substr((int)s.size() - tail);
}

// Print one aligned line (streaming)
static void print_aligned_line(const std::string& name, std::uint64_t count,
                               std::uint64_t total_size, int max_depth,
                               const Config& cfg,
                               int nw, int cnt_w, int sz_w, int dep_w,
                               int max_name_w,
                               bool is_total = false,
                               std::uint64_t grand_count = 0,
                               std::uint64_t grand_size = 0,
                               int total_max_depth = 0) {
    std::uint64_t c = is_total ? grand_count : count;
    std::string size_s;
    if (cfg.show_size) size_s = is_total ? fmt_size_str(grand_size)
                                          : fmt_size_str(total_size);
    std::string depth_s;
    if (cfg.show_depth) depth_s = is_total ? std::to_string(total_max_depth)
                                            : std::to_string(max_depth);
    std::string display = ellipsize(name, max_name_w);
    printf("%*s", cnt_w, fmt_num(c).c_str());
    if (cfg.show_size) printf("  %*s", sz_w, size_s.c_str());
    if (cfg.show_depth) printf("  %*s", dep_w, depth_s.c_str());
    printf("  %-*s\n", max_name_w, display.c_str());
}

// Streaming aligned output: pre-compute widths from paths, then print as we go
static void init_aligned_params(const std::vector<std::string>& paths,
                                const Config& cfg,
                                int& nw, int& cnt_w, int& sz_w, int& dep_w,
                                int& max_name_w) {
    // Pre-compute name width from paths
    nw = 5; // min for "total"
    for (auto& p : paths) {
        auto sl = p.find_last_of('/');
        std::string base = (sl != std::string::npos) ? p.substr(sl + 1) : p;
        if (base.empty()) base = p;
        nw = std::max(nw, (int)base.size());
    }
    // Use generous fixed widths for counts/sizes (since we can't know beforehand)
    cnt_w = 12;  // "9,999,999,999" = 12
    sz_w = 10;   // "999,999.9 TB" ≈ 10
    dep_w = 5;   // "65535" = 5

    // Terminal width
    int term_w = 80;
    const char* env = getenv("COLUMNS");
    if (env) term_w = atoi(env);
    if (term_w <= 0) term_w = 80;

    int min_w = cnt_w + 2 + (cfg.show_size ? sz_w + 2 : 0)
                       + (cfg.show_depth ? dep_w + 2 : 0);
    max_name_w = std::max(term_w - min_w, 10);
    // If name column would exceed actual longest name, cap it
    if (nw < max_name_w) max_name_w = nw;
}

// ---------------------------------------------------------------------------
//  Output: tree
// ---------------------------------------------------------------------------
struct TreeNode {
    std::string name;
    int files = 0;
    int dirs  = 0;
    std::vector<TreeNode> children;
};

static TreeNode build_tree(const std::string& path, int cur,
                           const Config& cfg) {
    TreeNode node;
    node.name = path;

    DIR* dp = opendir(path.c_str());
    if (!dp) return node;

    struct dirent* ent;
    std::vector<std::string> dirnames, filenames;

    while ((ent = readdir(dp))) {
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;
        std::string name(ent->d_name);
        if (!cfg.include_hidden && is_hidden(name)) continue;
        if (is_excluded(name, cfg.exclude_patterns)) continue;

        std::string full = path + "/" + name;
        struct stat s;
        if (cfg.follow_symlinks ? ::stat(full.c_str(), &s)
                                : ::lstat(full.c_str(), &s))
            continue;

        if (S_ISDIR(s.st_mode)) {
            dirnames.push_back(name);
        } else if (S_ISREG(s.st_mode)) {
            if (cfg.text_only && !is_text_file(full.c_str())) continue;
            filenames.push_back(name);
            node.files++;
        }
    }
    closedir(dp);
    node.dirs = (int)dirnames.size();
    std::sort(dirnames.begin(), dirnames.end());

    for (auto& dn : dirnames) {
        if (cfg.max_depth < 0 || cur + 1 <= cfg.max_depth) {
            node.children.push_back(
                build_tree(path + "/" + dn, cur + 1, cfg));
        } else {
            // Quick recursive count without building full tree
            FileInfo fi;
            walk((path + "/" + dn).c_str(), 0, fi, cfg);
            TreeNode stub;
            stub.name = dn;
            stub.files = (int)fi.count;
            node.children.push_back(std::move(stub));
        }
    }

    // Accumulate recursive totals from children
    for (auto& ch : node.children) {
        node.files += ch.files;
        node.dirs += ch.dirs;
    }
    return node;
}

static void print_tree_node(const TreeNode& node, const Config& cfg,
                            const std::string& prefix, bool last) {
    // Print this node
    std::string conn = last ? "└── " : "├── ";
    std::string label = node.name;
    auto p = node.name.find_last_of('/');
    if (p != std::string::npos) label = node.name.substr(p + 1);

    printf("%s%s", prefix.c_str(), conn.c_str());
    printf("%s", label.c_str());

    if (!node.children.empty()) {
        printf(" ");
        printf("[%df, %dd]", node.files, node.dirs);
    } else {
        printf(" [%df]", node.files);
    }
    printf("\n");

    std::string new_prefix = prefix + (last ? "    " : "│   ");
    for (size_t i = 0; i < node.children.size(); ++i) {
        bool is_last = (i + 1 == node.children.size());
        print_tree_node(node.children[i], cfg, new_prefix, is_last);
    }
}

static void output_tree(const std::string& path, int cur,
                        const Config& cfg, const std::string& prefix, bool last) {
    TreeNode tree = build_tree(path, cur, cfg);
    // print the root
    std::string label = path;
    auto p = path.find_last_of('/');
    if (p != std::string::npos) label = path.substr(p + 1);
    printf("%s [%df, %dd]\n", label.c_str(), tree.files, tree.dirs);

    for (size_t i = 0; i < tree.children.size(); ++i)
        print_tree_node(tree.children[i], cfg, "", i + 1 == tree.children.size());
}

// ---------------------------------------------------------------------------
//  Usage
// ---------------------------------------------------------------------------
static void usage() {
    fprintf(stderr,
"fscan - high-performance file counter\n"
"\n"
"Usage: fscan [options] <path> [path ...]\n"
"       fscan *  (shell-expanded)\n"
"\n"
"Counting:\n"
"  -a, --all            Include hidden files/dirs (starting with '.')\n"
"  -f, --follow         Follow symbolic links\n"
"  -L, --max-depth N    Limit recursion depth (default: unlimited)\n"
"  -T, --text           Only count text files (skip binary files)\n"
"  -j, --threads N      Number of threads (default: auto = CPU count)\n"
"      --since TIME     Count files modified after TIME\n"
"      --until TIME     Count files modified before TIME\n"
"      --exclude PAT    Exclude names matching PAT (glob-style, repeatable)\n"
"      --min-size SIZE  Only count files >= SIZE (e.g. 1G, 500M)\n"
"      --max-size SIZE  Only count files <= SIZE (e.g. 1G, 500M)\n"
"\n"
"Display:\n"
"  -e, --extensions     Show file count by extension\n"
"  -s, --size           Show total file size\n"
"  -d, --depth          Show directory depth statistics\n"
"  -t, --tree           Tree-style output with per-dir counts\n"
"  -A, --align          Align columns (truncates long names)\n"
"  -D, --dirs-only      Skip standalone files, only count directories\n"
"  -F, --find           Find individual files with sizes (list mode)\n"
"      --unit UNIT      Size unit: auto(default), B, KB, MB, GB, TB\n"
"\n"
"Output format:\n"
"      --csv            CSV output\n"
"\n"
"Time format: YYYY-MM-DD or YYYY-MM-DDTHH:MM:SS\n"
"\n"
"Examples:\n"
"  fscan src/                           Count files in src/\n"
"  fscan -e -s *                        Per-path with extensions and sizes\n"
"  fscan -t src/                        Tree view\n"
"  fscan --exclude '*.o' build/         Exclude .o files\n"
"  fscan --csv -e -s /var/log           CSV output\n"
"  fscan --since 2026-01-01 /tmp        Count files modified since Jan 1\n"
"  fscan --min-size 1G /var/log          Count files >= 1GB\n"
    );
}

// ---------------------------------------------------------------------------
//  Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "fscan: %s requires an argument\n", arg.c_str());
                exit(1);
            }
            return argv[++i];
        };

        if (arg == "--help")          { usage(); return 0; }
        else if (arg == "-a" || arg == "--all")           cfg.include_hidden = true;
        else if (arg == "-f" || arg == "--follow")        cfg.follow_symlinks = true;
        else if (arg == "-e" || arg == "--extensions")    cfg.show_extensions = true;
        else if (arg == "-s" || arg == "--size")          cfg.show_size = true;
        else if (arg == "-d" || arg == "--depth")         cfg.show_depth = true;
        else if (arg == "-t" || arg == "--tree")          cfg.tree_output = true;
        else if (arg == "-A" || arg == "--align")         cfg.align_output = true;
        else if (arg == "-D" || arg == "--dirs-only")     cfg.dirs_only = true;
        else if (arg == "-F" || arg == "--find")          cfg.find_mode = true;
        else if (arg == "--unit") {
            std::string v = next();
            if (v == "auto" || v == "AUTO") cfg.find_unit = 0;
            else if (v == "B" || v == "b") cfg.find_unit = 1;
            else if (v == "K" || v == "k" || v == "KB" || v == "kb") cfg.find_unit = 2;
            else if (v == "M" || v == "m" || v == "MB" || v == "mb") cfg.find_unit = 3;
            else if (v == "G" || v == "g" || v == "GB" || v == "gb") cfg.find_unit = 4;
            else if (v == "T" || v == "t" || v == "TB" || v == "tb") cfg.find_unit = 5;
            else { fprintf(stderr, "fscan: unknown unit '%s' (use B/KB/MB/GB/TB or auto)\n", v.c_str()); return 1; }
        }
        else if (arg == "-T" || arg == "--text")          cfg.text_only = true;
        else if (arg == "-j" || arg == "--threads")       cfg.thread_count = atoi(next());
        else if (arg == "-L" || arg == "--max-depth")     cfg.max_depth = atoi(next());
        else if (arg == "--since")                        cfg.since = parse_time(next());
        else if (arg == "--until")                        cfg.until = parse_time(next());
        else if (arg == "--min-size")                    cfg.min_size = parse_size(next());
        else if (arg == "--max-size")                    cfg.max_size = parse_size(next());
        else if (arg == "--exclude")                      cfg.exclude_patterns.push_back(next());
        else if (arg == "--csv")                          cfg.csv_output = true;
        else if (arg[0] == '-' && arg.size() > 1) {
            fprintf(stderr, "fscan: unknown option: %s\n\n", arg.c_str());
            usage(); return 1;
        }
        else {
            for (auto& p : resolve_arg(argv[i]))
                cfg.paths.push_back(p);
            if (cfg.paths.empty() && resolve_arg(argv[i]).empty()) return 1;
        }
    }

    if (cfg.paths.empty()) { usage(); return 1; }

    // Filter out standalone files if --dirs-only
    if (cfg.dirs_only) {
        size_t j = 0;
        for (size_t i = 0; i < cfg.paths.size(); ++i) {
            struct stat st;
            if (::stat(cfg.paths[i].c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                cfg.paths[j++] = cfg.paths[i];
        }
        cfg.paths.resize(j);
        if (cfg.paths.empty()) {
            fprintf(stderr, "fscan: no directories found\n");
            return 1;
        }
    }

    bool multi = cfg.paths.size() > 1;

    // Set find mode column width based on unit
    if (cfg.find_mode) {
        switch (cfg.find_unit) {
            case 1:  cfg.find_unit_w = 22; break;  // B: "14448391 B"
            case 2:  cfg.find_unit_w = 18; break;  // KB
            case 3:  cfg.find_unit_w = 15; break;  // MB
            case 4:  cfg.find_unit_w = 12; break;  // GB
            case 5:  cfg.find_unit_w = 10; break;  // TB
            default: cfg.find_unit_w = 15; break;  // auto
        }
    }

    // Pre-compute aligned column widths (only needs path names, not counts)
    int anw = 0, acnt_w = 0, asz_w = 0, adep_w = 0, aname_w = 0;
    if (cfg.align_output)
        init_aligned_params(cfg.paths, cfg, anw, acnt_w, asz_w, adep_w, aname_w);

    std::uint64_t grand = 0;
    std::uint64_t grand_size = 0;
    int total_max_depth = 0;

    for (size_t i = 0; i < cfg.paths.size(); ++i) {
        auto& p = cfg.paths[i];
        FileInfo info = count_path(p, cfg);
        grand += info.count;
        grand_size += info.total_size;
        if (info.max_depth > total_max_depth) total_max_depth = info.max_depth;

        auto sl = p.find_last_of('/');
        std::string base = (sl != std::string::npos) ? p.substr(sl + 1) : p;
        if (base.empty()) base = p;

        if (cfg.find_mode) {
            find_path(p, cfg);
        } else if (cfg.csv_output) {
            // CSV header once
            if (i == 0) {
                printf("path,count");
                if (cfg.show_size) printf(",size");
                if (cfg.show_extensions) printf(",extensions");
                printf("\n");
            }
            printf("\"%s\",%lu", p.c_str(), (unsigned long)info.count);
            if (cfg.show_size) printf(",%lu", (unsigned long)info.total_size);
            if (cfg.show_extensions && !info.ext_cnt.empty()) {
                std::string exts;
                for (auto& e : info.ext_cnt) {
                    if (!exts.empty()) exts += ";";
                    exts += e.first + ":" + std::to_string(e.second);
                }
                printf(",\"%s\"", exts.c_str());
            }
            printf("\n");
            fflush(stdout);
        } else if (cfg.tree_output) {
            if (multi)
                printf("%s\n", p.c_str());
            output_tree(p, 0, cfg, "", true);
            fflush(stdout);
        } else if (cfg.align_output) {
            print_aligned_line(base, info.count, info.total_size, info.max_depth,
                               cfg, anw, acnt_w, asz_w, adep_w, aname_w);
            if (cfg.show_extensions && !info.ext_cnt.empty()) {
                printf("  extensions:\n");
                std::vector<std::pair<std::string, std::uint64_t>> sorted(
                    info.ext_cnt.begin(), info.ext_cnt.end());
                std::sort(sorted.begin(), sorted.end(),
                          [](const auto& a, const auto& b){ return a.second > b.second; });
                for (auto& pr : sorted) {
                    std::uint64_t sz = 0;
                    auto it = info.ext_sz.find(pr.first);
                    if (it != info.ext_sz.end()) sz = it->second;
                    printf("    %-14s %s  (%s)\n",
                           pr.first.c_str(), fmt_num(pr.second).c_str(),
                           fmt_size_str(sz).c_str());
                }
            }
            fflush(stdout);
        } else {
            output_default(base, info, cfg);
            fflush(stdout);
        }
    }

    if (multi && !cfg.find_mode) {
        if (cfg.csv_output) {
            printf("\"total\",%lu", (unsigned long)grand);
            if (cfg.show_size) printf(",%lu", (unsigned long)grand_size);
            printf("\n");
        } else if (cfg.tree_output) {
            // no total for tree
        } else if (cfg.align_output) {
            print_aligned_line("total", 0, 0, 0,
                               cfg, anw, acnt_w, asz_w, adep_w, aname_w,
                               true, grand, grand_size, total_max_depth);
        } else {
            FileInfo total;
            total.count = grand;
            total.total_size = grand_size;
            total.max_depth = total_max_depth;
            output_default("total", total, cfg);
        }
    }

    return 0;
}

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

enum class SizeUnit { Auto = 0, B, KB, MB, GB, TB };

static constexpr std::uint64_t KB = 1024ULL;
static constexpr std::uint64_t MB = KB * 1024;
static constexpr std::uint64_t GB = MB * 1024;
static constexpr std::uint64_t TB = GB * 1024;

static constexpr int TEXT_DETECT_BUF = 8192;
static constexpr int DEFAULT_THREADS = 4;
static constexpr int MIN_ELLIPSIS_WIDTH = 5;

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
    bool find_mode        = false;

    SizeUnit find_unit    = SizeUnit::Auto;
    int find_unit_w       = 0;

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

static std::string fmt_find_size(std::uint64_t bytes, SizeUnit unit) {
    if (unit == SizeUnit::Auto) return fmt_size_str(bytes);
    static const char* units[] = {"","B","KB","MB","GB","TB"};
    double d = (double)bytes;
    for (int i = 1; i < (int)unit; ++i) d /= 1024.0;
    char buf[32];
    if (unit == SizeUnit::B)
        snprintf(buf, sizeof buf, "%lu %s", (unsigned long)bytes, units[(int)unit]);
    else
        snprintf(buf, sizeof buf, "%.0f %s", d, units[(int)unit]);
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

static bool is_text_file(const char* path);

static inline bool is_dots(const char* name) {
    return name[0] == '.' &&
        (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static inline bool do_stat(const char* path, struct stat& st, bool follow) {
    return (follow ? ::stat(path, &st) : ::lstat(path, &st)) == 0;
}

static inline bool passes_time_filter(time_t mtime, const Config& cfg) {
    if (cfg.since && mtime < cfg.since) return false;
    if (cfg.until && mtime > cfg.until) return false;
    return true;
}

static inline bool passes_file_filter(const std::string& path,
                                       std::uint64_t size, const Config& cfg) {
    if (cfg.min_size > 0 && size < cfg.min_size) return false;
    if (cfg.max_size > 0 && size > cfg.max_size) return false;
    if (cfg.text_only && !is_text_file(path.c_str())) return false;
    return true;
}

static bool should_skip_entry(const char* d_name, const Config& cfg) {
    if (is_dots(d_name)) return true;
    if (!cfg.include_hidden && d_name[0] == '.') return true;
    std::string name(d_name);
    if (is_excluded(name, cfg.exclude_patterns)) return true;
    return false;
}

static std::string basename(const std::string& path) {
    auto sl = path.find_last_of('/');
    return (sl != std::string::npos) ? path.substr(sl + 1) : path;
}

static int compute_thread_count(int config_count, size_t num_tasks) {
    int n = config_count > 0 ? config_count
                             : (int)std::thread::hardware_concurrency();
    if (n <= 0) n = DEFAULT_THREADS;
    return std::min(n, (int)num_tasks);
}

static void print_extensions(const FileInfo& info) {
    printf("  extensions:\n");
    std::vector<std::pair<std::string, std::uint64_t>> sorted(
        info.ext_cnt.begin(), info.ext_cnt.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    for (auto& kv : sorted) {
        std::uint64_t sz = 0;
        auto it = info.ext_sz.find(kv.first);
        if (it != info.ext_sz.end()) sz = it->second;
        printf("    %-14s %s  (%s)\n",
               kv.first.c_str(), fmt_num(kv.second).c_str(),
               fmt_size_str(sz).c_str());
    }
}

static void print_find_line(const std::string& path, std::uint64_t size,
                             const Config& cfg) {
    printf("%*s  %s\n", cfg.find_unit_w,
           fmt_find_size(size, cfg.find_unit).c_str(), path.c_str());
    fflush(stdout);
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
        case 'T': case 't': val *= TB; break;
        case 'G': case 'g': val *= GB; break;
        case 'M': case 'm': val *= MB; break;
        case 'K': case 'k': val *= KB; break;
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
    unsigned char buf[TEXT_DETECT_BUF];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == 0) return true;
    for (size_t i = 0; i < n; ++i)
        if (buf[i] == '\0') return false;
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
        if (should_skip_entry(entry->d_name, cfg)) continue;

        std::string name(entry->d_name);
        std::string full = std::string(dir) + "/" + name;

        struct stat st;
        if (!do_stat(full.c_str(), st, cfg.follow_symlinks)) continue;
        if (!passes_time_filter(st.st_mtime, cfg)) continue;

        if (S_ISDIR(st.st_mode)) {
            if (cfg.max_depth < 0 || cur_depth + 1 <= cfg.max_depth)
                walk(full.c_str(), cur_depth + 1, info, cfg);
        } else if (S_ISREG(st.st_mode)) {
            if (!passes_file_filter(full, (std::uint64_t)st.st_size, cfg)) continue;
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
        if (should_skip_entry(entry->d_name, cfg)) continue;

        std::string name(entry->d_name);
        std::string full = std::string(dir) + "/" + name;
        struct stat st;
        if (!do_stat(full.c_str(), st, cfg.follow_symlinks)) continue;
        if (!passes_time_filter(st.st_mtime, cfg)) continue;

        if (S_ISDIR(st.st_mode)) {
            find_walk(full.c_str(), cfg);
        } else if (S_ISREG(st.st_mode)) {
            if (!passes_file_filter(full, (std::uint64_t)st.st_size, cfg)) continue;
            std::lock_guard<std::mutex> lk(find_mtx);
            print_find_line(full, (std::uint64_t)st.st_size, cfg);
        }
    }
    closedir(dp);
}

// Parallel find: split top-level subdirs across threads
static void find_path(const std::string& path, const Config& cfg) {
    struct stat st;
    if (!do_stat(path.c_str(), st, cfg.follow_symlinks)) return;

    if (S_ISREG(st.st_mode)) {
        if (!passes_file_filter(path, (std::uint64_t)st.st_size, cfg)) return;
        print_find_line(path, (std::uint64_t)st.st_size, cfg);
        return;
    }
    if (!S_ISDIR(st.st_mode)) return;

    DIR* dp = opendir(path.c_str());
    if (!dp) return;

    struct dirent* ent;
    std::vector<std::string> subdirs;

    while ((ent = readdir(dp))) {
        if (should_skip_entry(ent->d_name, cfg)) continue;

        std::string name(ent->d_name);
        std::string full = path + "/" + name;
        struct stat s;
        if (!do_stat(full.c_str(), s, cfg.follow_symlinks)) continue;
        if (!passes_time_filter(s.st_mtime, cfg)) continue;

        if (S_ISDIR(s.st_mode)) {
            subdirs.push_back(full);
        } else if (S_ISREG(s.st_mode)) {
            if (!passes_file_filter(full, (std::uint64_t)s.st_size, cfg)) continue;
            print_find_line(full, (std::uint64_t)s.st_size, cfg);
        }
    }
    closedir(dp);

    if (subdirs.empty()) return;

    int nthreads = compute_thread_count(cfg.thread_count, subdirs.size());

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
    if (!do_stat(path.c_str(), st, cfg.follow_symlinks)) return info;

    if (S_ISREG(st.st_mode)) {
        if (!passes_file_filter(path, (std::uint64_t)st.st_size, cfg)) return info;
        info.count = 1;
        info.total_size = (std::uint64_t)st.st_size;
        std::string name = basename(path);
        if (name.empty()) name = path;
        std::string ext = file_ext(name);
        info.ext_cnt[ext] = 1;
        info.ext_sz[ext] = info.total_size;
        return info;
    }
    if (!S_ISDIR(st.st_mode)) return info;

    DIR* dp = opendir(path.c_str());
    if (!dp) return info;

    struct dirent* ent;
    std::vector<std::string> subdirs;

    while ((ent = readdir(dp))) {
        if (should_skip_entry(ent->d_name, cfg)) continue;

        std::string name(ent->d_name);
        std::string full = path + "/" + name;
        struct stat s;
        if (!do_stat(full.c_str(), s, cfg.follow_symlinks)) continue;
        if (!passes_time_filter(s.st_mtime, cfg)) continue;

        if (S_ISDIR(s.st_mode)) {
            subdirs.push_back(full);
        } else if (S_ISREG(s.st_mode)) {
            if (!passes_file_filter(full, (std::uint64_t)s.st_size, cfg)) continue;
            info.count++;
            info.total_size += (std::uint64_t)s.st_size;
            std::string ext = file_ext(name);
            info.ext_cnt[ext]++;
            info.ext_sz[ext] += (std::uint64_t)s.st_size;
        }
    }
    closedir(dp);

    if (subdirs.empty()) return info;

    int nthreads = compute_thread_count(cfg.thread_count, subdirs.size());

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

    if (cfg.show_extensions && !info.ext_cnt.empty())
        print_extensions(info);
}

// Truncate long names with ellipsis in the middle
static std::string ellipsize(const std::string& s, int max_w) {
    if ((int)s.size() <= max_w) return s;
    if (max_w < MIN_ELLIPSIS_WIDTH) return s.substr(0, max_w);
    int head = (max_w - 3) / 2;
    int tail = max_w - 3 - head;
    return s.substr(0, head) + "..." + s.substr((int)s.size() - tail);
}

struct AlignWidths {
    int nw = 0, cnt_w = 0, sz_w = 0, dep_w = 0, max_name_w = 0;
};

static void print_aligned_line(const std::string& name, std::uint64_t count,
                               std::uint64_t total_size, int max_depth,
                               const Config& cfg, const AlignWidths& aw) {
    std::string display = ellipsize(name, aw.max_name_w);
    printf("%*s", aw.cnt_w, fmt_num(count).c_str());
    if (cfg.show_size) printf("  %*s", aw.sz_w, fmt_size_str(total_size).c_str());
    if (cfg.show_depth) printf("  %*s", aw.dep_w, std::to_string(max_depth).c_str());
    printf("  %-*s\n", aw.max_name_w, display.c_str());
}

static AlignWidths init_aligned_params(const std::vector<std::string>& paths,
                                       const Config& cfg) {
    static constexpr int DEFAULT_COUNT_WIDTH = 12;
    static constexpr int DEFAULT_SIZE_WIDTH  = 10;
    static constexpr int DEFAULT_DEPTH_WIDTH = 5;
    static constexpr int DEFAULT_TERM_WIDTH  = 80;

    AlignWidths aw;
    aw.cnt_w = DEFAULT_COUNT_WIDTH;
    aw.sz_w  = DEFAULT_SIZE_WIDTH;
    aw.dep_w = DEFAULT_DEPTH_WIDTH;

    aw.nw = 5;
    for (auto& p : paths) {
        std::string base = basename(p);
        if (base.empty()) base = p;
        aw.nw = std::max(aw.nw, (int)base.size());
    }

    int term_w = DEFAULT_TERM_WIDTH;
    const char* env = getenv("COLUMNS");
    if (env) term_w = atoi(env);
    if (term_w <= 0) term_w = DEFAULT_TERM_WIDTH;

    int min_w = aw.cnt_w + 2 + (cfg.show_size ? aw.sz_w + 2 : 0)
                           + (cfg.show_depth ? aw.dep_w + 2 : 0);
    aw.max_name_w = std::max(term_w - min_w, 10);
    if (aw.nw < aw.max_name_w) aw.max_name_w = aw.nw;
    return aw;
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
    std::vector<std::string> dirnames;

    while ((ent = readdir(dp))) {
        if (should_skip_entry(ent->d_name, cfg)) continue;

        std::string name(ent->d_name);
        std::string full = path + "/" + name;
        struct stat s;
        if (!do_stat(full.c_str(), s, cfg.follow_symlinks)) continue;

        if (S_ISDIR(s.st_mode)) {
            dirnames.push_back(name);
        } else if (S_ISREG(s.st_mode)) {
            if (!passes_file_filter(full, (std::uint64_t)s.st_size, cfg)) continue;
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
            FileInfo fi;
            walk((path + "/" + dn).c_str(), 0, fi, cfg);
            TreeNode stub;
            stub.name = dn;
            stub.files = (int)fi.count;
            node.children.push_back(std::move(stub));
        }
    }

    for (auto& ch : node.children) {
        node.files += ch.files;
        node.dirs += ch.dirs;
    }
    return node;
}

static void print_tree_node(const TreeNode& node, const Config& cfg,
                            const std::string& prefix, bool last) {
    std::string conn = last ? "\xe2\x94\x94\xe2\x94\x80\xe2\x94\x80 " : "\xe2\x94\x9c\xe2\x94\x80\xe2\x94\x80 ";
    std::string label = basename(node.name);
    if (label.empty()) label = node.name;

    printf("%s%s", prefix.c_str(), conn.c_str());
    printf("%s", label.c_str());

    if (!node.children.empty()) {
        printf(" ");
        printf("[%df, %dd]", node.files, node.dirs);
    } else {
        printf(" [%df]", node.files);
    }
    printf("\n");

    std::string new_prefix = prefix + (last ? "    " : "\xe2\x94\x82   ");
    for (size_t i = 0; i < node.children.size(); ++i) {
        bool is_last = (i + 1 == node.children.size());
        print_tree_node(node.children[i], cfg, new_prefix, is_last);
    }
}

static void output_tree(const std::string& path, int cur, const Config& cfg) {
    TreeNode tree = build_tree(path, cur, cfg);
    std::string label = basename(path);
    if (label.empty()) label = path;
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
            if (v == "auto" || v == "AUTO") cfg.find_unit = SizeUnit::Auto;
            else if (v == "B" || v == "b") cfg.find_unit = SizeUnit::B;
            else if (v == "K" || v == "k" || v == "KB" || v == "kb") cfg.find_unit = SizeUnit::KB;
            else if (v == "M" || v == "m" || v == "MB" || v == "mb") cfg.find_unit = SizeUnit::MB;
            else if (v == "G" || v == "g" || v == "GB" || v == "gb") cfg.find_unit = SizeUnit::GB;
            else if (v == "T" || v == "t" || v == "TB" || v == "tb") cfg.find_unit = SizeUnit::TB;
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
            auto resolved = resolve_arg(argv[i]);
            for (auto& p : resolved)
                cfg.paths.push_back(p);
            if (cfg.paths.empty() && resolved.empty()) return 1;
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
            case SizeUnit::B:  cfg.find_unit_w = 22; break;
            case SizeUnit::KB: cfg.find_unit_w = 18; break;
            case SizeUnit::MB: cfg.find_unit_w = 15; break;
            case SizeUnit::GB: cfg.find_unit_w = 12; break;
            case SizeUnit::TB: cfg.find_unit_w = 10; break;
            default:           cfg.find_unit_w = 15; break;
        }
    }

    AlignWidths aw;
    if (cfg.align_output)
        aw = init_aligned_params(cfg.paths, cfg);

    std::uint64_t grand = 0;
    std::uint64_t grand_size = 0;
    int total_max_depth = 0;

    for (size_t i = 0; i < cfg.paths.size(); ++i) {
        auto& p = cfg.paths[i];
        FileInfo info = count_path(p, cfg);
        grand += info.count;
        grand_size += info.total_size;
        if (info.max_depth > total_max_depth) total_max_depth = info.max_depth;

        std::string base = basename(p);
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
            output_tree(p, 0, cfg);
            fflush(stdout);
        } else if (cfg.align_output) {
            print_aligned_line(base, info.count, info.total_size, info.max_depth,
                               cfg, aw);
            if (cfg.show_extensions && !info.ext_cnt.empty())
                print_extensions(info);
            fflush(stdout);
        } else {
            output_default(base, info, cfg);
            fflush(stdout);
        }
    }

    if (multi && !cfg.find_mode && !cfg.tree_output) {
        if (cfg.csv_output) {
            printf("\"total\",%lu", (unsigned long)grand);
            if (cfg.show_size) printf(",%lu", (unsigned long)grand_size);
            printf("\n");
        } else if (cfg.align_output) {
            print_aligned_line("total", grand, grand_size, total_max_depth,
                               cfg, aw);
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

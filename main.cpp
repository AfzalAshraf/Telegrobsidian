// =============================================================================
//  Telegrobsidian - Obsidian <-> Telegram <-> Notion sync daemon (C++20)
// =============================================================================
//
//  Threads
//  -------
//    main          : supervisor (signal handling, startup, ordered shutdown)
//    http          : cpp-httplib server, receives Telegram webhook POSTs
//    sync-worker   : single writer for the vault + Notion outbound calls
//    file-watcher  : recursive inotify watcher over the vault
//    notion-poller : incremental pull of the Notion database (optional)
//
//  Sync model
//  ----------
//    Telegram message -> vault file (telegram_inbox/) -> new Notion page
//    Local vault file -> new Notion page (obsidian_sync/ files excluded)
//    Notion page      -> mirrored vault file (notion_sync/), updated in place
//
//  Loop protection
//  ---------------
//    Writing to a file raises inotify events, so every write the daemon makes
//    is registered in a suppression registry. The first event observed for a
//    path whose mtime is not newer than our own write is dropped, which breaks
//    the write -> event -> write feedback loop. Entries expire on a cooldown
//    (and are never consulted by a later edit), so a crash between the write
//    and the bookkeeping cannot permanently mute a file.
//
//  Build
//  -----
//    1) cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
//    2) g++ -std=c++20 -O2 -Wall -Wextra -Iinclude main.cpp -o telegrobsidian
//           -DCPPHTTPLIB_OPENSSL_SUPPORT -lssl -lcrypto -lpthread
//
//    Drop -DCPPHTTPLIB_OPENSSL_SUPPORT / -lssl / -lcrypto for a Telegram-only
//    build (Notion integration is then disabled at runtime).
// =============================================================================

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(TELEGROBSIDIAN_USE_OPENSSL) && !defined(CPPHTTPLIB_OPENSSL_SUPPORT)
#define CPPHTTPLIB_OPENSSL_SUPPORT
#endif

#include "httplib.h"
#include "nlohmann/json.hpp"

#if defined(CPPHTTPLIB_OPENSSL_SUPPORT) || defined(CPPHTTPLIB_MBEDTLS_SUPPORT) ||   \
    defined(CPPHTTPLIB_WOLFSSL_SUPPORT)
#define TELEGROBSIDIAN_HAS_SSL 1
#else
#define TELEGROBSIDIAN_HAS_SSL 0
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;
using namespace std::chrono_literals;

namespace tgo {

// =============================================================================
//  Version / constants
// =============================================================================
constexpr const char* kVersion = "1.0.0";
constexpr int kStateVersion = 1;
// Bump when the rendered markdown changes so existing mirrors are re-rendered.
constexpr int kRenderVersion = 1;

constexpr size_t kInotifyBufferSize = 64 * 1024;  // >= sizeof(inotify_event)+NAME_MAX
constexpr size_t kMaxCaptureChildren = 100;       // Notion: max blocks per request
constexpr size_t kMaxRichTextChunk = 1900;        // Notion: max rich text length
constexpr int kMaxPageFailures = 3;               // give up mirroring a page after N polls
constexpr size_t kNotionTitlePropertyProbe = 1;

// =============================================================================
//  Small utilities
// =============================================================================
// -----------------------------------------------------------------------------
//  printf-style formatting without std::format
// -----------------------------------------------------------------------------
// libstdc++ only ships <format> from GCC 13 onward, so the daemon carries this
// small formatter instead of linking another dependency. It also avoids passing
// a runtime format string to printf-family functions, which newer compilers
// reject as a potential format-string vulnerability (-Wformat-security /
// -Wformat-nonliteral) and which would be a real hazard if any caller ever
// forwarded user input as the pattern.
//
// Supported (the subset this project uses): %s %d %i %u %x %X %c %f %g %% with
// the flags '-' and '0', an optional width, an optional precision and the
// length modifiers l/ll/z/h.
struct FormatArg {
    std::string text;                // default rendering
    unsigned long long integer = 0;  // valid when integral
    double real = 0.0;               // valid when floating
    bool integral = false;
    bool floating = false;
};

inline FormatArg make_arg(const std::string& value) { return FormatArg{value}; }
inline FormatArg make_arg(std::string_view value) { return FormatArg{std::string(value)}; }
inline FormatArg make_arg(const char* value) {
    return FormatArg{value != nullptr ? std::string(value) : std::string("(null)")};
}
inline FormatArg make_arg(const fs::path& value) { return FormatArg{value.string()}; }

template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
FormatArg make_arg(T value) {
    FormatArg arg;
    if constexpr (std::is_same_v<T, bool>) {
        arg.text = value ? "true" : "false";
        arg.integer = value ? 1ULL : 0ULL;
        arg.integral = true;
    } else if constexpr (std::is_floating_point_v<T>) {
        std::ostringstream os;
        os << value;
        arg.text = os.str();
        arg.real = static_cast<double>(value);
        arg.floating = true;
    } else {
        arg.integer = static_cast<unsigned long long>(value);
        arg.text = std::to_string(value);
        arg.integral = true;
    }
    return arg;
}

inline std::string to_hex(unsigned long long value) {
    if (value == 0) return "0";
    static const char* digits = "0123456789abcdef";
    std::string out;
    while (value > 0) {
        out.insert(out.begin(), digits[value & 0xF]);
        value >>= 4;
    }
    return out;
}

inline std::string format_impl(std::string_view pattern, const std::vector<FormatArg>& args) {
    std::string out;
    out.reserve(pattern.size() + 32);
    size_t next_arg = 0;

    for (size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != '%') {
            out += pattern[i];
            continue;
        }
        if (i + 1 >= pattern.size()) {
            out += '%';
            break;
        }
        if (pattern[i + 1] == '%') {
            out += '%';
            ++i;
            continue;
        }

        size_t j = i + 1;
        bool left_aligned = false;
        bool zero_padded = false;
        for (; j < pattern.size(); ++j) {
            const char flag = pattern[j];
            if (flag == '-') {
                left_aligned = true;
            } else if (flag == '0') {
                zero_padded = true;
            } else if (flag == '+' || flag == ' ' || flag == '#') {
                // accepted and ignored
            } else {
                break;
            }
        }
        size_t width = 0;
        bool has_width = false;
        for (; j < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[j])); ++j) {
            has_width = true;
            width = width * 10 + static_cast<size_t>(pattern[j] - '0');
        }
        size_t precision = 0;
        bool has_precision = false;
        if (j < pattern.size() && pattern[j] == '.') {
            ++j;
            while (j < pattern.size() && std::isdigit(static_cast<unsigned char>(pattern[j]))) {
                has_precision = true;
                precision = precision * 10 + static_cast<size_t>(pattern[j] - '0');
                ++j;
            }
        }
        while (j < pattern.size() && (pattern[j] == 'l' || pattern[j] == 'z' || pattern[j] == 'h' ||
                                      pattern[j] == 'q' || pattern[j] == 'j' || pattern[j] == 't')) {
            ++j;
        }
        if (j >= pattern.size()) {  // truncated conversion: copy verbatim
            out += pattern.substr(i);
            break;
        }

        const char conversion = pattern[j];
        const bool known = conversion == 's' || conversion == 'd' || conversion == 'i' ||
                           conversion == 'u' || conversion == 'x' || conversion == 'X' ||
                           conversion == 'c' || conversion == 'f' || conversion == 'g';
        if (!known) {
            out += pattern.substr(i, j - i + 1);
            i = j;
            continue;
        }

        // A missing argument is a programming error: render it visibly instead
        // of reading out of bounds.
        const FormatArg arg = next_arg < args.size()
                                  ? args[next_arg++]
                                  : FormatArg{"<missing>" + std::string(1, conversion)};

        std::string value;
        if (conversion == 'f' && arg.floating) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(has_precision ? static_cast<int>(precision) : 6)
               << arg.real;
            value = os.str();
        } else if ((conversion == 'x' || conversion == 'X') && arg.integral) {
            value = to_hex(arg.integer);
            if (conversion == 'X') {
                for (char& c : value) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
        } else {
            value = arg.text;
        }

        if (has_precision && value.size() < precision) {
            value.insert(0, precision - value.size(), '0');
        }
        if (has_width && value.size() < width) {
            const size_t padding = width - value.size();
            const bool numeric_zero_pad = zero_padded && !left_aligned;
            if (left_aligned) {
                value.append(padding, ' ');
            } else {
                value.insert(0, padding, numeric_zero_pad ? '0' : ' ');
            }
        }

        out += value;
        i = j;
    }
    return out;
}

// All call sites pass a string literal as the pattern; the signature accepts a
// string_view so the literal is not copied.
template <typename... Args>
std::string fmt(std::string_view pattern, const Args&... args) {
    const std::vector<FormatArg> collected{make_arg(args)...};
    return format_impl(pattern, collected);
}

// Verifies the formatter against the conversions this project relies on. Run by
// --self-test; it exists because a hand-written formatter deserves its own
// checks rather than only being exercised by the integration test.
inline bool check_formatter(std::string& report) {
    struct Case {
        const char* name;
        std::string got;
        std::string want;
    };
    const std::vector<Case> cases = {
        {"literal", fmt("hello world"), "hello world"},
        {"percent", fmt("100%% sure"), "100% sure"},
        {"string", fmt("a=%s", std::string("b")), "a=b"},
        {"c-string", fmt("%s/%s", "x", "yy"), "x/yy"},
        {"path", fmt("%s", fs::path("/tmp/vault")), "/tmp/vault"},
        {"string-view", fmt("%s", std::string_view("view")), "view"},
        {"bool", fmt("%s", true), "true"},
        {"int", fmt("%d", 42), "42"},
        {"negative", fmt("%lld", static_cast<long long>(-12)), "-12"},
        {"unsigned", fmt("%llu", static_cast<unsigned long long>(18446744073709551615ULL)),
         "18446744073709551615"},
        {"size-t", fmt("%zu", static_cast<size_t>(17)), "17"},
        {"two-digit", fmt("%02d:%02d:%02d", 9, 5, 3), "09:05:03"},
        {"three-digit-with-suffix", fmt("%03dZ", 5), "005Z"},
        {"four-digit", fmt("%04d-%02d", 2026, 6), "2026-06"},
        {"left-aligned", fmt("[%-5s]", std::string("ab")), "[ab   ]"},
        {"right-aligned", fmt("[%5s]", std::string("ab")), "[   ab]"},
        {"hex", fmt("%x", 255), "ff"},
        {"hex-padded", fmt("%04x", 15), "000f"},
        {"hex-upper", fmt("%X", 255), "FF"},
        {"hex-64", fmt("%016llx", static_cast<unsigned long long>(0xabc)), "0000000000000abc"},
        {"json-escape", fmt("\\u%04x", 31), "\\u001f"},
        {"float", fmt("%f", 1.5), "1.500000"},
        {"precision", fmt("%.3d", 7), "007"},
        {"adjacent", fmt("%s%s", "a", "b"), "ab"},
        {"no-args-mixed", fmt("no arguments here"), "no arguments here"},
        {"too-few-args", fmt("%s and %s", "only-one"), "only-one and <missing>s"},
    };

    std::ostringstream failures;
    size_t failed = 0;
    for (const Case& item : cases) {
        if (item.got != item.want) {
            ++failed;
            failures << "\n  " << item.name << ": got '" << item.got << "', want '" << item.want << "'";
        }
    }
    if (failed == 0) {
        report = fmt("%zu formatter checks passed", cases.size());
        return true;
    }
    report = fmt("%zu/%zu formatter checks FAILED", failed, cases.size()) + failures.str();
    return false;
}

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool contains_ci(std::string_view haystack, std::string_view needle) {
    return to_lower(std::string(haystack)).find(to_lower(std::string(needle))) !=
           std::string::npos;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string_view::npos) {
            out.emplace_back(text.substr(start));
            break;
        }
        out.emplace_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

// 64-bit FNV-1a. Used only to detect "content unchanged since last push",
// so a non-cryptographic hash is sufficient.
uint64_t fnv1a(std::string_view data) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : data) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

std::string hex64(uint64_t v) { return fmt("%016llx", static_cast<unsigned long long>(v)); }

// Notion returns null for optional fields, and nlohmann's value() throws a
// type_error when the stored value is not convertible. Every access to API
// JSON therefore goes through these accessors.
std::string json_string(const json& obj, const char* key, const std::string& fallback = {}) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_string()) return fallback;
    return it->get<std::string>();
}

bool json_bool(const json& obj, const char* key, bool fallback = false) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_boolean()) return fallback;
    return it->get<bool>();
}

int64_t json_int(const json& obj, const char* key, int64_t fallback = 0) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_number_integer()) return fallback;
    return it->get<int64_t>();
}

json json_array(const json& obj, const char* key) {
    if (!obj.is_object()) return json::array();
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return json::array();
    return *it;
}

// =============================================================================
//  Time helpers (UTC, ISO-8601, filesystem clock conversion)
// =============================================================================
std::string iso8601_utc(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()) {
    const auto secs = std::chrono::time_point_cast<std::chrono::seconds>(tp);
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(tp - secs).count();
    const std::time_t t = std::chrono::system_clock::to_time_t(secs);
    std::tm tm{};
    gmtime_r(&t, &tm);
    return fmt("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
               tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(millis));
}

std::string local_human(std::chrono::system_clock::time_point tp = std::chrono::system_clock::now()) {
    const std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    return fmt("%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
}

std::optional<std::chrono::system_clock::time_point> parse_iso8601(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0, ms = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d.%d", &y, &mo, &d, &h, &mi, &se, &ms) < 6) {
        return std::nullopt;
    }
    std::tm tm{};
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = se;
    const std::time_t t = timegm(&tm);
    if (t < 0) return std::nullopt;
    return std::chrono::system_clock::from_time_t(t) + std::chrono::milliseconds(ms);
}

// libstdc++ only provides a portable file_clock -> system_clock conversion from
// GCC 13 (std::chrono::clock_cast). Subtracting the two clock "now" values is
// accurate to microseconds, which is orders of magnitude below our tolerances.
std::chrono::system_clock::time_point file_time_to_system(fs::file_time_type ft) {
    const auto sys_now = std::chrono::system_clock::now();
    const auto file_now = fs::file_time_type::clock::now();
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(ft - file_now + sys_now);
}

std::optional<std::chrono::system_clock::time_point> file_mtime(const fs::path& p) {
    std::error_code ec;
    const auto ft = fs::last_write_time(p, ec);
    if (ec) return std::nullopt;
    return file_time_to_system(ft);
}

// Canonical key used by every path-keyed map (suppression, fingerprints).
std::string path_key(const fs::path& p) {
    std::error_code ec;
    const fs::path abs = fs::absolute(p, ec);
    return (ec ? p : abs).lexically_normal().string();
}

// =============================================================================
//  Logging
// =============================================================================
enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Success };

const char* log_level_name(LogLevel l) {
    switch (l) {
        case LogLevel::Trace:   return "TRACE";
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO";
        case LogLevel::Warn:    return "WARN";
        case LogLevel::Error:   return "ERROR";
        case LogLevel::Success: return "OK";
    }
    return "?";
}

std::optional<LogLevel> parse_log_level(const std::string& s) {
    const std::string v = to_lower(trim(s));
    if (v == "trace") return LogLevel::Trace;
    if (v == "debug") return LogLevel::Debug;
    if (v == "info") return LogLevel::Info;
    if (v == "warn" || v == "warning") return LogLevel::Warn;
    if (v == "error") return LogLevel::Error;
    return std::nullopt;
}

std::string json_escape(std::string_view in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    out += fmt("\\u%04x", static_cast<int>(static_cast<unsigned char>(c)));
                } else {
                    out += c;
                }
        }
    }
    return out;
}

class Logger {
public:
    static void configure(LogLevel min_level, bool timestamps, bool json_lines, bool color) {
        min_level_.store(static_cast<int>(min_level), std::memory_order_relaxed);
        timestamps_ = timestamps;
        json_ = json_lines;
        color_ = color && !json_lines;
    }

    static bool enabled(LogLevel l) {
        return static_cast<int>(l) >= min_level_.load(std::memory_order_relaxed);
    }

    static void log(LogLevel l, const std::string& msg) {
        if (!enabled(l)) return;
        const std::string line =
            json_ ? fmt("{\"ts\":\"%s\",\"level\":\"%s\",\"msg\":\"%s\"}",
                        iso8601_utc(), log_level_name(l), json_escape(msg))
                  : (timestamps_ ? fmt("[%s] %-5s %s", local_human(), log_level_name(l), msg)
                                 : fmt("%-5s %s", log_level_name(l), msg));

        static std::mutex mtx;
        std::lock_guard<std::mutex> lock(mtx);
        std::ostream& os = (l == LogLevel::Error || l == LogLevel::Warn) ? std::cerr : std::cout;
        if (color_) {
            os << color_for(l) << line << "\033[0m\n";
        } else {
            os << line << '\n';
        }
        os.flush();
    }

    static void trace(const std::string& m) { log(LogLevel::Trace, m); }
    static void debug(const std::string& m) { log(LogLevel::Debug, m); }
    static void info(const std::string& m) { log(LogLevel::Info, m); }
    static void warn(const std::string& m) { log(LogLevel::Warn, m); }
    static void error(const std::string& m) { log(LogLevel::Error, m); }
    static void success(const std::string& m) { log(LogLevel::Success, m); }

private:
    static const char* color_for(LogLevel l) {
        switch (l) {
            case LogLevel::Trace:   return "\033[0;90m";
            case LogLevel::Debug:   return "\033[0;90m";
            case LogLevel::Info:    return "\033[0;36m";
            case LogLevel::Warn:    return "\033[0;33m";
            case LogLevel::Error:   return "\033[0;31m";
            case LogLevel::Success: return "\033[0;32m";
        }
        return "";
    }

    inline static std::atomic<int> min_level_{static_cast<int>(LogLevel::Info)};
    inline static bool timestamps_ = true;
    inline static bool json_ = false;
    inline static bool color_ = false;
};

// =============================================================================
//  CLI / environment parsing
// =============================================================================
struct CliOptions {
    std::vector<std::string> env_files;
    bool once = false;        // --once      : poll Notion once, then exit
    bool self_test = false;   // --self-test : validate config/credentials, then exit
    bool show_help = false;
    bool show_version = false;
    std::vector<std::string> warnings;
};

std::string env_string(const char* key, const std::string& fallback) {
    if (const char* v = std::getenv(key); v != nullptr && *v != '\0') return std::string(v);
    return fallback;
}

int64_t env_int(const char* key, int64_t fallback, std::vector<std::string>& warnings,
                int64_t min = INT64_MIN, int64_t max = INT64_MAX) {
    const char* raw = std::getenv(key);
    if (raw == nullptr || *raw == '\0') return fallback;
    try {
        const int64_t v = std::stoll(trim(raw));
        if (v < min || v > max) {
            warnings.push_back(fmt("%s=%s is out of range [%lld, %lld]; using %lld", key, raw,
                                   static_cast<long long>(min), static_cast<long long>(max),
                                   static_cast<long long>(fallback)));
            return fallback;
        }
        return v;
    } catch (const std::exception&) {
        warnings.push_back(fmt("%s=%s is not a number; using %lld", key, raw,
                               static_cast<long long>(fallback)));
        return fallback;
    }
}

bool env_bool(const char* key, bool fallback, std::vector<std::string>& warnings) {
    const char* raw = std::getenv(key);
    if (raw == nullptr || *raw == '\0') return fallback;
    const std::string v = to_lower(trim(raw));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    warnings.push_back(fmt("%s=%s is not a boolean (true/false); using %s", key, raw,
                           fallback ? "true" : "false"));
    return fallback;
}

// Minimal dotenv loader: KEY=VALUE per line, '#' comments, optional quotes.
// Existing environment variables always win over the file.
void load_env_file(const fs::path& path, std::vector<std::string>& warnings) {
    std::ifstream in(path);
    if (!in) {
        warnings.push_back(fmt("env file not readable: %s", path.string()));
        return;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;
        const size_t eq = trimmed.find('=');
        if (eq == std::string::npos) {
            warnings.push_back(fmt("%s:%d: ignoring malformed line", path.string(), lineno));
            continue;
        }
        const std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front()) {
            value = value.substr(1, value.size() - 2);
        }
        if (key.empty()) continue;
        if (std::getenv(key.c_str()) == nullptr) {
            ::setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
        }
    }
}

CliOptions parse_cli(int argc, char** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--env-file" && i + 1 < argc) {
            opt.env_files.emplace_back(argv[++i]);
        } else if (starts_with(arg, "--env-file=")) {
            opt.env_files.push_back(arg.substr(std::strlen("--env-file=")));
        } else if (arg == "--once") {
            opt.once = true;
        } else if (arg == "--self-test") {
            opt.self_test = true;
        } else if (arg == "--help" || arg == "-h") {
            opt.show_help = true;
        } else if (arg == "--version" || arg == "-V") {
            opt.show_version = true;
        } else {
            opt.warnings.push_back(fmt("unknown argument ignored: %s", arg));
        }
    }
    return opt;
}

void print_help(const char* argv0) {
    std::cout <<
        "Telegrobsidian " << kVersion << " - Obsidian <-> Telegram <-> Notion sync daemon\n\n"
        "Usage: " << argv0 << " [options]\n\n"
        "Options:\n"
        "  --once              Poll Notion once (and process the queue), then exit.\n"
        "  --self-test         Validate configuration and credentials, then exit.\n"
        "  --env-file <path>   Load KEY=VALUE pairs before reading the environment.\n"
        "  -h, --help          Show this help.\n"
        "  -V, --version       Show the version.\n\n"
        "Configuration is entirely environment driven; see README.md and .env.example.\n";
}

// =============================================================================
//  Configuration
// =============================================================================
struct Config {
    // Vault
    fs::path vault_path{"/var/lib/obsidian/vault"};
    fs::path state_file;
    std::string notion_mirror_dir{"notion_sync"};
    std::string telegram_inbox_dir{"telegram_inbox"};
    std::string obsidian_push_dir{"obsidian_sync"};

    // Notion
    std::string notion_api_key;
    std::string notion_database_id;
    std::string notion_version{"2022-06-28"};
    std::string notion_base_url{"https://api.notion.com"};
    std::chrono::seconds notion_poll_interval{60};
    std::chrono::seconds notion_cursor_overlap{1};
    int notion_page_cache_max{5000};

    // Telegram
    std::string telegram_bot_token;
    std::string telegram_chat_id;
    std::string telegram_webhook_secret;
    std::string telegram_public_url;  // set to auto-register the webhook
    // Overridable for a self-hosted Bot API server (or the test double).
    std::string telegram_api_base{"https://api.telegram.org"};

    // HTTP endpoint
    std::string webhook_bind{"0.0.0.0"};
    int webhook_port{8080};
    size_t max_payload_bytes{1024 * 1024};

    // Behaviour
    std::chrono::milliseconds dedup_cooldown{5000};
    std::chrono::milliseconds event_debounce{300};
    std::chrono::milliseconds quiesce_window{200};
    std::chrono::seconds drain_timeout{5};
    std::chrono::seconds rescan_interval{120};
    std::chrono::seconds http_connect_timeout{5};
    std::chrono::seconds http_read_timeout{15};
    bool push_mirror_files{false};
    // On a first run an existing vault is registered as already synced instead
    // of creating one Notion page per note; opt in with this flag.
    bool push_existing_on_startup{false};
    bool use_system_ca_bundle{true};
    int max_push_attempts{3};

    // Logging
    LogLevel log_level{LogLevel::Info};
    bool log_timestamps{true};
    bool log_json{false};

    // CLI
    bool once{false};
    bool self_test{false};

    std::vector<std::string> warnings;

    bool notion_configured() const {
        return !notion_api_key.empty() && !notion_database_id.empty();
    }

    static Config load() {
        Config c;
        c.vault_path = env_string("OBSIDIAN_VAULT_PATH", c.vault_path.string());
        c.notion_mirror_dir = env_string("NOTION_MIRROR_DIR", c.notion_mirror_dir);
        c.telegram_inbox_dir = env_string("TELEGRAM_INBOX_DIR", c.telegram_inbox_dir);
        c.obsidian_push_dir = env_string("OBSIDIAN_PUSH_DIR", c.obsidian_push_dir);

        c.notion_api_key = env_string("NOTION_API_KEY", "");
        c.notion_database_id = env_string("NOTION_DATABASE_ID", "");
        c.notion_version = env_string("NOTION_VERSION", c.notion_version);
        c.notion_base_url = env_string("NOTION_BASE_URL", c.notion_base_url);
        c.notion_poll_interval = std::chrono::seconds{env_int(
            "NOTION_POLL_INTERVAL_SEC", c.notion_poll_interval.count(), c.warnings, 5, 86400)};
        c.notion_page_cache_max = static_cast<int>(
            env_int("NOTION_PAGE_CACHE_MAX", c.notion_page_cache_max, c.warnings, 100, 1000000));

        c.telegram_bot_token = env_string("TELEGRAM_BOT_TOKEN", "");
        c.telegram_chat_id = env_string("TELEGRAM_CHAT_ID", "");
        c.telegram_webhook_secret = env_string("TELEGRAM_WEBHOOK_SECRET", "");
        c.telegram_public_url = env_string("TELEGRAM_PUBLIC_URL", "");
        c.telegram_api_base = env_string("TELEGRAM_API_BASE", c.telegram_api_base);
        while (!c.telegram_public_url.empty() && c.telegram_public_url.back() == '/') {
            c.telegram_public_url.pop_back();
        }
        while (!c.telegram_api_base.empty() && c.telegram_api_base.back() == '/') {
            c.telegram_api_base.pop_back();
        }

        c.webhook_bind = env_string("WEBHOOK_BIND", c.webhook_bind);
        c.webhook_port = static_cast<int>(
            env_int("WEBHOOK_PORT", c.webhook_port, c.warnings, 1, 65535));
        c.max_payload_bytes = static_cast<size_t>(
            env_int("WEBHOOK_MAX_PAYLOAD_BYTES", static_cast<int64_t>(c.max_payload_bytes),
                    c.warnings, 1024, 64 * 1024 * 1024));

        c.dedup_cooldown = std::chrono::milliseconds{
            env_int("DEDUP_COOLDOWN_MS", c.dedup_cooldown.count(), c.warnings, 0, 3600000)};
        c.event_debounce = std::chrono::milliseconds{
            env_int("DEBOUNCE_MS", c.event_debounce.count(), c.warnings, 0, 60000)};
        c.quiesce_window = std::chrono::milliseconds{
            env_int("QUIESCE_MS", c.quiesce_window.count(), c.warnings, 0, 60000)};
        c.drain_timeout = std::chrono::seconds{
            env_int("DRAIN_TIMEOUT_SEC", c.drain_timeout.count(), c.warnings, 0, 300)};
        c.rescan_interval = std::chrono::seconds{
            env_int("WATCHER_RESCAN_SEC", c.rescan_interval.count(), c.warnings, 10, 86400)};
        c.http_connect_timeout = std::chrono::seconds{
            env_int("HTTP_CONNECT_TIMEOUT_SEC", c.http_connect_timeout.count(), c.warnings, 1, 120)};
        c.http_read_timeout = std::chrono::seconds{
            env_int("HTTP_READ_TIMEOUT_SEC", c.http_read_timeout.count(), c.warnings, 1, 300)};
        c.push_mirror_files = env_bool("NOTION_PUSH_MIRROR_FILES", c.push_mirror_files, c.warnings);
        c.push_existing_on_startup =
            env_bool("PUSH_EXISTING_ON_STARTUP", c.push_existing_on_startup, c.warnings);
        c.use_system_ca_bundle =
            env_bool("USE_SYSTEM_CA_BUNDLE", c.use_system_ca_bundle, c.warnings);
        c.max_push_attempts =
            static_cast<int>(env_int("MAX_PUSH_ATTEMPTS", c.max_push_attempts, c.warnings, 1, 10));

        const std::string state_env = env_string("STATE_FILE", "");
        c.state_file = state_env.empty()
                           ? c.vault_path / ".telegrobsidian" / "state.json"
                           : fs::path(state_env);

        if (auto lvl = parse_log_level(env_string("LOG_LEVEL", "info"))) {
            c.log_level = *lvl;
        } else {
            c.warnings.push_back("LOG_LEVEL is not one of trace/debug/info/warn/error; using info");
        }
        c.log_timestamps = env_bool("LOG_TIMESTAMPS", c.log_timestamps, c.warnings);
        c.log_json = env_bool("LOG_JSON", c.log_json, c.warnings);
        return c;
    }

    // Warnings that indicate a likely copy/paste of the placeholder docs.
    std::vector<std::string> validate() const {
        std::vector<std::string> problems;
        if (notion_configured()) {
            const std::string id = notion_database_id;
            std::string hex_only;
            for (char ch : id) {
                if (ch == '-') continue;
                if (std::isxdigit(static_cast<unsigned char>(ch))) {
                    hex_only += ch;
                } else {
                    hex_only.clear();
                    break;
                }
            }
            if (hex_only.size() != 32) {
                problems.push_back(
                    "NOTION_DATABASE_ID does not look like a Notion database id "
                    "(expected 32 hex characters, dashes optional)");
            }
            if (!starts_with(notion_api_key, "secret_") && !starts_with(notion_api_key, "ntn_")) {
                problems.push_back(
                    "NOTION_API_KEY does not start with 'secret_' or 'ntn_' - is this a "
                    "Notion integration token?");
            }
            if (contains_ci(notion_api_key, "yourtoken") || contains_ci(notion_api_key, "<") ||
                contains_ci(notion_database_id, "yourdatabase") ||
                contains_ci(notion_database_id, "<")) {
                problems.push_back("NOTION_API_KEY / NOTION_DATABASE_ID still look like placeholders");
            }
            if (notion_base_url.empty() || notion_base_url.back() == '/') {
                problems.push_back(
                    "NOTION_BASE_URL must not be empty or end with '/' (e.g. https://api.notion.com)");
            }
            if (contains_ci(notion_base_url, "://notion.com") ||
                contains_ci(notion_base_url, "//www.notion")) {
                problems.push_back(
                    "NOTION_BASE_URL points at the Notion web app, not the API; use "
                    "https://api.notion.com");
            }
        }
        if (webhook_port <= 0 || webhook_port > 65535) {
            problems.push_back("WEBHOOK_PORT must be between 1 and 65535");
        }
        for (const auto* name : {"TELEGRAM_API_BASE", "TELEGRAM_PUBLIC_URL"}) {
            const std::string& value = std::string(name) == "TELEGRAM_API_BASE" ? telegram_api_base
                                                                            : telegram_public_url;
            if (!value.empty() && !starts_with(value, "http://") && !starts_with(value, "https://")) {
                problems.push_back(fmt("%s must start with http:// or https:// (got %s)", std::string(name),
                                       value));
            }
        }
        return problems;
    }
};

// =============================================================================
//  Persistent state (Notion cursor, mirror bookkeeping, push fingerprints)
// =============================================================================
class StateStore {
public:
    StateStore(fs::path path, int page_cache_max)
        : path_(std::move(path)), page_cache_max_(page_cache_max) {}

    // Returns false when an existing state file could not be parsed; the daemon
    // then starts from scratch instead of trusting corrupt bookkeeping.
    // True when there was no usable state file: the first run over an existing
    // vault is then handled differently (see PUSH_EXISTING_ON_STARTUP).
    bool is_pristine() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return pristine_;
    }

    bool load() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!fs::exists(path_)) return true;
        pristine_ = false;
        std::ifstream in(path_);
        if (!in) {
            Logger::warn(fmt("state file %s is not readable; starting fresh", path_.string()));
            return false;
        }
        std::stringstream buf;
        buf << in.rdbuf();
        const json j = json::parse(buf.str(), nullptr, /*allow_exceptions=*/false);
        if (j.is_discarded() || !j.is_object()) {
            Logger::warn(fmt("state file %s is corrupt; starting fresh", path_.string()));
            return false;
        }
        if (json_int(j, "state_version") != kStateVersion) {
            Logger::warn(fmt("state file %s has an unknown version; starting fresh", path_.string()));
            return false;
        }
        notion_cursor_ = json_string(j, "notion_cursor");
        if (json_int(j, "render_version") != kRenderVersion) {
            Logger::info("render format changed: existing Notion mirrors will be refreshed");
            pages_.clear();
        } else if (j.contains("pages") && j["pages"].is_object()) {
            for (auto it = j["pages"].begin(); it != j["pages"].end(); ++it) {
                if (it.value().is_string()) pages_[it.key()] = it.value().get<std::string>();
            }
        }
        if (j.contains("pushed") && j["pushed"].is_object()) {
            for (auto it = j["pushed"].begin(); it != j["pushed"].end(); ++it) {
                if (it.value().is_string()) pushed_[it.key()] = it.value().get<std::string>();
            }
        }
        return true;
    }

    std::string notion_cursor() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return notion_cursor_;
    }

    void set_notion_cursor(const std::string& value) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (notion_cursor_ == value) return;
        notion_cursor_ = value;
        dirty_ = true;
    }

    std::optional<std::string> page_version(const std::string& page_id) const {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = pages_.find(page_id);
        if (it == pages_.end()) return std::nullopt;
        return it->second;
    }

    void set_page_version(const std::string& page_id, const std::string& version) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto& slot = pages_[page_id];
        if (slot == version) return;
        slot = version;
        pages_touched_.push_back(page_id);
        dirty_ = true;
    }

    std::optional<std::string> pushed_hash(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto it = pushed_.find(path);
        if (it == pushed_.end()) return std::nullopt;
        return it->second;
    }

    void set_pushed_hash(const std::string& path, const std::string& hash) {
        std::lock_guard<std::mutex> lock(mtx_);
        auto& slot = pushed_[path];
        if (slot == hash) return;
        slot = hash;
        pushed_touched_.push_back(path);
        dirty_ = true;
    }

    void forget_pushed_hash(const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (pushed_.erase(path) > 0) dirty_ = true;
    }

    bool dirty() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return dirty_;
    }

    bool save() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!dirty_) return true;
        prune_locked();

        json j;
        j["state_version"] = kStateVersion;
        j["render_version"] = kRenderVersion;
        j["notion_cursor"] = notion_cursor_;
        j["pages"] = json::object();
        for (const auto& [id, version] : pages_) j["pages"][id] = version;
        j["pushed"] = json::object();
        for (const auto& [path, hash] : pushed_) j["pushed"][path] = hash;
        j["updated_at"] = iso8601_utc();

        std::error_code ec;
        fs::create_directories(path_.parent_path(), ec);
        const fs::path tmp = path_.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::trunc | std::ios::binary);
            if (!out) {
                Logger::warn(fmt("cannot write state file %s", tmp.string()));
                return false;
            }
            out << j.dump(1) << '\n';
            out.flush();
            if (!out) {
                Logger::warn(fmt("failed writing state file %s", tmp.string()));
                return false;
            }
        }
        fs::rename(tmp, path_, ec);
        if (ec) {
            Logger::warn(fmt("cannot replace state file %s: %s", path_.string(), ec.message()));
            fs::remove(tmp, ec);
            return false;
        }
        dirty_ = false;
        return true;
    }

    size_t cached_pages() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return pages_.size();
    }

    void clear_page_cache() {
        std::lock_guard<std::mutex> lock(mtx_);
        pages_.clear();
        dirty_ = true;
    }

private:
    // Bounded growth: Notion databases change over time, so the oldest inserted
    // entries are evicted once the cache exceeds its configured size.
    void prune_locked() {
        while (static_cast<int>(pages_.size()) > page_cache_max_ && !pages_touched_.empty()) {
            const std::string victim = pages_touched_.front();
            pages_touched_.pop_front();
            if (pages_.erase(victim) > 0) {
                Logger::debug(fmt("page cache: dropped bookkeeping for %s", victim));
            }
        }
        while (pushed_.size() > 5000 && !pushed_touched_.empty()) {
            const std::string victim = pushed_touched_.front();
            pushed_touched_.pop_front();
            pushed_.erase(victim);
        }
        // Drop touch entries whose key was already re-inserted (kept fresh).
        std::unordered_set<std::string> live;
        live.reserve(pages_.size());
        for (const auto& [id, _] : pages_) live.insert(id);
        std::deque<std::string> cleaned;
        for (const auto& id : pages_touched_) {
            if (live.erase(id) > 0) cleaned.push_back(id);
        }
        pages_touched_ = std::move(cleaned);

        std::unordered_set<std::string> live_push;
        live_push.reserve(pushed_.size());
        for (const auto& [p, _] : pushed_) live_push.insert(p);
        std::deque<std::string> cleaned_push;
        for (const auto& p : pushed_touched_) {
            if (live_push.erase(p) > 0) cleaned_push.push_back(p);
        }
        pushed_touched_ = std::move(cleaned_push);
    }

    mutable std::mutex mtx_;
    fs::path path_;
    int page_cache_max_;
    std::string notion_cursor_;
    std::unordered_map<std::string, std::string> pages_;
    std::unordered_map<std::string, std::string> pushed_;
    std::deque<std::string> pages_touched_;
    std::deque<std::string> pushed_touched_;
    bool dirty_ = false;
    bool pristine_ = true;
};

// =============================================================================
//  Loop suppression
// =============================================================================
// Records writes performed by this process so the resulting inotify events can
// be recognised and dropped. Lookups are decided with the observed mtime:
//   * event mtime <= our write time (+ tolerance)  -> our own echo, drop it
//   * event mtime >  our write time (+ tolerance)  -> somebody else wrote
//     afterwards, treat as a real change
// Entries expire after the cooldown, so an abrupt exit cannot mute a path.
class SuppressionRegistry {
public:
    explicit SuppressionRegistry(std::chrono::milliseconds cooldown) : cooldown_(cooldown) {}

    void register_write(const fs::path& p, std::chrono::system_clock::time_point written_at) {
        std::lock_guard<std::mutex> lock(mtx_);
        entries_[normalize(p)] = Entry{written_at, std::chrono::steady_clock::now()};
        ++writes_;
    }

    bool should_ignore(const fs::path& p, std::optional<std::chrono::system_clock::time_point> mtime) {
        std::lock_guard<std::mutex> lock(mtx_);
        const std::string key = normalize(p);
        const auto it = entries_.find(key);
        if (it == entries_.end()) return false;

        // Hard expiry measured on a monotonic clock (robust across clock jumps).
        if (std::chrono::steady_clock::now() - it->second.observed > cooldown_) {
            entries_.erase(it);
            return false;
        }
        if (mtime.has_value() && *mtime > it->second.written_at + kMtimeTolerance) {
            entries_.erase(it);  // a newer external write
            return false;
        }
        ++suppressed_;
        return true;
    }

    size_t writes() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return writes_;
    }

    size_t suppressed() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return suppressed_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return entries_.size();
    }

    void prune() {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            it = (now - it->second.observed > cooldown_) ? entries_.erase(it) : std::next(it);
        }
    }

    static std::string normalize(const fs::path& p) { return path_key(p); }

private:
    struct Entry {
        std::chrono::system_clock::time_point written_at;
        std::chrono::steady_clock::time_point observed;
    };

    // inotify reports the mtime granularity of the filesystem; a write and its
    // event can therefore appear identical. Anything beyond this is external.
    static constexpr auto kMtimeTolerance = std::chrono::milliseconds(50);

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Entry> entries_;
    std::chrono::milliseconds cooldown_;
    size_t writes_ = 0;
    size_t suppressed_ = 0;
};

// Collapses bursts of filesystem events per path without ever losing the last
// change: every event bumps a version, and the watcher schedules one task each
// time the version moved past what it already scheduled *and* the path has been
// quiet for the debounce window. Coalescing therefore never swallows an update
// that happened while an earlier task was still being processed.
class FileEventTracker {
public:
    void note_event(const fs::path& p) {
        std::lock_guard<std::mutex> lock(mtx_);
        Entry& entry = entries_[path_key(p)];
        entry.path = p;
        ++entry.version;
        entry.last_event = std::chrono::steady_clock::now();
    }

    // Paths whose burst has settled and that still need a task.
    std::vector<fs::path> take_ready(std::chrono::milliseconds quiet) {
        std::vector<fs::path> ready;
        std::lock_guard<std::mutex> lock(mtx_);
        const auto now = std::chrono::steady_clock::now();
        for (auto& [key, entry] : entries_) {
            if (entry.version <= entry.scheduled) continue;
            if (now - entry.last_event < quiet) continue;
            entry.scheduled = entry.version;
            ready.push_back(entry.path);
            (void)key;
        }
        return ready;
    }

    void forget(const fs::path& p) {
        std::lock_guard<std::mutex> lock(mtx_);
        entries_.erase(path_key(p));
    }

    void prune() {
        std::lock_guard<std::mutex> lock(mtx_);
        const auto now = std::chrono::steady_clock::now();
        for (auto it = entries_.begin(); it != entries_.end();) {
            const bool idle = it->second.version == it->second.scheduled;
            const bool stale = now - it->second.last_event > std::chrono::minutes(10);
            it = (idle && stale) ? entries_.erase(it) : std::next(it);
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return entries_.size();
    }

private:
    struct Entry {
        fs::path path;
        uint64_t version = 0;    // bumped by every event
        uint64_t scheduled = 0;  // version a task was last queued for
        std::chrono::steady_clock::time_point last_event{};
    };

    mutable std::mutex mtx_;
    std::unordered_map<std::string, Entry> entries_;
};

// =============================================================================
//  Task queue
// =============================================================================
enum class TaskKind { TelegramWebhook, LocalFileChanged };

const char* task_kind_name(TaskKind k) {
    switch (k) {
        case TaskKind::TelegramWebhook:   return "telegram";
        case TaskKind::LocalFileChanged:  return "local-file";
    }
    return "?";
}

struct SyncTask {
    TaskKind kind{TaskKind::TelegramWebhook};
    std::string payload;   // raw Telegram webhook body
    std::string filepath;  // absolute path for file events
};

class TaskQueue {
public:
    void push(SyncTask task) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            queue_.push_back(std::move(task));
            if (queue_.size() > high_water_) high_water_ = queue_.size();
        }
        cv_.notify_one();
    }

    // Returns false only when the queue is empty *and* a stop was requested, so
    // a shutdown still drains what Telegram already handed over. An empty queue
    // during normal operation keeps the caller waiting.
    bool pop(SyncTask& out, std::stop_token st) {
        std::unique_lock<std::mutex> lock(mtx_);
        for (;;) {
            if (!queue_.empty()) {
                out = std::move(queue_.front());
                queue_.pop_front();
                return true;
            }
            if (st.stop_requested()) return false;
            cv_.wait_for(lock, 100ms, [&] { return !queue_.empty() || st.stop_requested(); });
        }
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return queue_.size();
    }

    size_t high_water() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return high_water_;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<SyncTask> queue_;
    size_t high_water_ = 0;
};

// =============================================================================
//  Stats
// =============================================================================
struct Stats {
    std::atomic<uint64_t> webhooks_received{0};
    std::atomic<uint64_t> webhooks_rejected{0};
    std::atomic<uint64_t> tasks_processed{0};
    std::atomic<uint64_t> files_written{0};
    std::atomic<uint64_t> files_unchanged{0};
    std::atomic<uint64_t> pushes_attempted{0};
    std::atomic<uint64_t> pushes_succeeded{0};
    std::atomic<uint64_t> pushes_skipped{0};
    std::atomic<uint64_t> pushes_failed{0};
    std::atomic<uint64_t> notion_polls{0};
    std::atomic<uint64_t> notion_poll_failures{0};
    std::atomic<uint64_t> notion_pages_scanned{0};
    std::atomic<uint64_t> notion_pages_rendered{0};
    std::atomic<uint64_t> events_received{0};
    std::atomic<uint64_t> events_suppressed{0};
    std::atomic<uint64_t> events_scheduled{0};
    std::atomic<uint64_t> inotify_overflows{0};
    std::atomic<uint64_t> errors{0};

    json snapshot() const {
        return json{
            {"webhooks_received", webhooks_received.load()},
            {"webhooks_rejected", webhooks_rejected.load()},
            {"tasks_processed", tasks_processed.load()},
            {"files_written", files_written.load()},
            {"files_unchanged", files_unchanged.load()},
            {"pushes_attempted", pushes_attempted.load()},
            {"pushes_succeeded", pushes_succeeded.load()},
            {"pushes_skipped", pushes_skipped.load()},
            {"pushes_failed", pushes_failed.load()},
            {"notion_polls", notion_polls.load()},
            {"notion_poll_failures", notion_poll_failures.load()},
            {"notion_pages_scanned", notion_pages_scanned.load()},
            {"notion_pages_rendered", notion_pages_rendered.load()},
            {"events_received", events_received.load()},
            {"events_suppressed", events_suppressed.load()},
            {"events_scheduled", events_scheduled.load()},
            // events that were folded into a scheduled task
            {"events_coalesced",
             events_received.load() > events_scheduled.load()
                 ? events_received.load() - events_scheduled.load()
                 : 0},
            {"inotify_overflows", inotify_overflows.load()},
            {"errors", errors.load()},
        };
    }
};

// =============================================================================
//  Small RAII / sleep helpers
// =============================================================================
template <typename F>
class ScopeExit {
public:
    explicit ScopeExit(F f) : f_(std::move(f)) {}
    ~ScopeExit() { f_(); }
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

private:
    F f_;
};

template <typename F>
ScopeExit<F> on_scope_exit(F f) {
    return ScopeExit<F>(std::move(f));
}

// Sleep that wakes up immediately when the owning thread is asked to stop.
void interruptible_sleep(std::chrono::milliseconds d, std::stop_token st) {
    if (d.count() <= 0 || st.stop_requested()) return;
    std::mutex m;
    std::condition_variable cv;
    bool woke = false;
    std::stop_callback cb(st, [&] {
        std::lock_guard<std::mutex> lock(m);
        woke = true;
        cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(m);
    cv.wait_for(lock, d, [&] { return woke; });
}

// =============================================================================
//  Markdown helpers
// =============================================================================
std::string yaml_quote(std::string value) {
    std::string out;
    out.reserve(value.size() + 2);
    for (char c : value) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n' || c == '\r' || c == '\t') {
            out += ' ';
            continue;
        }
        out += c;
    }
    return "\"" + out + "\"";
}

std::string strip_frontmatter(const std::string& content) {
    if (!starts_with(content, "---\n") && !starts_with(content, "---\r\n")) return content;
    const size_t first_end = content.find('\n');
    if (first_end == std::string::npos) return content;
    size_t pos = first_end + 1;
    while (pos < content.size()) {
        const size_t line_end = content.find('\n', pos);
        const std::string line =
            trim(content.substr(pos, (line_end == std::string::npos ? content.size() : line_end) - pos));
        pos = (line_end == std::string::npos) ? content.size() : line_end + 1;
        if (line == "---") {
            while (pos < content.size() && (content[pos] == '\n' || content[pos] == '\r')) ++pos;
            return content.substr(pos);
        }
    }
    return content;  // unterminated frontmatter: keep everything
}

std::optional<std::string> frontmatter_value(const std::string& content, const std::string& key) {
    if (!starts_with(content, "---\n") && !starts_with(content, "---\r\n")) return std::nullopt;
    const size_t first_end = content.find('\n');
    size_t pos = first_end + 1;
    while (pos < content.size()) {
        const size_t line_end = content.find('\n', pos);
        const size_t end = (line_end == std::string::npos) ? content.size() : line_end;
        const std::string line = trim(content.substr(pos, end - pos));
        if (line == "---") break;
        if (starts_with(line, key + ":")) {
            std::string value = trim(line.substr(key.size() + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
                value = value.substr(1, value.size() - 2);
            }
            return value;
        }
        if (line_end == std::string::npos) break;
        pos = line_end + 1;
    }
    return std::nullopt;
}

// Title resolution for outbound pushes: frontmatter, then first H1, then name.
std::string document_title(const std::string& content, const std::string& fallback) {
    if (auto t = frontmatter_value(content, "title"); t.has_value() && !trim(*t).empty()) {
        return trim(*t);
    }
    for (const std::string& line : split_lines(content)) {
        const std::string l = trim(line);
        if (starts_with(l, "# ")) {
            const std::string heading = trim(l.substr(2));
            if (!heading.empty()) return heading;
        }
        if (l.size() > 200) break;
    }
    return fallback;
}

// =============================================================================
//  Vault writer (atomic, loop-suppressed)
// =============================================================================
struct WriteOutcome {
    bool ok = false;
    fs::path path;
    bool unchanged = false;
    std::string error;
};

class VaultWriter {
public:
    VaultWriter(const Config& cfg, SuppressionRegistry& suppression)
        : root_(cfg.vault_path), suppression_(suppression) {
        std::error_code ec;
        root_ = fs::absolute(root_, ec);
    }

    const fs::path& root() const { return root_; }

    // Rejects absolute paths and any attempt to escape the vault.
    std::optional<fs::path> resolve_relative(const std::string& relative) const {
        fs::path rel(relative);
        if (rel.is_absolute() || relative.empty()) return std::nullopt;
        for (const auto& part : rel) {
            if (part == "..") return std::nullopt;
        }
        fs::path full = (root_ / rel).lexically_normal();
        const std::string prefix = root_.string();
        const std::string full_str = full.string();
        if (full_str != prefix && !starts_with(full_str, prefix + "/")) return std::nullopt;
        return full;
    }

    // Writes markdown with YAML frontmatter + body. The rename() at the end
    // makes the update atomic, so readers never observe a partial file.
    // Existing files with identical content are left untouched.
    WriteOutcome write_markdown(const std::string& relative_filename, const std::string& frontmatter_extra,
                                const std::string& body, const std::string& title) {
        WriteOutcome out;
        const auto resolved = resolve_relative(relative_filename);
        if (!resolved.has_value()) {
            out.error = fmt("refusing to write outside the vault: %s", relative_filename);
            return out;
        }
        out.path = *resolved;

        std::string content;
        content.reserve(frontmatter_extra.size() + body.size() + 128);
        content += "---\n";
        content += fmt("title: %s\n", yaml_quote(title));
        content += frontmatter_extra;
        content += "---\n\n";
        content += body;
        if (!body.empty() && body.back() != '\n') content += '\n';

        try {
            std::error_code ec;
            fs::create_directories(out.path.parent_path(), ec);
            if (ec) {
                out.error = fmt("cannot create directory %s: %s", out.path.parent_path().string(),
                                ec.message());
                return out;
            }

            if (read_if_exists(out.path) == std::optional<std::string>(content)) {
                out.ok = true;
                out.unchanged = true;
                return out;
            }

            // Unique temp name: concurrent writers (or a previous crash) can
            // never collide, unlike a fixed ".tmp" suffix.
            const fs::path tmp = out.path.parent_path() /
                                 fmt(".%s.%d.tmp", out.path.filename().string(),
                                     static_cast<int>(::getpid()));
            {
                std::ofstream file(tmp, std::ios::trunc | std::ios::binary);
                if (!file) {
                    out.error = fmt("cannot open %s for writing", tmp.string());
                    return out;
                }
                file << content;
                file.flush();
                if (!file) {
                    out.error = fmt("failed writing %s", tmp.string());
                    file.close();
                    fs::remove(tmp, ec);
                    return out;
                }
            }
            // Registered before the rename (tolerance covers the sub-millisecond
            // gap) and refreshed after it, so an event observed in between is
            // still attributed to this write.
            suppression_.register_write(out.path, std::chrono::system_clock::now());
            fs::rename(tmp, out.path, ec);
            if (ec) {
                out.error = fmt("cannot replace %s: %s", out.path.string(), ec.message());
                fs::remove(tmp, ec);
                return out;
            }
            // Recorded *after* the rename: the file is visible with its final
            // mtime, and late events carry an mtime <= this timestamp.
            suppression_.register_write(out.path, std::chrono::system_clock::now());
            out.ok = true;
            return out;
        } catch (const std::exception& e) {
            out.error = fmt("unexpected filesystem error: %s", std::string(e.what()));
            return out;
        }
    }

    static std::optional<std::string> read_if_exists(const fs::path& p) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return std::nullopt;
        std::stringstream buf;
        buf << in.rdbuf();
        if (!in.good() && !in.eof()) return std::nullopt;
        return buf.str();
    }

private:
    fs::path root_;
    SuppressionRegistry& suppression_;
};

// =============================================================================
//  Recursive inotify watcher
// =============================================================================
class VaultWatcher {
public:
    VaultWatcher(const Config& cfg, TaskQueue& queue, SuppressionRegistry& suppression,
                 FileEventTracker& tracker, Stats& stats)
        : root_(cfg.vault_path), queue_(queue), suppression_(suppression), tracker_(tracker),
          stats_(stats), rescan_interval_(cfg.rescan_interval),
          quiet_window_(cfg.event_debounce) {}

    // Opens inotify and watches every directory that exists right now. Called
    // from the main thread *before* the startup catch-up reads the vault, so
    // every change made after that point is guaranteed to raise an event.
    bool prime(std::stop_token st) {
        inotify_fd_ = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) {
            Logger::error(fmt("inotify_init1 failed: %s", std::string(std::strerror(errno))));
            ++stats_.errors;
            return false;
        }
        scan_and_watch(st, /*scan_files=*/false);
        return true;
    }

    void run(std::stop_token st) {
        if (inotify_fd_ < 0) {
            Logger::error("file watcher started without a valid inotify descriptor");
            ++stats_.errors;
            return;
        }
        const auto close_fd = on_scope_exit([this] {
            if (inotify_fd_ >= 0) ::close(inotify_fd_);
            inotify_fd_ = -1;
        });

        Logger::info(fmt("Watching %s (%zu directories)", root_.string(), watch_count()));

        std::vector<char> buffer(kInotifyBufferSize);
        auto last_rescan = std::chrono::steady_clock::now();

        while (!st.stop_requested()) {
            struct pollfd pfd;
            pfd.fd = inotify_fd_;
            pfd.events = POLLIN;
            pfd.revents = 0;

            const int ready = ::poll(&pfd, 1, 250);  // 250 ms tick keeps stop responsive
            if (ready < 0) {
                if (errno == EINTR) continue;
                Logger::error(fmt("inotify poll failed: %s", std::string(std::strerror(errno))));
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_rescan > rescan_interval_) {
                last_rescan = now;
                prune_dead_watches();
                // Picks up directories that appeared while we were not looking
                // (including after an inotify overflow) and pushes their files.
                scan_and_watch(st, /*scan_files=*/true);
                tracker_.prune();
                suppression_.prune();
            }

            if (ready > 0) {
                const ssize_t len = ::read(inotify_fd_, buffer.data(), buffer.size());
                if (len < 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                        Logger::error(fmt("inotify read failed: %s", std::string(std::strerror(errno))));
                        break;
                    }
                } else if (len > 0) {
                    handle_events(buffer.data(), static_cast<size_t>(len), st);
                }
            }

            // Enqueue whatever has been quiet long enough. Doing this on every
            // tick (not only when the kernel delivers an event) is what makes a
            // change that landed during processing get picked up afterwards.
            flush_settled_changes();
        }
        Logger::debug("file watcher stopped");
    }

private:
    void scan_and_watch(std::stop_token st, bool scan_files) {
        add_watch(root_, scan_files);
        std::error_code ec;
        fs::recursive_directory_iterator it(root_, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            Logger::warn(fmt("cannot scan %s: %s", root_.string(), ec.message()));
            return;
        }
        const fs::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (st.stop_requested()) return;
            if (ec) {
                ec.clear();
                continue;
            }
            const fs::directory_entry& entry = *it;
            std::error_code type_ec;
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(type_ec) && !type_ec) {
                if (!name.empty() && name[0] == '.') {
                    it.disable_recursion_pending();  // .git, .obsidian, .telegrobsidian
                    continue;
                }
                add_watch(entry.path(), scan_files);
            } else if (entry.is_symlink(type_ec) && !type_ec) {
                it.disable_recursion_pending();  // never follow symlinks out of the vault
            }
        }
    }

    // Every file that already exists inside a directory we just started
    // watching is pushed once: its inotify events were created before the watch
    // existed and can therefore never be delivered (e.g. `git checkout` creating
    // a directory full of notes). The worker skips anything unchanged, so this
    // only costs a hash per file.
    void enqueue_existing_files(const fs::path& dir) {
        std::error_code ec;
        int queued = 0;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) return;
        for (const auto& entry : it) {
            const std::string name = entry.path().filename().string();
            if (name.empty() || name[0] == '.') continue;
            std::error_code type_ec;
            if (!entry.is_regular_file(type_ec) || type_ec) continue;
            if (entry.path().extension() != ".md") continue;
            SyncTask task;
            task.kind = TaskKind::LocalFileChanged;
            task.filepath = entry.path().string();
            queue_.push(std::move(task));
            ++queued;
        }
        if (queued > 0) Logger::debug(fmt("queued %d existing file(s) in %s", queued, dir.string()));
    }

    void add_watch(const fs::path& dir, bool scan_files) {
        const std::string dir_str = dir.string();
        if (watched_paths_.count(dir_str) > 0) return;

        const int wd = ::inotify_add_watch(inotify_fd_, dir_str.c_str(),
                                           IN_MODIFY | IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM |
                                               IN_CLOSE_WRITE | IN_DELETE | IN_DELETE_SELF |
                                               IN_MOVE_SELF | IN_ONLYDIR);
        if (wd < 0) {
            Logger::debug(fmt("cannot watch %s: %s", dir_str, std::string(std::strerror(errno))));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mtx_);
            by_wd_[wd] = dir;
            watched_paths_.insert(dir_str);
        }
        Logger::trace(fmt("watching %s (wd %d)", dir_str, wd));
        if (scan_files) enqueue_existing_files(dir);
    }

    void prune_dead_watches() {
        std::error_code ec;
        std::vector<std::string> dead;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            for (auto it = watched_paths_.begin(); it != watched_paths_.end();) {
                if (!fs::exists(*it, ec)) {
                    dead.push_back(*it);
                    it = watched_paths_.erase(it);
                } else {
                    ++it;
                }
            }
            for (auto it = by_wd_.begin(); it != by_wd_.end();) {
                if (!fs::exists(it->second, ec)) {
                    ::inotify_rm_watch(inotify_fd_, it->first);
                    it = by_wd_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        if (!dead.empty()) Logger::debug(fmt("pruned %zu dead watch entries", dead.size()));
    }

    void handle_events(const char* data, size_t len, std::stop_token st) {
        std::vector<fs::path> changed;
        size_t offset = 0;
        while (offset + sizeof(struct inotify_event) <= len) {
            const auto* event = reinterpret_cast<const struct inotify_event*>(data + offset);
            offset += sizeof(struct inotify_event) + event->len;

            if ((event->mask & IN_Q_OVERFLOW) != 0) {
                Logger::warn("inotify queue overflowed: events were lost, rescanning the vault");
                ++stats_.inotify_overflows;
                {
                    // The lock must be released before rescanning: add_watch()
                    // takes it again.
                    std::lock_guard<std::mutex> lock(mtx_);
                    by_wd_.clear();
                    watched_paths_.clear();
                }
                scan_and_watch(st, /*scan_files=*/true);
                return;
            }

            if (event->wd < 0) continue;
            fs::path dir;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                const auto it = by_wd_.find(event->wd);
                if (it == by_wd_.end()) continue;
                dir = it->second;
                if ((event->mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF)) != 0) {
                    watched_paths_.erase(dir.string());
                    by_wd_.erase(it);
                }
            }

            const std::string name = (event->len > 0) ? std::string(event->name) : std::string();
            if (name.empty()) continue;
            if (name[0] == '.') continue;  // editor swap files, our .tmp files

            const fs::path full = dir / name;
            const bool is_dir = (event->mask & IN_ISDIR) != 0;

            if (is_dir) {
                // scan_files=true: a directory can appear already populated.
                if ((event->mask & (IN_CREATE | IN_MOVED_TO)) != 0) add_watch(full, /*scan_files=*/true);
                continue;
            }
            if (full.extension() != ".md") continue;

            if ((event->mask & (IN_DELETE | IN_MOVED_FROM)) != 0) {
                Logger::debug(fmt("removed locally: %s (Notion pages are not deleted)", full.string()));
                tracker_.forget(full);
                continue;
            }
            ++stats_.events_received;
            changed.push_back(full);
        }

        for (const fs::path& path : changed) {
            tracker_.note_event(path);
        }
    }

    // Applies loop suppression and hands settled paths to the sync worker.
    void flush_settled_changes() {
        for (const fs::path& path : tracker_.take_ready(quiet_window_)) {
            const auto mtime = file_mtime(path);
            if (suppression_.should_ignore(path, mtime)) {
                ++stats_.events_suppressed;
                Logger::trace(fmt("suppressed self-induced event: %s", path.string()));
                continue;
            }
            SyncTask task;
            task.kind = TaskKind::LocalFileChanged;
            task.filepath = path.string();
            ++stats_.events_scheduled;
            Logger::debug(fmt("queued local change: %s (queue depth %zu)", path.string(),
                              queue_.size() + 1));
            queue_.push(std::move(task));
        }
    }

public:
    size_t watch_count() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return static_cast<size_t>(by_wd_.size());
    }

private:

    fs::path root_;
    TaskQueue& queue_;
    SuppressionRegistry& suppression_;
    FileEventTracker& tracker_;
    Stats& stats_;
    std::chrono::seconds rescan_interval_;
    std::chrono::milliseconds quiet_window_;

    int inotify_fd_ = -1;
    mutable std::mutex mtx_;
    std::unordered_map<int, fs::path> by_wd_;
    std::unordered_set<std::string> watched_paths_;
};

// =============================================================================
//  Notion API client
// =============================================================================
struct ApiResult {
    bool ok = false;            // transport level success (HTTP response received)
    int status = 0;             // 0 = connection/transport error
    std::string body;
    std::string error;          // transport error or Notion error message
    std::string error_code;
    bool transient = false;     // retry may help (429 / 5xx / plain network issue)

    bool http_2xx() const { return status >= 200 && status < 300; }
};

class NotionClient {
public:
    explicit NotionClient(const Config& cfg) : cfg_(cfg) {}

    bool enabled() const { return enabled_; }

    void set_enabled(bool value) { enabled_ = value; }

    // Lazily resolves the title property of the target database ("Name" is the
    // common case, but the API rejects pushes when it differs).
    std::string title_property() {
        std::lock_guard<std::mutex> lock(title_mtx_);
        if (!title_property_.empty()) return title_property_;
        title_property_ = "Name";
        if (cfg_.notion_database_id.empty()) return title_property_;

        const ApiResult res = request("GET", "/v1/databases/" + cfg_.notion_database_id, nullptr);
        if (!res.ok || !res.http_2xx()) {
            if (res.status == 400) {
                Logger::warn("could not read the database schema; the integration may lack access");
            }
            return title_property_;
        }
        const json j = json::parse(res.body, nullptr, false);
        if (j.is_discarded() || !j.contains("properties")) return title_property_;
        for (auto it = j["properties"].begin(); it != j["properties"].end(); ++it) {
            if (json_string(it.value(), "type") == "title") {
                title_property_ = it.key();
                Logger::debug(fmt("database title property: %s", title_property_));
                return title_property_;
            }
        }
        return title_property_;
    }

    void set_title_property(const std::string& name) {
        std::lock_guard<std::mutex> lock(title_mtx_);
        title_property_ = name;
    }

    ApiResult database_info() { return request("GET", "/v1/databases/" + cfg_.notion_database_id, nullptr); }

    ApiResult query_database(const json& body) {
        return request("POST", "/v1/databases/" + cfg_.notion_database_id + "/query", &body);
    }

    ApiResult fetch_block_children(const std::string& block_id) {
        const ApiResult res =
            request("GET", "/v1/blocks/" + block_id + "/children?page_size=100", nullptr);
        if (!res.ok || !res.http_2xx()) return res;
        // Follow pagination for pages with more than 100 blocks.
        json merged = json::parse(res.body, nullptr, false);
        if (merged.is_discarded()) {
            ApiResult bad = res;
            bad.error = "unparseable blocks response";
            return bad;
        }
        if (!merged.contains("results") || !merged["results"].is_array()) {
            ApiResult bad = res;
            bad.error = "block list without results";
            return bad;
        }
        int guard = 0;
        while (json_bool(merged, "has_more") && guard++ < 20) {
            const std::string cursor = json_string(merged, "next_cursor");
            if (cursor.empty()) break;
            const ApiResult page = request(
                "GET", "/v1/blocks/" + block_id + "/children?page_size=100&start_cursor=" + cursor,
                nullptr);
            if (!page.ok || !page.http_2xx()) break;
            const json more = json::parse(page.body, nullptr, false);
            if (more.is_discarded() || !more.contains("results")) break;
            for (const auto& item : more["results"]) merged["results"].push_back(item);
            merged["has_more"] = json_bool(more, "has_more");
            merged["next_cursor"] = json_string(more, "next_cursor");
        }
        ApiResult good = res;
        good.body = merged.dump();
        return good;
    }

    // Creates a page whose parent is the configured database.
    ApiResult create_page(const std::string& title, const std::string& markdown_body) {
        const std::string title_prop = title_property();

        json children = json::array();
        const std::vector<std::string> chunks = split_rich_text(markdown_body, kMaxRichTextChunk);
        for (size_t i = 0; i < chunks.size() && i < kMaxCaptureChildren; ++i) {
            children.push_back(json{{"object", "block"},
                                    {"type", "paragraph"},
                                    {"paragraph",
                                     {{"rich_text",
                                       json::array({json{{"type", "text"},
                                                         {"text", {{"content", chunks[i]}}}}})}}}});
        }
        if (chunks.size() > kMaxCaptureChildren) {
            Logger::warn(fmt("note is longer than %zu paragraphs; only the first %zu are pushed",
                             kMaxCaptureChildren, kMaxCaptureChildren));
        }

        json body = {
            {"parent", {{"database_id", cfg_.notion_database_id}}},
            {"properties",
             {{title_prop,
               {{"title",
                 json::array({json{{"type", "text"},
                                   {"text", {{"content", truncate_utf8(title, 2000)}}}}})}}}}},
            {"children", children},
        };
        return request("POST", "/v1/pages", &body);
    }

    static std::vector<std::string> split_rich_text(const std::string& text, size_t max_len) {
        std::vector<std::string> chunks;
        if (text.empty()) return {""};
        size_t start = 0;
        while (start < text.size()) {
            size_t end = std::min(start + max_len, text.size());
            if (end < text.size()) {
                const size_t nl = text.rfind('\n', end);
                if (nl != std::string::npos && nl > start) end = nl + 1;  // prefer line breaks
            }
            chunks.push_back(text.substr(start, end - start));
            start = end;
        }
        return chunks;
    }

    // Truncates without splitting a UTF-8 sequence.
    static std::string truncate_utf8(const std::string& s, size_t max_bytes) {
        if (s.size() <= max_bytes) return s;
        size_t end = max_bytes;
        while (end > 0 && (static_cast<unsigned char>(s[end]) & 0xC0) == 0x80) --end;
        return s.substr(0, end);
    }

private:
    ApiResult request(const std::string& method, const std::string& path, const json* body) {
        ApiResult out;
        if (cfg_.notion_api_key.empty()) {
            out.error = "NOTION_API_KEY is not configured";
            return out;
        }

        std::string payload;
        if (body != nullptr) payload = body->dump();

        const int attempts = std::max(1, cfg_.max_push_attempts);
        for (int attempt = 1; attempt <= attempts; ++attempt) {
            ApiResult res = perform(method, path, payload);

            // Retry only when the server explicitly refused the request: a
            // connection error (status == 0) may have reached Notion already,
            // and re-sending could create a duplicate page.
            if (res.ok && (res.status == 429 || (res.status >= 500 && res.status < 600))) {
                res.transient = true;
                if (attempt < attempts) {
                    const auto backoff = std::chrono::milliseconds(500 * (1 << (attempt - 1)));
                    Logger::warn(fmt("Notion %s %s -> HTTP %d (attempt %d/%d), retrying in %lld ms",
                                     method, path, res.status, attempt, attempts,
                                     static_cast<long long>(backoff.count())));
                    interruptible_sleep(backoff, std::stop_token{});
                    continue;
                }
                return res;
            }
            return res;
        }
        out.error = "unreachable";
        return out;
    }

    ApiResult perform(const std::string& method, const std::string& path, const std::string& payload) {
        ApiResult out;
        // Plain HTTP works without a TLS backend; HTTPS cannot be attempted at
        // all when the library was built without one.
        if (!TELEGROBSIDIAN_HAS_SSL && starts_with(cfg_.notion_base_url, "https://")) {
            out.error = "this build has no TLS support (rebuild with OpenSSL)";
            return out;
        }
        try {
            httplib::Client client(cfg_.notion_base_url);
            if (!client.is_valid()) {
                out.error = fmt("invalid NOTION_BASE_URL: %s", cfg_.notion_base_url);
                return out;
            }
            client.set_connection_timeout(cfg_.http_connect_timeout);
            client.set_read_timeout(cfg_.http_read_timeout);
            client.set_write_timeout(cfg_.http_read_timeout);
#if TELEGROBSIDIAN_HAS_SSL
            client.enable_server_certificate_verification(true);
#endif

            const httplib::Headers headers = {
                {"Authorization", "Bearer " + cfg_.notion_api_key},
                {"Notion-Version", cfg_.notion_version},
                {"Content-Type", "application/json"},
                {"User-Agent", fmt("telegrobsidian/%s", std::string(kVersion))},
            };

            httplib::Result res;
            if (method == "GET") {
                res = client.Get(path, headers);
            } else {
                res = client.Post(path, headers, payload, "application/json");
            }

            if (!res) {
                // Negative status codes are transport errors (0 means "unknown"),
                // so they can never be confused with an HTTP status.
                const int code = static_cast<int>(res.error());
                out.status = (code == static_cast<int>(httplib::Error::Success)) ? -1 : -code;
                out.error = httplib::to_string(res.error());
                out.transient = (code == static_cast<int>(httplib::Error::Connection) ||
                                 code == static_cast<int>(httplib::Error::ConnectionTimeout) ||
                                 code == static_cast<int>(httplib::Error::Timeout) ||
                                 code == static_cast<int>(httplib::Error::Read) ||
                                 code == static_cast<int>(httplib::Error::Write));
                return out;
            }

            out.ok = true;
            out.status = res->status;
            out.body = res->body;
            if (!out.http_2xx()) {
                const json err = json::parse(res->body, nullptr, false);
                if (!err.is_discarded() && err.contains("message")) {
                    out.error_code = json_string(err, "code");
                    out.error = json_string(err, "message");
                } else {
                    out.error = truncate_utf8(res->body, 400);
                }
            }
            return out;
        } catch (const std::exception& e) {
            out.error = fmt("HTTP client error: %s", std::string(e.what()));
            return out;
        }
    }

    const Config& cfg_;
    bool enabled_ = true;
    std::mutex title_mtx_;
    std::string title_property_;
};

// =============================================================================
//  Notion -> Markdown rendering
// =============================================================================
std::string render_rich_text(const json& rich_text) {
    if (!rich_text.is_array()) return {};
    std::string out;
    for (const auto& part : rich_text) {
        const json text = part.contains("text") ? part["text"] : json::object();
        std::string content;
        if (text.contains("content") && text["content"].is_string()) {
            content = text["content"].get<std::string>();
        } else if (json_string(part, "type") == "equation" && part.contains("equation")) {
            content = json_string(part["equation"], "expression");
        }
        if (content.empty() && json_string(part, "type") != "mention") continue;

        const json ann = part.contains("annotations") ? part["annotations"] : json::object();
        const bool bold = json_bool(ann, "bold");
        const bool italic = json_bool(ann, "italic");
        const bool code = json_bool(ann, "code");
        const bool strike = json_bool(ann, "strikethrough");
        const bool underline = json_bool(ann, "underline");

        std::string href;
        if (text.contains("link") && text["link"].is_object()) {
            href = json_string(text["link"], "url");
        } else if (json_string(part, "type") == "mention") {
            content = part.contains("plain_text") ? json_string(part, "plain_text") : content;
        }

        std::string piece = content;
        if (bold) piece = "**" + piece + "**";
        if (italic) piece = "*" + piece + "*";
        if (strike) piece = "~~" + piece + "~~";
        if (code) piece = "`" + piece + "`";
        if (underline) piece = "<u>" + piece + "</u>";
        if (!href.empty() && !code) piece = "[" + piece + "](" + href + ")";
        out += piece;
    }
    return out;
}

class NotionRenderer {
public:
    // Renders a page (already carrying a "blocks" array of block objects) as
    // Obsidian-flavoured markdown.
    static std::string render_page(const json& page) {
        std::string out;
        if (page.contains("blocks") && page["blocks"].is_array()) {
            render_blocks(page["blocks"], out, 0);
        }
        // Collapse trailing blank lines.
        while (!out.empty() && out.back() == '\n') out.pop_back();
        if (!out.empty()) out += "\n";
        return out;
    }

    static std::string page_title(const json& page, const std::string& title_property) {
        if (!page.contains("properties")) return "Untitled";
        const json& props = page["properties"];
        auto read = [&](const std::string& key) -> std::string {
            if (!props.contains(key)) return {};
            const json& prop = props[key];
            if (!prop.contains("title") || !prop["title"].is_array()) return {};
            std::string title;
            for (const auto& part : prop["title"]) {
                title += json_string(part, "plain_text");
            }
            return title;
        };
        std::string title = read(title_property);
        if (title.empty()) {
            for (auto it = props.begin(); it != props.end(); ++it) {
                title = read(it.key());
                if (!title.empty()) break;
            }
        }
        title = trim(title);
        if (title.empty()) title = "Untitled";
        return title;
    }

private:
    static void render_blocks(const json& blocks, std::string& out, int depth) {
        if (depth > 4 || !blocks.is_array()) return;
        for (const auto& block : blocks) {
            if (!block.is_object()) continue;
            const std::string type = json_string(block, "type", "unsupported");
            const json data = block.contains(type) ? block[type] : json::object();
            const std::string text = render_rich_text(json_array(data, "rich_text"));

            if (type == "paragraph") {
                out += text + "\n\n";
            } else if (type == "heading_1") {
                out += "# " + text + "\n\n";
            } else if (type == "heading_2") {
                out += "## " + text + "\n\n";
            } else if (type == "heading_3") {
                out += "### " + text + "\n\n";
            } else if (type == "bulleted_list_item" || type == "toggle") {
                out += "- " + text + "\n";
            } else if (type == "numbered_list_item") {
                out += "1. " + text + "\n";
            } else if (type == "to_do") {
                out += json_bool(data, "checked") ? "- [x] " : "- [ ] ";
                out += text + "\n";
            } else if (type == "quote") {
                out += "> " + text + "\n\n";
            } else if (type == "callout") {
                const std::string emoji = data.contains("icon") && data["icon"].is_object()
                                              ? json_string(data["icon"], "emoji")
                                              : std::string{};
                out += "> " + (emoji.empty() ? std::string("**Note:** ") : emoji + " ") + text + "\n\n";
            } else if (type == "code") {
                out += "```" + json_string(data, "language") + "\n" + text + "\n```\n\n";
            } else if (type == "divider") {
                out += "---\n\n";
            } else if (type == "equation") {
                out += "$$\n" + json_string(data, "expression") + "\n$$\n\n";
            } else if (type == "bookmark" || type == "link_preview") {
                out += "[" + json_string(data, "url") + "](" + json_string(data, "url") +
                       ")\n\n";
            } else if (type == "image" || type == "audio" || type == "video" || type == "file" ||
                       type == "pdf") {
                out += render_file_block(type, data);
            } else if (type == "table_row") {
                const json& cells = data.contains("cells") ? data["cells"] : json::array();
                out += "|";
                for (const auto& cell : cells) out += " " + render_rich_text(cell) + " |";
                out += "\n";
            } else if (type == "child_page") {
                out += "## " + json_string(data, "title", "Untitled") + "\n\n";
            } else if (type == "child_database") {
                out += "**Database:** " + json_string(data, "title", "Untitled") + "\n\n";
            } else if (type == "unsupported" || type.empty()) {
                out += "<!-- unsupported Notion block -->\n";
            } else {
                out += fmt("<!-- unsupported Notion block: %s -->\n", type);
                if (!text.empty()) out += text + "\n";
            }

            if (block.contains("children") && block["children"].is_array() && !block["children"].empty()) {
                std::string nested;
                render_blocks(block["children"], nested, depth + 1);
                if (!nested.empty()) {
                    if (type == "bulleted_list_item" || type == "numbered_list_item" ||
                        type == "to_do" || type == "toggle") {
                        out += indent_block(nested);
                    } else {
                        out += nested;
                    }
                }
            }
        }
    }

    static std::string render_file_block(const std::string& type, const json& data) {
        std::string url;
        if (data.contains("file") && data["file"].is_object()) {
            url = json_string(data["file"], "url");
        } else if (data.contains("external") && data["external"].is_object()) {
            url = json_string(data["external"], "url");
        }
        if (url.empty()) return fmt("<!-- %s without a reachable URL -->\n", type);
        const std::string caption = render_rich_text(json_array(data, "caption"));
        if (type == "image") return "![" + caption + "](" + url + ")\n\n";
        if (type == "video" || type == "audio") return "[" + (caption.empty() ? type : caption) + "](" + url + ")\n\n";
        return "[" + (caption.empty() ? std::string("attachment") : caption) + "](" + url + ")\n\n";
    }

    static std::string indent_block(const std::string& text) {
        std::string out;
        for (const std::string& line : split_lines(text)) {
            out += line.empty() ? std::string("\n") : "  " + line + "\n";
        }
        return out;
    }
};

// =============================================================================
//  Telegram update parsing
// =============================================================================
struct TelegramMessage {
    bool valid = false;
    bool is_edit = false;
    std::string text;
    std::string author;
    int64_t chat_id = 0;
    int64_t message_id = 0;
    int64_t date = 0;
    std::string kind;
};

TelegramMessage parse_telegram_update(const json& update) {
    TelegramMessage msg;
    for (const char* key : {"message", "edited_message", "channel_post", "edited_channel_post"}) {
        if (!update.contains(key) || !update[key].is_object()) continue;
        const json& m = update[key];
        msg.kind = key;
        msg.is_edit = (std::string(key).find("edited") == 0);
        msg.chat_id = m.contains("chat") ? json_int(m["chat"], "id") : 0;
        msg.message_id = json_int(m, "message_id");
        msg.date = json_int(m, "date");
        if (m.contains("text") && m["text"].is_string()) {
            msg.text = m["text"].get<std::string>();
        } else if (m.contains("caption") && m["caption"].is_string()) {
            msg.text = m["caption"].get<std::string>();
        }
        if (m.contains("from") && m["from"].is_object()) {
            const json& from = m["from"];
            msg.author = json_string(from, "username");
            if (msg.author.empty()) {
                msg.author = trim(json_string(from, "first_name") + " " +
                                  json_string(from, "last_name"));
            }
        }
        if (m.contains("chat") && m["chat"].is_object()) {
            const std::string chat_title = json_string(m["chat"], "title");
            if (!chat_title.empty()) msg.author = chat_title;
        }
        msg.valid = msg.message_id != 0;
        return msg;
    }
    return msg;
}

std::string sanitize_filename_component(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' ||
                        c == '.';
        out += ok ? c : '_';
    }
    if (out.size() > 120) out = out.substr(0, 120);
    return out.empty() ? std::string("unnamed") : out;
}

// =============================================================================
//  Daemon
// =============================================================================
volatile std::sig_atomic_t g_stop_requested = 0;

class Daemon {
public:
    Daemon(Config cfg, CliOptions cli)
        : cfg_(std::move(cfg)),
          cli_(std::move(cli)),
          suppression_(cfg_.dedup_cooldown),
          writer_(cfg_, suppression_),
          watcher_(cfg_, queue_, suppression_, tracker_, stats_),
          state_(cfg_.state_file, cfg_.notion_page_cache_max),
          notion_(cfg_) {}

    int run() {
        install_signal_handlers();

        if (!prepare()) return EXIT_FAILURE;

        if (cli_.self_test) return self_test();

        if (cli_.once && !notion_.enabled()) {
            Logger::error("--once requires Notion sync; configure NOTION_API_KEY and "
                          "NOTION_DATABASE_ID (or run the daemon normally)");
            return EXIT_FAILURE;
        }

        std::error_code ec;
        fs::create_directories(cfg_.vault_path, ec);
        if (ec) {
            Logger::error(fmt("cannot create vault %s: %s", cfg_.vault_path.string(), ec.message()));
            return EXIT_FAILURE;
        }

        Logger::success(fmt("Telegrobsidian %s starting", std::string(kVersion)));
        log_config_summary();

        // Watch first, catch up second: anything modified from here on is
        // guaranteed to be seen as an inotify event.
        if (!watcher_.prime(std::stop_token{})) {
            fatal_error_ = true;
            g_stop_requested = 1;  // fail fast: without inotify there is no sync
        }

        std::jthread worker([this](std::stop_token st) { guarded("sync-worker", [&] { worker_loop(st); }); });
        std::jthread watcher([this](std::stop_token st) { guarded("file-watcher", [&] { watcher_.run(st); }); });
        std::jthread poller;
        if (notion_.enabled() && !cli_.once) {
            poller = std::jthread([this](std::stop_token st) { guarded("notion-poller", [&] { poller_loop(st); }); });
        }

        server_.set_payload_max_length(cfg_.max_payload_bytes);
        wire_http_routes();

        std::jthread http([this] { guarded("http-server", [&] {
            Logger::success(fmt("HTTP webhook listening on %s:%d", cfg_.webhook_bind, cfg_.webhook_port));
            const bool ok = server_.listen(cfg_.webhook_bind, cfg_.webhook_port);
            if (!ok) {
                Logger::error(fmt("cannot bind %s:%d (already in use?)", cfg_.webhook_bind,
                                  cfg_.webhook_port));
                ++stats_.errors;
                fatal_error_ = true;
                g_stop_requested = 1;
            }
        }); });

        catch_up_existing_files();

        register_telegram_webhook();

        if (cli_.once) {
            // Single pass: poll, mirror, then wait for the worker to finish the
            // resulting tasks before shutting down.
            poll_notion_once(std::stop_token{});
            const auto deadline = std::chrono::steady_clock::now() + 5 * cfg_.drain_timeout;
            while (queue_.size() > 0 && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(50ms);
            }
            Logger::info(fmt("--once: poll finished (%zu task(s) pending), shutting down", queue_.size()));
            g_stop_requested = 1;
        }

        supervise();
        Logger::info(fmt("stopped: %llu task(s) processed, %llu file(s) written, %llu push(es) ok, "
                          "%llu error(s)",
                          static_cast<unsigned long long>(stats_.tasks_processed.load()),
                          static_cast<unsigned long long>(stats_.files_written.load()),
                          static_cast<unsigned long long>(stats_.pushes_succeeded.load()),
                          static_cast<unsigned long long>(stats_.errors.load())));
        return fatal_error_ ? EXIT_FAILURE : EXIT_SUCCESS;
    }

private:
    // ------------------------------------------------------------- setup
    static void install_signal_handlers() {
        struct sigaction action {};
        action.sa_handler = [](int) { g_stop_requested = 1; };
        sigemptyset(&action.sa_mask);
        action.sa_flags = 0;  // no SA_RESTART: interrupt blocking accept()/poll()
        ::sigaction(SIGINT, &action, nullptr);
        ::sigaction(SIGTERM, &action, nullptr);

        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigemptyset(&ignore.sa_mask);
        ::sigaction(SIGPIPE, &ignore, nullptr);  // a dead webhook client must not kill us
    }

    bool prepare() {
        for (const auto& warning : cfg_.warnings) Logger::warn(warning);

        if (!state_.load()) {
            Logger::warn("continuing with an empty sync state");
        }

        if (cfg_.notion_configured()) {
            const std::vector<std::string> problems = cfg_.validate();
            for (const auto& p : problems) Logger::warn(std::string("configuration: ") + p);
            if (!problems.empty()) ++stats_.errors;
        }

        const bool notion_needs_tls = starts_with(cfg_.notion_base_url, "https://");
        if (!cfg_.notion_configured()) {
            notion_.set_enabled(false);
            Logger::warn("Notion sync disabled (set NOTION_API_KEY and NOTION_DATABASE_ID to enable)");
        } else if (notion_needs_tls && !TELEGROBSIDIAN_HAS_SSL) {
            notion_.set_enabled(false);
            Logger::error("Notion is configured but this binary was built without TLS support; "
                          "rebuild with OpenSSL (-DTELEGROBSIDIAN_WITH_SSL=ON). Notion sync is "
                          "disabled.");
            fatal_error_ = true;
        } else if (!notion_needs_tls) {
            // Plain HTTP is useful against a local gateway or a test double, but
            // the integration token travels in clear text.
            const bool loopback = contains_ci(cfg_.notion_base_url, "127.0.0.1") ||
                                  contains_ci(cfg_.notion_base_url, "localhost") ||
                                  contains_ci(cfg_.notion_base_url, "[::1]");
            if (loopback) {
                Logger::info(fmt("NOTION_BASE_URL is plain HTTP on loopback (%s): assuming a test "
                                  "double", cfg_.notion_base_url));
            } else {
                Logger::warn(fmt("NOTION_BASE_URL is not HTTPS (%s): the integration token will be "
                                  "sent in clear text", cfg_.notion_base_url));
            }
        }

#if TELEGROBSIDIAN_HAS_SSL
        if (notion_.enabled() && cfg_.use_system_ca_bundle) {
            if (const char* file = std::getenv("SSL_CERT_FILE"); file != nullptr && *file != '\0') {
                Logger::info(fmt("using CA bundle from SSL_CERT_FILE: %s", std::string(file)));
            } else if (const std::string bundle = find_ca_bundle(); !bundle.empty()) {
                ::setenv("SSL_CERT_FILE", bundle.c_str(), 0);
                Logger::info(fmt("using system CA bundle: %s", bundle));
            } else {
                Logger::warn("no system CA bundle found; Notion HTTPS requests may fail");
            }
        }
#endif
        return true;
    }

    static std::string find_ca_bundle() {
        static const char* candidates[] = {
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/etc/ssl/ca-bundle.pem",
            "/etc/ssl/cert.pem",
            "/usr/local/etc/openssl/cert.pem",
            "/opt/homebrew/etc/openssl@3/cert.pem",
        };
        std::error_code ec;
        for (const char* candidate : candidates) {
            if (fs::exists(candidate, ec)) return candidate;
        }
        return {};
    }

    void log_config_summary() const {
        Logger::info(fmt("vault:        %s", cfg_.vault_path.string()));
        Logger::info(fmt("state:        %s", cfg_.state_file.string()));
        Logger::info(fmt("mirror dirs:  %s/ (pull), %s/ (push)", cfg_.notion_mirror_dir,
                         cfg_.obsidian_push_dir));
        Logger::info(fmt("notion:       %s (db %s, version %s, poll %llds)",
                         notion_.enabled() ? "enabled" : "disabled",
                         cfg_.notion_database_id.empty() ? "-" : redact(cfg_.notion_database_id),
                         cfg_.notion_version, static_cast<long long>(cfg_.notion_poll_interval.count())));
        Logger::info(fmt("telegram:     %s", cfg_.telegram_bot_token.empty()
                                                    ? "webhook only (no bot token set)"
                                                    : "bot token configured"));
        Logger::info(fmt("tls:          %s", TELEGROBSIDIAN_HAS_SSL ? "openssl" : "disabled"));
    }

    // HTTPS needs a TLS backend; plain HTTP is accepted for loopback endpoints
    // (a local Bot API server, or the test double used by the smoke test).
    bool api_base_reachable(const std::string& base, const char* what) const {
        if (!starts_with(base, "https://")) return true;
        if (TELEGROBSIDIAN_HAS_SSL) return true;
        Logger::warn(fmt("%s requires TLS but this build has no TLS support; skipping", std::string(what)));
        return false;
    }

    static std::string redact(const std::string& value) {
        if (value.size() <= 8) return "***";
        return value.substr(0, 4) + "..." + value.substr(value.size() - 4);
    }

    // Every long running thread runs through this: an unexpected exception is
    // reported and shuts the daemon down so the service manager can restart it
    // cleanly instead of leaving a half-dead process behind (or calling
    // std::terminate because an exception escaped a thread).
    template <typename F>
    void guarded(const char* thread_name, F&& body) {
        try {
            body();
        } catch (const std::exception& e) {
            ++stats_.errors;
            Logger::error(fmt("%s thread aborted: %s", std::string(thread_name),
                              std::string(e.what())));
            fatal_error_ = true;
            g_stop_requested = 1;
        } catch (...) {
            ++stats_.errors;
            Logger::error(fmt("%s thread aborted with an unknown exception",
                              std::string(thread_name)));
            fatal_error_ = true;
            g_stop_requested = 1;
        }
    }

    // Startup catch-up: files that changed while the daemon was not running are
    // pushed now. On a *first* run over an existing vault the fingerprints are
    // recorded instead of pushing, so the daemon does not turn a whole vault
    // into pages behind the user's back (--once and PUSH_EXISTING_ON_STARTUP=true
    // opt into the bulk upload).
    void catch_up_existing_files() {
        const bool seed = state_.is_pristine() && !cfg_.push_existing_on_startup && !cli_.once;
        if (seed) {
            Logger::warn("first run over an existing vault: existing files are registered as already "
                         "synced. Set PUSH_EXISTING_ON_STARTUP=true (or use --once) to upload them "
                         "to Notion now.");
        }

        size_t scanned = 0, queued = 0, seeded = 0, unchanged = 0, excluded = 0;
        std::error_code ec;
        fs::recursive_directory_iterator it(cfg_.vault_path,
                                            fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            Logger::warn(fmt("cannot scan %s: %s", cfg_.vault_path.string(), ec.message()));
            return;
        }
        for (const auto& entry : it) {
            std::error_code type_ec;
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(type_ec) && !type_ec) {
                if (!name.empty() && name[0] == '.') it.disable_recursion_pending();
                continue;
            }
            if (!entry.is_regular_file(type_ec) || type_ec) continue;
            if (entry.path().extension() != ".md") continue;
            if (in_mirror_dir(entry.path()) && !cfg_.push_mirror_files) {
                ++excluded;
                continue;
            }
            ++scanned;

            const auto content = VaultWriter::read_if_exists(entry.path());
            if (!content.has_value()) continue;
            const std::string fingerprint = hex64(fnv1a(*content));
            if (auto previous = state_.pushed_hash(path_key(entry.path()));
                previous.has_value() && *previous == fingerprint) {
                ++unchanged;
                continue;
            }
            if (seed) {
                state_.set_pushed_hash(path_key(entry.path()), fingerprint);
                ++seeded;
                continue;
            }
            SyncTask task;
            task.kind = TaskKind::LocalFileChanged;
            task.filepath = entry.path().string();
            queue_.push(std::move(task));
            ++queued;
        }
        state_.save();

        Logger::info(fmt("startup scan: %zu markdown file(s) - %zu queued, %zu unchanged, "
                          "%zu excluded",
                          scanned, queued, unchanged, excluded));
        if (seeded > 0) {
            Logger::success(fmt("registered %zu existing file(s) as already synced", seeded));
        }
    }

    // ------------------------------------------------------------- supervisor
    void supervise() {
        const auto started = std::chrono::steady_clock::now();
        auto last_state_save = started;

        while (g_stop_requested == 0) {
            std::this_thread::sleep_for(100ms);
            if (std::chrono::steady_clock::now() - last_state_save > 10s) {
                last_state_save = std::chrono::steady_clock::now();
                state_.save();
            }
        }

        Logger::info("Shutdown requested; stopping threads");
        server_.stop();
        // jthread destructors request_stop() + join; the worker drains the queue
        // first because pop() keeps returning queued tasks until it is empty.
    }

    // -------------------------------------------------------------- workers
    void worker_loop(std::stop_token st) {
        Logger::debug("sync worker started");
        SyncTask task;
        std::optional<std::chrono::steady_clock::time_point> drain_deadline;
        while (queue_.pop(task, st)) {
            try {
                handle_task(task);
            } catch (const std::exception& e) {
                ++stats_.errors;
                Logger::error(fmt("unhandled error in %s task: %s", std::string(task_kind_name(task.kind)),
                                  std::string(e.what())));
            }
            ++stats_.tasks_processed;

            if (st.stop_requested()) {
                // Bounded drain: hand over what Telegram already delivered, but
                // never hang a shutdown on a large backlog.
                if (!drain_deadline.has_value()) {
                    drain_deadline = std::chrono::steady_clock::now() + cfg_.drain_timeout;
                    Logger::info(fmt("draining %zu queued task(s) before exit", queue_.size()));
                }
                if (std::chrono::steady_clock::now() > *drain_deadline) {
                    Logger::warn(fmt("drain timeout reached with %zu task(s) left", queue_.size()));
                    break;
                }
            }
        }
        state_.save();
        Logger::debug("sync worker stopped");
    }

    void handle_task(const SyncTask& task) {
        switch (task.kind) {
            case TaskKind::TelegramWebhook:  handle_telegram(task.payload); break;
            case TaskKind::LocalFileChanged: handle_local_file(task.filepath); break;
        }
    }

    // ------------------------------------------------------------- telegram
    void handle_telegram(const std::string& body) {
        const json update = json::parse(body, nullptr, false);
        if (update.is_discarded() || !update.is_object()) {
            Logger::warn("webhook payload is not valid JSON");
            return;
        }
        const TelegramMessage msg = parse_telegram_update(update);
        if (!msg.valid) {
            Logger::debug("webhook update has no message; ignored");
            return;
        }
        if (trim(msg.text).empty()) {
            Logger::info(fmt("ignoring message %lld from chat %lld without text or caption",
                             static_cast<long long>(msg.message_id), static_cast<long long>(msg.chat_id)));
            return;
        }

        std::string title = trim(split_lines(msg.text).front());
        if (title.empty()) title = "Telegram Capture";
        if (title.size() > 60) title = NotionClient::truncate_utf8(title, 60);
        if (msg.is_edit) title += " (edited)";

        const std::string filename =
            fmt("%s/tg_%s_%s.md", cfg_.telegram_inbox_dir,
                sanitize_filename_component(std::to_string(msg.chat_id)),
                sanitize_filename_component(std::to_string(msg.message_id)));

        const std::string date =
            msg.date > 0 ? iso8601_utc(std::chrono::system_clock::from_time_t(msg.date)) : iso8601_utc();
        std::string frontmatter;
        frontmatter += fmt("source: telegram\n");
        frontmatter += fmt("chat_id: %lld\n", static_cast<long long>(msg.chat_id));
        frontmatter += fmt("message_id: %lld\n", static_cast<long long>(msg.message_id));
        frontmatter += fmt("date: %s\n", date);
        if (!msg.author.empty()) frontmatter += fmt("author: %s\n", yaml_quote(msg.author));

        const WriteOutcome write =
            writer_.write_markdown(filename, frontmatter, msg.text, title);
        if (!write.ok) {
            ++stats_.errors;
            Logger::error(fmt("cannot store Telegram message: %s", write.error));
            return;
        }
        if (write.unchanged) {
            ++stats_.files_unchanged;
        } else {
            ++stats_.files_written;
            Logger::success(fmt("captured Telegram message -> %s", write.path.filename().string()));
        }

        if (!notion_.enabled()) return;

        // Skip the push when the stored content has not changed since the last
        // successful push (Telegram re-delivers webhooks, and edits rewrite the
        // same file).
        const auto content = VaultWriter::read_if_exists(write.path);
        if (!content.has_value()) return;
        const std::string fingerprint = hex64(fnv1a(*content));
        if (auto previous = state_.pushed_hash(path_key(write.path));
            previous.has_value() && *previous == fingerprint) {
            ++stats_.pushes_skipped;
            Logger::debug("Telegram message already pushed to Notion; skipping");
            return;
        }

        push_to_notion(title, strip_frontmatter(*content), write.path, fingerprint);
    }

    // ---------------------------------------------------------- local files
    void handle_local_file(const std::string& path) {
        const fs::path file(path);

        if (!fs::exists(file)) {
            state_.forget_pushed_hash(path_key(file));
            Logger::debug(fmt("skipping vanished file: %s", path));
            return;
        }

        if (in_mirror_dir(file) && !cfg_.push_mirror_files) {
            ++stats_.pushes_skipped;
            Logger::debug(fmt("not pushing %s: Notion mirrors are pull-only (see NOTION_PUSH_MIRROR_FILES)",
                              path));
            return;
        }

        // Wait for the writer to settle so a partially written file is never
        // pushed. Editors commonly truncate + write, which produces a window
        // where the file is incomplete.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const auto mtime = file_mtime(file);
            if (!mtime.has_value()) break;
            const auto age = std::chrono::system_clock::now() - *mtime;
            if (age > cfg_.quiesce_window) break;
            Logger::trace(fmt("waiting for %s to settle (age %lld ms)", file.string(),
                              static_cast<long long>(
                                  std::chrono::duration_cast<std::chrono::milliseconds>(age).count())));
            interruptible_sleep(cfg_.quiesce_window, std::stop_token{});
        }

        const auto content = VaultWriter::read_if_exists(file);
        if (!content.has_value()) {
            Logger::warn(fmt("cannot read %s; skipping push", path));
            return;
        }

        const std::string fingerprint = hex64(fnv1a(*content));
        if (auto previous = state_.pushed_hash(path_key(file));
            previous.has_value() && *previous == fingerprint) {
            ++stats_.pushes_skipped;
            Logger::debug(fmt("content unchanged since last push: %s", path));
            return;
        }

        if (!notion_.enabled()) return;

        const std::string fallback_title = file.stem().string();
        const std::string title = fmt("Obsidian: %s", document_title(*content, fallback_title));
        push_to_notion(title, strip_frontmatter(*content), file, fingerprint);
    }

    bool in_mirror_dir(const fs::path& p) const {
        if (cfg_.notion_mirror_dir.empty()) return false;
        const fs::path mirror = fs::absolute(cfg_.vault_path / cfg_.notion_mirror_dir).lexically_normal();
        const std::string prefix = mirror.string() + "/";
        return starts_with(p.lexically_normal().string(), prefix);
    }

    void push_to_notion(const std::string& title, const std::string& body, const fs::path& source,
                        const std::string& fingerprint) {
        ++stats_.pushes_attempted;
        const ApiResult res = notion_.create_page(title, body);
        if (res.ok && res.http_2xx()) {
            ++stats_.pushes_succeeded;
            state_.set_pushed_hash(path_key(source), fingerprint);
            state_.save();
            Logger::success(fmt("pushed to Notion: %s", title));
            return;
        }

        ++stats_.pushes_failed;
        ++stats_.errors;
        if (res.status < 0) {
            Logger::error(fmt("Notion request failed for '%s' (%s): %s - not retried automatically, "
                              "the next edit of this file will push again",
                              title, transport_error_name(-res.status), res.error));
        } else if (res.status == 0) {
            Logger::error(fmt("Notion request failed for '%s': %s", title, res.error));
        } else {
            Logger::error(fmt("Notion rejected '%s' with HTTP %d %s: %s", title, res.status,
                              res.error_code, res.error));
            Logger::warn("the file is marked as handled; it is pushed again once its content "
                         "changes");
            if (res.status == 401) {
                Logger::error("check NOTION_API_KEY (integration token) and that the integration is "
                              "connected to the database");
            } else if (res.status == 404) {
                Logger::error("check NOTION_DATABASE_ID and that the database is shared with the "
                              "integration");
            } else if (res.status == 400 && contains_ci(res.error, "property")) {
                Logger::error("the database title property could not be matched; set the database "
                              "up with a single title property or adjust NOTION_VERSION");
            }
            // A rejected request is recorded so a retry of the same content is
            // not attempted again on every event.
            if (res.status >= 400 && res.status < 500 && res.status != 429 && res.status != 408) {
                state_.set_pushed_hash(path_key(source), fingerprint);
                state_.save();
            }
        }
    }

    static const char* transport_error_name(int code) {
        switch (static_cast<httplib::Error>(code)) {
            case httplib::Error::Connection:        return "connection failed";
            case httplib::Error::ConnectionTimeout: return "connection timeout";
            case httplib::Error::Timeout:           return "timeout";
            case httplib::Error::Read:              return "read error";
            case httplib::Error::Write:             return "write error";
            case httplib::Error::SSLConnection:     return "TLS handshake failed";
            case httplib::Error::SSLServerVerification: return "certificate verification failed";
            case httplib::Error::Canceled:          return "cancelled";
            default:                                return "transport error";
        }
    }

    // ---------------------------------------------------------------- notion
    void poller_loop(std::stop_token st) {
        Logger::debug("notion poller started");
        if (cli_.once) return;  // run() already did the single pass

        poll_notion_once(st);

        while (!st.stop_requested()) {
            interruptible_sleep(cfg_.notion_poll_interval, st);
            if (st.stop_requested()) break;
            poll_notion_once(st);
        }
        Logger::debug("notion poller stopped");
    }

    // Incremental pull: Notion -> markdown -> vault file, done in the poller
    // thread. Rendering inline is what makes the cursor below trustworthy.
    //
    // The cursor is committed only when a whole walk succeeded. Pages that were
    // mirrored are recognised by their last_edited_time on the next walk, so
    // repeating a walk costs one query, not one API call per page.
    void poll_notion_once(std::stop_token st) {
        ++stats_.notion_polls;
        Logger::debug("polling Notion for changes");

        std::string after;
        const std::string cursor = state_.notion_cursor();
        if (!cursor.empty()) {
            if (auto parsed = parse_iso8601(cursor); parsed.has_value()) {
                // A one second overlap guards against pages edited in the same
                // millisecond as the cursor.
                after = iso8601_utc(*parsed - cfg_.notion_cursor_overlap);
            } else {
                Logger::warn(fmt("state cursor '%s' is not a timestamp; re-reading the whole "
                                  "database", cursor));
            }
        }

        std::optional<std::chrono::system_clock::time_point> newest;
        std::string walk_cursor;
        int batches = 0;
        size_t mirrored = 0;
        bool walk_complete = false;

        while (!st.stop_requested()) {
            json body = {
                {"page_size", 100},
                {"sorts", json::array({json{{"timestamp", "last_edited_time"},
                                            {"direction", "descending"}}})},
            };
            if (!after.empty()) {
                body["filter"] = {{"timestamp", "last_edited_time"},
                                  {"last_edited_time", {{"after", after}}}};
            }
            if (!walk_cursor.empty()) body["start_cursor"] = walk_cursor;

            const ApiResult res = notion_.query_database(body);
            if (!res.ok || !res.http_2xx()) {
                ++stats_.notion_poll_failures;
                ++stats_.errors;
                if (res.status == 404) {
                    Logger::error("Notion query failed with HTTP 404: check NOTION_DATABASE_ID and "
                                  "that the integration has access to the database");
                } else if (res.status == 400 && contains_ci(res.error, "data_source")) {
                    Logger::error(fmt("Notion rejected the query (%s). This integration may use a "
                                      "newer API version; try NOTION_VERSION=2025-09-03 and see "
                                      "the README for the data-source change.", res.error));
                } else if (res.status > 0) {
                    Logger::warn(fmt("Notion query failed: HTTP %d %s %s", res.status, res.error_code,
                                     res.error));
                } else {
                    Logger::warn(fmt("Notion query failed: %s", res.error));
                }
                return;  // the cursor stays where it was
            }

            const json payload = json::parse(res.body, nullptr, false);
            if (payload.is_discarded() || !payload.contains("results") ||
                !payload["results"].is_array()) {
                ++stats_.notion_poll_failures;
                Logger::warn("Notion returned an unparseable query response");
                return;
            }

            for (const auto& page : payload["results"]) {
                if (st.stop_requested()) return;
                ++stats_.notion_pages_scanned;
                if (json_string(page, "object") != "page") continue;

                if (auto edited = parse_iso8601(json_string(page, "last_edited_time"));
                    edited.has_value() && (!newest.has_value() || *edited > *newest)) {
                    newest = *edited;
                }

                if (mirror_page_if_changed(page)) {
                    ++mirrored;
                } else {
                    // Nothing may be committed past a page that could not be read:
                    // the next poll repeats this walk and skips what is already
                    // written.
                    state_.save();
                    return;
                }
            }

            ++batches;
            walk_cursor = json_string(payload, "next_cursor");
            if (!json_bool(payload, "has_more") || walk_cursor.empty()) {
                walk_complete = true;
                break;
            }
            if (batches > 500) {
                Logger::warn("stopping the Notion walk after 500 batches; the next poll continues");
                return;
            }
        }

        if (walk_complete && newest.has_value()) {
            state_.set_notion_cursor(iso8601_utc(*newest));
        }
        state_.save();
        if (mirrored > 0) {
            Logger::info(fmt("Notion: %zu page(s) mirrored into %s/", mirrored,
                             cfg_.notion_mirror_dir));
        }
    }

    // Mirrors one page when its last_edited_time differs from the recorded
    // version. Returns false when the page could not be read, which makes the
    // caller leave the cursor untouched so the page is retried next poll.
    bool mirror_page_if_changed(const json& page) {
        const std::string page_id = json_string(page, "id");
        const std::string last_edited = json_string(page, "last_edited_time");
        if (page_id.empty()) return true;

        if (auto known = state_.page_version(page_id); known.has_value() && *known == last_edited) {
            ++stats_.files_unchanged;
            return true;
        }

        if (json_bool(page, "archived") || json_bool(page, "in_trash")) {
            Logger::debug(fmt("page %s is archived in Notion; leaving the vault untouched", page_id));
            state_.set_page_version(page_id, last_edited);
            return true;
        }

        const ApiResult blocks = notion_.fetch_block_children(page_id);
        if (!blocks.ok || !blocks.http_2xx()) {
            if (++page_failures_[page_id] < kMaxPageFailures) {
                ++stats_.errors;
                Logger::warn(fmt("cannot fetch blocks for page %s (attempt %d/%d): %s", page_id,
                                 page_failures_[page_id], kMaxPageFailures, blocks.error));
                return false;
            }
            Logger::error(fmt("giving up on page %s after %d attempts: %s", page_id, kMaxPageFailures,
                              blocks.error));
            state_.set_page_version(page_id, last_edited);
            page_failures_.erase(page_id);
            return true;
        }

        const json children = json::parse(blocks.body, nullptr, false);
        if (children.is_discarded() || !children.contains("results") ||
            !children["results"].is_array()) {
            Logger::warn(fmt("unparseable block list for page %s", page_id));
            return false;
        }

        json full = page;
        full["blocks"] = json::array();
        for (const auto& block : children["results"]) {
            full["blocks"].push_back(with_nested_children(block, 0));
        }

        const std::string title = NotionRenderer::page_title(full, notion_.title_property());
        const std::string markdown = NotionRenderer::render_page(full);

        std::string frontmatter;
        frontmatter += fmt("notion_id: %s\n", page_id);
        frontmatter += fmt("notion_url: %s\n", json_string(page, "url"));
        frontmatter += fmt("last_edited: %s\n", last_edited);
        if (page.contains("created_time")) {
            frontmatter += fmt("created: %s\n", json_string(page, "created_time"));
        }

        const std::string relative = fmt("%s/%s.md", cfg_.notion_mirror_dir, page_id);
        const WriteOutcome write = writer_.write_markdown(relative, frontmatter, markdown, title);
        if (!write.ok) {
            ++stats_.errors;
            Logger::error(fmt("cannot mirror Notion page '%s' (%s): %s", title, page_id, write.error));
            return false;
        }

        if (write.unchanged) {
            ++stats_.files_unchanged;
        } else {
            ++stats_.files_written;
            ++stats_.notion_pages_rendered;
            Logger::success(fmt("mirrored Notion page: %s -> %s", title, write.path.filename().string()));
        }
        state_.set_page_version(page_id, last_edited);
        page_failures_.erase(page_id);
        state_.save();
        return true;
    }

    // Blocks with children need a second request; nesting is capped so a hostile
    // or deeply nested page cannot explode the number of API calls. Sub-pages and
    // child databases are rendered as links: their content is not a block of this
    // page and recursing into them could pull in an entire workspace.
    json with_nested_children(const json& block, int depth) {
        json copy = block;
        if (depth >= 3 || !json_bool(copy, "has_children")) return copy;
        const std::string type = json_string(copy, "type");
        if (type == "child_page" || type == "child_database") return copy;
        const std::string id = json_string(copy, "id");
        if (id.empty()) return copy;
        const ApiResult res = notion_.fetch_block_children(id);
        if (!res.ok || !res.http_2xx()) return copy;
        const json children = json::parse(res.body, nullptr, false);
        if (children.is_discarded() || !children.contains("results")) return copy;
        copy["children"] = json::array();
        for (const auto& child : children["results"]) {
            copy["children"].push_back(with_nested_children(child, depth + 1));
        }
        return copy;
    }

    // ------------------------------------------------------------------ http
    void wire_http_routes() {
        server_.set_exception_handler([](const httplib::Request&, httplib::Response& res,
                                         std::exception_ptr ep) {
            std::string what = "unknown error";
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
            Logger::error(fmt("request handler threw: %s", what));
            res.status = 500;
            res.set_content(json{{"ok", false}, {"error", "internal error"}}.dump(), "application/json");
        });
        server_.set_error_handler([](const httplib::Request&, httplib::Response& res) {
            if (res.status >= 400) {
                res.set_content(json{{"ok", false}, {"status", res.status}}.dump(), "application/json");
            }
        });
        server_.set_read_timeout(30s);
        server_.set_write_timeout(30s);

        server_.Get("/healthz", [this](const httplib::Request&, httplib::Response& res) {
            json body{
                {"ok", true},
                {"version", kVersion},
                {"uptime_seconds", static_cast<int64_t>(
                                       std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() - started_).count())},
                {"notion_enabled", notion_.enabled()},
                {"notion_cursor", state_.notion_cursor()},
                {"cached_pages", state_.cached_pages()},
                {"queue_depth", queue_.size()},
                {"watched_directories", watcher_.watch_count()},
                {"suppressed_paths", suppression_.size()},
                {"stats", stats_.snapshot()},
            };
            res.status = 200;
            res.set_content(body.dump(), "application/json");
        });

        server_.Get("/", [](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_content(fmt("Telegrobsidian %s\n\nPOST %s\nGET /healthz\n",
                                std::string(kVersion), std::string("/telegram-webhook")),
                            "text/plain; charset=utf-8");
        });

        server_.Post("/telegram-webhook", [this](const httplib::Request& req, httplib::Response& res) {
            ++stats_.webhooks_received;
            if (!cfg_.telegram_webhook_secret.empty()) {
                const std::string token =
                    req.get_header_value("X-Telegram-Bot-Api-Secret-Token", "");
                if (token != cfg_.telegram_webhook_secret) {
                    ++stats_.webhooks_rejected;
                    Logger::warn("rejected a webhook call with a missing or wrong secret token");
                    res.status = 403;
                    res.set_content(json{{"ok", false}, {"error", "forbidden"}}.dump(),
                                    "application/json");
                    return;
                }
            }
            if (req.body.empty()) {
                ++stats_.webhooks_rejected;
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "empty body"}}.dump(), "application/json");
                return;
            }
            const json parsed = json::parse(req.body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object()) {
                ++stats_.webhooks_rejected;
                res.status = 400;
                res.set_content(json{{"ok", false}, {"error", "body must be a Telegram Update object"}}.dump(),
                                "application/json");
                return;
            }
            const TelegramMessage msg = parse_telegram_update(parsed);
            if (!msg.valid || trim(msg.text).empty()) {
                // Acknowledge with 200: Telegram retries non-2xx replies, and
                // there is nothing to retry for a sticker or a service message.
                res.status = 200;
                res.set_content(json{{"ok", true}, {"queued", false}, {"reason", "no text content"}}.dump(),
                                "application/json");
                return;
            }
            SyncTask task;
            task.kind = TaskKind::TelegramWebhook;
            task.payload = req.body;
            queue_.push(std::move(task));
            res.status = 200;
            res.set_content(json{{"ok", true}, {"queued", true}, {"queue_depth", queue_.size()}}.dump(),
                            "application/json");
        });
    }

    // Registers the webhook with Telegram when TELEGRAM_PUBLIC_URL is provided.
    void register_telegram_webhook() {
        if (cfg_.telegram_bot_token.empty() || cfg_.telegram_public_url.empty()) {
            if (!cfg_.telegram_bot_token.empty()) {
                Logger::info("set TELEGRAM_PUBLIC_URL to let the daemon register its webhook "
                             "automatically (see scripts/register-telegram-webhook.sh)");
            }
            return;
        }
        if (!api_base_reachable(cfg_.telegram_api_base, "registering the Telegram webhook")) return;

        const std::string url = cfg_.telegram_public_url + "/telegram-webhook";
        webhook_registrar_ = std::jthread([this, url] {
            try {
                httplib::Client client(cfg_.telegram_api_base);
                client.set_connection_timeout(cfg_.http_connect_timeout);
                client.set_read_timeout(cfg_.http_read_timeout);
#if TELEGROBSIDIAN_HAS_SSL
                client.enable_server_certificate_verification(true);
#endif

                httplib::Params params{{"url", url}};
                if (!cfg_.telegram_webhook_secret.empty()) {
                    params.emplace("secret_token", cfg_.telegram_webhook_secret);
                }
                const auto res =
                    client.Post(("/bot" + cfg_.telegram_bot_token + "/setWebhook").c_str(), params);
                if (res && res->status == 200) {
                    Logger::success(fmt("Telegram webhook registered: %s", url));
                } else {
                    Logger::warn(fmt("setWebhook failed (HTTP %d): %s", res ? res->status : 0,
                                     res ? NotionClient::truncate_utf8(res->body, 300)
                                         : httplib::to_string(res.error())));
                }
            } catch (const std::exception& e) {
                Logger::warn(fmt("setWebhook failed: %s", std::string(e.what())));
            }
        });
    }

    // ------------------------------------------------------------- self test
    int self_test() {
        bool ok = true;
        Logger::info("self-test: validating configuration and credentials");

        std::string formatter_report;
        const bool formatter_ok = check_formatter(formatter_report);
        ok = ok && formatter_ok;
        Logger::log(formatter_ok ? LogLevel::Success : LogLevel::Error,
                    (formatter_ok ? "internal: " : "internal: ") + formatter_report);

        if (notion_.enabled()) {
            const ApiResult res = notion_.database_info();
            if (res.ok && res.http_2xx()) {
                const json j = json::parse(res.body, nullptr, false);
                if (j.is_discarded()) {
                    ok = false;
                    Logger::error("Notion returned an unparseable database description");
                } else {
                    std::string db_title;
                    if (j.contains("title")) db_title = render_rich_text(j["title"]);
                    std::string title_property = "Name";
                    if (j.contains("properties")) {
                        for (auto it = j["properties"].begin(); it != j["properties"].end(); ++it) {
                            if (json_string(it.value(), "type") == "title") {
                                title_property = it.key();
                                break;
                            }
                        }
                    }
                    notion_.set_title_property(title_property);
                    Logger::success(fmt("Notion reachable: database '%s' (title property: '%s')",
                                        db_title.empty() ? std::string("?") : db_title,
                                        title_property));
                    Logger::info(fmt("integration token accepted; %zu page(s) cached locally",
                                     state_.cached_pages()));
                }
            } else {
                ok = false;
                Logger::error(fmt("Notion check failed (HTTP %d, transport %d): %s", res.status,
                                  res.status, res.error));
                if (res.status == 404) {
                    Logger::error("the database id is wrong, or the integration was never granted "
                                  "access to it (Notion: ... -> Connections -> add your integration)");
                } else if (res.status == 401) {
                    Logger::error("NOTION_API_KEY was rejected; create an internal integration token "
                                  "at https://www.notion.so/my-integrations");
                }
            }
        } else {
            Logger::warn("Notion sync is not configured/enabled; skipping the Notion check");
        }

        if (!cfg_.telegram_bot_token.empty()) {
            const bool reachable = api_base_reachable(cfg_.telegram_api_base, "the Telegram self-test");
            try {
                if (!reachable) {
                    ok = false;
                } else {
                httplib::Client client(cfg_.telegram_api_base);
                client.set_connection_timeout(cfg_.http_connect_timeout);
#if TELEGROBSIDIAN_HAS_SSL
                client.enable_server_certificate_verification(true);
#endif
                const auto res = client.Get(("/bot" + cfg_.telegram_bot_token + "/getMe").c_str());
                if (res && res->status == 200) {
                    const json me = json::parse(res->body, nullptr, false);
                    std::string username;
                    if (!me.is_discarded() && me.contains("result")) {
                        username = json_string(me["result"], "username");
                    }
                    Logger::success(fmt("Telegram bot token accepted (@%s)",
                                        username.empty() ? std::string("unknown") : username));
                } else {
                    ok = false;
                    Logger::error(fmt("Telegram getMe failed (HTTP %d); check TELEGRAM_BOT_TOKEN",
                                      res ? res->status : 0));
                }
                }
            } catch (const std::exception& e) {
                ok = false;
                Logger::error(fmt("Telegram getMe failed: %s", std::string(e.what())));
            }
        }

        // The webhook path is validated offline: binding happens in a normal run.
        Logger::info(fmt("webhook endpoint would be: POST %s:%d/telegram-webhook", cfg_.webhook_bind,
                         cfg_.webhook_port));
        Logger::info(fmt("vault %s is %s", cfg_.vault_path.string(),
                         fs::exists(cfg_.vault_path) ? "present" : "missing (will be created)"));

        Logger::log(ok ? LogLevel::Success : LogLevel::Error,
                    ok ? "self-test passed" : "self-test failed");
        return ok ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    // ---------------------------------------------------------------- members
    Config cfg_;
    CliOptions cli_;
    Stats stats_;
    TaskQueue queue_;
    FileEventTracker tracker_;
    SuppressionRegistry suppression_;
    VaultWriter writer_;
    VaultWatcher watcher_;
    StateStore state_;
    NotionClient notion_;
    httplib::Server server_;
    std::jthread webhook_registrar_;
    std::atomic<bool> fatal_error_{false};
    // Pages the Notion API refused to serve; stops retrying them forever.
    std::unordered_map<std::string, int> page_failures_;
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
};

}  // namespace tgo

// =============================================================================
//  Entry point
// =============================================================================
int main(int argc, char** argv) {
    using namespace tgo;

    CliOptions cli = parse_cli(argc, argv);
    if (cli.show_help) {
        print_help(argv[0]);
        return EXIT_SUCCESS;
    }
    if (cli.show_version) {
        std::cout << "telegrobsidian " << kVersion << " (tls: "
                  << (TELEGROBSIDIAN_HAS_SSL ? "openssl" : "disabled") << ")\n";
        return EXIT_SUCCESS;
    }

    for (const auto& warning : cli.warnings) Logger::warn(warning);
    for (const auto& file : cli.env_files) load_env_file(file, cli.warnings);

    Config cfg = Config::load();
    cfg.once = cli.once;
    cfg.self_test = cli.self_test;

    const bool is_tty = ::isatty(fileno(stdout)) != 0;
    Logger::configure(cfg.log_level, cfg.log_timestamps, cfg.log_json, is_tty);

    for (const auto& warning : cli.warnings) Logger::warn(warning);
    for (const auto& problem : cfg.validate()) Logger::warn(std::string("configuration: ") + problem);

    try {
        Daemon daemon(std::move(cfg), std::move(cli));
        return daemon.run();
    } catch (const std::exception& e) {
        Logger::error(fmt("fatal: %s", std::string(e.what())));
        return EXIT_FAILURE;
    }
}

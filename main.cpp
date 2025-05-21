#include <cxxabi.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>

// Maximum call depth
using i64 = int64_t;
static const int MAX_DEPTH = 256;

// Stacks for names, timestamps, and skip flags
static thread_local const char* name_stack[MAX_DEPTH];
static thread_local i64 time_stack[MAX_DEPTH];
static thread_local bool skip_stack[MAX_DEPTH] = {};
static thread_local int depth = 0;

// Demangle cache
typedef std::unordered_map<std::string, std::string> DemangleCache;
__attribute__((no_instrument_function)) static inline const char* demangle_cached(const char* m) {
    static thread_local DemangleCache cache;
    auto it = cache.find(m);
    if (it != cache.end()) return it->second.c_str();
    int status = 0;
    char* dem = abi::__cxa_demangle(m, nullptr, nullptr, &status);
    std::string out = (status == 0 && dem) ? dem : m;
    free(dem);
    auto res = cache.emplace(m, std::move(out));
    return res.first->second.c_str();
}

// Helpers not to be instrumented
__attribute__((no_instrument_function)) static inline i64 now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (i64)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

__attribute__((no_instrument_function)) static inline void print_line(const char* prefix, const char* name, i64 dur_ns) {
    char buf[512];

    const char* suffix = "ns";
    double dur = (double)dur_ns;
    if (dur_ns >= 1000000000LL) {
        dur /= 1000000000.0;
        suffix = "s";
    } else if (dur_ns >= 1000000LL) {
        dur /= 1000000.0;
        suffix = "ms";
    } else if (dur_ns >= 1000LL) {
        dur /= 1000.0;
        suffix = "us";
    }
    // print depth
    int p = 0;
    for (int i = 0; i < depth; ++i) {
        buf[p++] = ' ';
        buf[p++] = ' ';
    }
    // print prefix
    int n = snprintf(buf + p, sizeof(buf) - p, "%s %s: %.3f %s\n", prefix, name, dur, suffix);
    write(STDERR_FILENO, buf, p + n);
}

__attribute__((no_instrument_function)) static inline bool should_trace_symbol(const char* m) {
    if (!m) return false;
    // Fast reject std:: namespace via mangled prefixes
    static const char* mangled_std_m[] = {
        "_ZNSt",    // libstdc++ std::
        "_ZN3std",  // libc++ std::
        "_ZSt",     // std helper
        "_ZNKSt",   // libstdc++ const member
        "_ZNK3std"  // libc++ const member
    };
    size_t mlen = strlen(m);
    for (auto prefix : mangled_std_m) {
        size_t plen = strlen(prefix);
        if (mlen >= plen && strncmp(m, prefix, plen) == 0) {
            return false;
        }
    }
    // Demangle and blacklist prefixes
    const char* name = demangle_cached(m);
    static const char* blacklist[] = {"_GLOBAL__sub", "__gnu", "__cxx", "dbg_internal", "operator<", nullptr};
    for (auto p = blacklist; *p; ++p) {
        if (strncmp(name, *p, strlen(*p)) == 0) return false;
    }
    return true;
}

extern "C" {

__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* f, void*) {
    // Determine if this frame should be skipped
    bool skip_here = (depth > 0 && skip_stack[depth - 1]);
    Dl_info d;
    if (!skip_here) {
        if (!dladdr(f, &d) || !should_trace_symbol(d.dli_sname)) {
            skip_here = true;
        }
    }
    skip_stack[depth] = skip_here;
    if (skip_here) {
        depth++;
        return;
    }
    // Log entry (no duration)
    const char* m = d.dli_sname;
    name_stack[depth] = m;
    time_stack[depth] = now_ns();
    // indentation
    char buf[512];
    int p = 0;
    for (int i = 0; i < depth; ++i) {
        buf[p++] = ' ';
        buf[p++] = ' ';
    }
    buf[p++] = '>';
    buf[p++] = '>';
    buf[p++] = ' ';
    const char* name = demangle_cached(m);
    int n = snprintf(buf + p, sizeof(buf) - p, "%s\n", name);
    write(STDERR_FILENO, buf, p + n);
    depth++;
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* f, void*) {
    if (depth <= 0) return;
    bool skip_here = skip_stack[depth - 1];
    depth--;
    if (skip_here) {
        skip_stack[depth] = false;
        return;
    }
    // Log exit with duration
    const char* m = name_stack[depth];
    i64 dur = now_ns() - time_stack[depth];
    print_line("<<", demangle_cached(m), dur);
}

}  // extern "C"

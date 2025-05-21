
# A Minimal Function-Level Profiler for Local Codeforces Testing/benchmarking

This note introduces a small, self-contained C++ profiler that prints a timed call tree for every function in your solution.
It relies only on the standard tool-chain (GCC / Clang) and POSIX.

---

## Principle of operation

* **`-finstrument-functions`** inserts hooks on every non-inlined function.
* The two hooks (`__cyg_profile_func_enter/exit`) in *profiler.cpp*

  * reject library frames (mangled `std::` symbols, selected helpers),
  * demangle names once and cache them,
  * time calls with `clock_gettime(CLOCK_MONOTONIC)`,
  * write an indented trace to **stderr**.

---

## Basic usage

```bash
g++ -std=c++23 -O2 -g -Wall \
    -finstrument-functions \
    -finstrument-functions-exclude-file-list=/bits/stl,debug.h \
    solution.cpp profiler.cpp -o prof           # add -ldl on older glibc
./prof < input.txt > output.txt                 # profile appears on stderr
```

Typical output

```
>> main
  >> solve()
    >> sieve()
    << sieve(): 982.968 ms
    >> linear_sieve()
    << linear_sieve(): 452.570 ms
  << solve(): 1.436 s
<< main: 4.203 s
```

`>>` marks entry, `<<` exit; indentation is call depth. Units auto-scale from ns to s.

---

## Trade-offs

| Strengths                                     | Limitations                                                      |
| --------------------------------------------- | ---------------------------------------------------------------- |
| Zero code changes inside the solution         | Hook overhead ≈ 50 – 100 ns per call                             |
| Clear visual call hierarchy                   | Slightly inhibits inlining; numbers differ from final submission |
| Filters out standard library noise by default | POSIX only (`dladdr`); not portable to MSVC                      |
| No external tools required                    | Never submit with instrumentation flags: size & timing differ    |

---

## Source

Place the following file next to your solution and include it in the build:

<details><summary><code>profiler.cpp</code> (≈ 170 lines)</summary>

```cpp
// profiler.cpp – single translation unit
#include <cxxabi.h>
#include <dlfcn.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unordered_map>

using i64 = long long;
static const int MAX_DEPTH = 256;

static thread_local const char* name_stack[MAX_DEPTH];
static thread_local i64  time_stack[MAX_DEPTH];
static thread_local bool skip_stack[MAX_DEPTH]{};
static thread_local int  depth = 0;

// ── helpers not to be instrumented ────────────────────────────────────────────
__attribute__((no_instrument_function))
static i64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return i64(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
}

__attribute__((no_instrument_function))
static const char* demangle_cached(const char* m) {
    using cache_t = std::unordered_map<std::string, std::string>;
    static thread_local cache_t cache;
    if (!m) return "?";
    auto it = cache.find(m);
    if (it != cache.end()) return it->second.c_str();

    int status = 0;
    char* dem = abi::__cxa_demangle(m, nullptr, nullptr, &status);
    std::string out = (status == 0 && dem) ? dem : m;
    free(dem);
    return cache.emplace(m, std::move(out)).first->second.c_str();
}

__attribute__((no_instrument_function))
static void print_line(const char* prefix, const char* name, i64 ns) {
    char buf[512]; const char* unit = "ns"; double t = double(ns);
    if (ns >= 1'000'000'000LL) { t /= 1e9; unit = "s";  }
    else if (ns >= 1'000'000LL) { t /= 1e6; unit = "ms"; }
    else if (ns >= 1'000LL)     { t /= 1e3; unit = "μs"; }

    int p = 0;
    for (int i = 0; i < depth; ++i) { buf[p++] = ' '; buf[p++] = ' '; }
    int n = snprintf(buf + p, sizeof(buf) - p,
                     "%s %s: %.3f %s\n", prefix, name, t, unit);
    write(STDERR_FILENO, buf, p + n);
}

__attribute__((no_instrument_function))
static bool trace_symbol(const char* m) {
    if (!m) return false;
    static const char* std_pref[] = {
        "_ZNSt","_ZN3std","_ZSt","_ZNKSt","_ZNK3std"
    };
    for (auto pre : std_pref)
        if (!strncmp(m, pre, strlen(pre))) return false;

    const char* d = demangle_cached(m);
    static const char* deny[] = {
        "_GLOBAL__sub","__gnu","__cxx","operator<", nullptr
    };
    for (auto p = deny; *p; ++p)
        if (!strncmp(d, *p, strlen(*p))) return false;
    return true;
}

// ── hooks ─────────────────────────────────────────────────────────────────────
extern "C" {

__attribute__((no_instrument_function))
void __cyg_profile_func_enter(void* f, void*) {
    bool skip = (depth && skip_stack[depth - 1]);
    Dl_info d;
    if (!skip && (!dladdr(f, &d) || !trace_symbol(d.dli_sname)))
        skip = true;

    skip_stack[depth] = skip;
    if (skip) { ++depth; return; }

    name_stack[depth] = d.dli_sname;
    time_stack[depth] = now_ns();

    char buf[512]; int p = 0;
    for (int i = 0; i < depth; ++i) { buf[p++] = ' '; buf[p++] = ' '; }
    buf[p++] = '>'; buf[p++] = '>'; buf[p++] = ' ';
    int n = snprintf(buf + p, sizeof(buf) - p, "%s\n",
                     demangle_cached(d.dli_sname));
    write(STDERR_FILENO, buf, p + n);
    ++depth;
}

__attribute__((no_instrument_function))
void __cyg_profile_func_exit(void*, void*) {
    if (!depth) return;
    bool skip = skip_stack[--depth];
    if (skip) { skip_stack[depth] = false; return; }

    print_line("<<", demangle_cached(name_stack[depth]),
               now_ns() - time_stack[depth]);
}

} // extern "C"
```

</details>

---

### Final remarks

Use this profiler only on your machine to locate bottlenecks before submitting the optimised, clean binary. For finer micro-benchmarking or production profiling, consider `perf` or `callgrind`.

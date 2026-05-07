#pragma once

#include <cstdlib>
#include <thread>
#include <vector>

namespace slothdb {

// Returns the number of hardware threads available, clamped to 1 on
// single-threaded WASM (Emscripten without pthreads). Std::thread's
// constructor throws "thread constructor failed: Not supported" in
// that environment, so callers must check the return value before
// spawning. With this helper, callers can uniformly do:
//
//   unsigned int nt = slothdb::HWThreads();
//   if (nt > 1) { ...spawn... } else { ...inline... }
inline unsigned int HWThreads() {
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    return 1;
#else
    unsigned int n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif
    if (const char *cap = std::getenv("SLOTHDB_MAX_THREADS")) {
        unsigned int c = (unsigned int)std::atoi(cap);
        if (c > 0 && c < n) n = c;
    }
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return n;
#endif
}

// Run `fn(t)` for `t` in `[0, nt)`. When `nt > 1` and threading is
// available, spawn `nt - 1` worker threads and run `fn(0)` on the
// calling thread; otherwise run `fn(0)` inline. Joins all workers
// before returning. Mirrors the spawn-and-join pattern that physical
// operators were repeating, with an inline fallback that's safe on
// single-threaded WASM.
template <class Fn>
inline void ParallelFor(unsigned int nt, Fn &&fn) {
    if (nt <= 1) {
        fn(0u);
        return;
    }
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
    for (unsigned int t = 0; t < nt; t++) fn(t);
#else
    std::vector<std::thread> ts;
    ts.reserve(nt - 1);
    for (unsigned int t = 1; t < nt; t++) ts.emplace_back(fn, t);
    fn(0u);
    for (auto &t : ts) if (t.joinable()) t.join();
#endif
}

} // namespace slothdb

#pragma once

// Last line of defence for the capture relay: runs `f` and turns a C++
// exception - and, with MSVC, a structured exception such as an access
// violation inside a driver - into an error string instead of terminating
// the whole app. The relay then disables itself and reports the error.

#include <cstdio>
#include <exception>
#include <string>
#include <type_traits>
#include <utility>

#if defined(_WIN32) && defined(_MSC_VER)
#include <windows.h>
#endif

namespace usc::capture {

namespace detail {
using RawFn = void (*)(void*);

#if defined(_WIN32) && defined(_MSC_VER)
// No C++ objects in this frame: required for __try/__except.
inline bool runSeh(RawFn fn, void* ctx, unsigned long& code) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        code = GetExceptionCode();
        return false;
    }
}
#else
inline bool runSeh(RawFn fn, void* ctx, unsigned long&) {
    fn(ctx);
    return true;
}
#endif
} // namespace detail

// Returns true if `f` completed; otherwise `error` describes what happened.
template <class F>
bool guarded(F&& f, std::string& error) {
    struct Ctx {
        std::remove_reference_t<F>* f;
        std::string* err;
    } ctx{&f, &error};
    unsigned long code = 0;
    error.clear();
    bool ok = detail::runSeh(
        [](void* p) {
            auto* c = static_cast<Ctx*>(p);
            try {
                (*c->f)();
            } catch (const std::exception& e) {
                *c->err = std::string("internal error: ") + e.what();
            } catch (...) {
                *c->err = "internal error (unknown exception)";
            }
        },
        &ctx, code);
    if (!ok) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "crash prevented (exception 0x%08lX)", code);
        error = buf;
    }
    return error.empty();
}

} // namespace usc::capture

#pragma once
#ifdef _WIN32

// Minimal owning COM pointer (no ATL/WRL dependency, builds with MSVC and MinGW).
#include <unknwn.h>

namespace usc::capture {

template <class T>
class ComRef {
public:
    ComRef() = default;
    explicit ComRef(T* p) : p_(p) {}
    ComRef(const ComRef& o) : p_(o.p_) { if (p_) p_->AddRef(); }
    ComRef(ComRef&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~ComRef() { reset(); }
    ComRef& operator=(ComRef o) noexcept {
        T* t = p_;
        p_ = o.p_;
        o.p_ = t;
        return *this;
    }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T** put() {
        reset();
        return &p_;
    }
    void** putVoid() { return reinterpret_cast<void**>(put()); }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

} // namespace usc::capture

#endif

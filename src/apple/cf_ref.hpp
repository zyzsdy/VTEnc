#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <stdexcept>
#include <utility>

namespace vtenc {

// Adopts a +1 Create/Copy result. Borrowed references must not be passed here.
template <typename T>
class CFRef {
public:
    explicit CFRef(T value) : value_(value) {
        if (!value_) throw std::runtime_error("Apple API returned a null Create/Copy result");
    }
    ~CFRef() { if (value_) CFRelease(value_); }
    CFRef(const CFRef&) = delete;
    CFRef& operator=(const CFRef&) = delete;
    CFRef(CFRef&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    CFRef& operator=(CFRef&&) = delete;
    [[nodiscard]] T get() const noexcept { return value_; }

private:
    T value_;
};

} // namespace vtenc

#pragma once
#include <cstddef>
#include <utility>

namespace regml {
template <class T> class WiiUInstancePtr {
  public:
    WiiUInstancePtr() = default;
    explicit WiiUInstancePtr(T *instance) : instance_(instance) { retain(); }
    WiiUInstancePtr(const WiiUInstancePtr &other) : instance_(other.instance_) { retain(); }
    WiiUInstancePtr(WiiUInstancePtr &&other) noexcept : instance_(std::exchange(other.instance_, nullptr)) {}
    WiiUInstancePtr &operator=(WiiUInstancePtr other) noexcept {
        std::swap(instance_, other.instance_);
        return *this;
    }
    ~WiiUInstancePtr() { release(); }
    T *get() const noexcept { return instance_; }
    T *operator->() const noexcept { return instance_; }
    T &operator*() const noexcept { return *instance_; }
    explicit operator bool() const noexcept { return instance_ != nullptr; }
    friend bool operator==(const WiiUInstancePtr &left, const WiiUInstancePtr &right) noexcept {
        return left.instance_ == right.instance_;
    }

  private:
    void retain() noexcept {
        if (instance_)
            ++instance_->references;
    }
    void release() noexcept {
        if (instance_ && --instance_->references == 0)
            delete instance_;
        instance_ = nullptr;
    }
    T *instance_ = nullptr;
};
} // namespace regml

#pragma once
#include <cstddef>
#include <utility>

namespace regml {
template <class T> class IntrusivePtr {
  public:
    IntrusivePtr() = default;
    explicit IntrusivePtr(T *instance) noexcept : instance_(instance) { retain(); }
    IntrusivePtr(const IntrusivePtr &other) noexcept : instance_(other.instance_) { retain(); }
    IntrusivePtr(IntrusivePtr &&other) noexcept : instance_(std::exchange(other.instance_, nullptr)) {}
    IntrusivePtr &operator=(IntrusivePtr other) noexcept {
        std::swap(instance_, other.instance_);
        return *this;
    }
    ~IntrusivePtr() { release(); }
    void reset(T *instance = nullptr) noexcept {
        IntrusivePtr replacement(instance);
        std::swap(instance_, replacement.instance_);
    }
    T *get() const noexcept { return instance_; }
    T *operator->() const noexcept { return instance_; }
    T &operator*() const noexcept { return *instance_; }
    explicit operator bool() const noexcept { return instance_ != nullptr; }
    friend bool operator==(const IntrusivePtr &left, const IntrusivePtr &right) noexcept {
        return left.instance_ == right.instance_;
    }
    friend bool operator==(const IntrusivePtr &left, std::nullptr_t) noexcept { return !left.instance_; }
    friend bool operator==(std::nullptr_t, const IntrusivePtr &right) noexcept { return !right.instance_; }

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

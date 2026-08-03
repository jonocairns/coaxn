#pragma once

#include <utility>

namespace coax::win {

// Minimal owning COM pointer. Deliberately small and dependency-free rather
// than pulling in WRL or ATL, which vary in availability across toolchains.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr& other) : ptr_(other.ptr_) {
        if (ptr_) ptr_->AddRef();
    }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            if (other.ptr_) other.ptr_->AddRef();
            reset();
            ptr_ = other.ptr_;
        }
        return *this;
    }

    ComPtr(ComPtr&& other) noexcept : ptr_(std::exchange(other.ptr_, nullptr)) {}

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            ptr_ = std::exchange(other.ptr_, nullptr);
        }
        return *this;
    }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }

    // Adopts a pointer this object does not own yet, taking a reference of its
    // own. The new reference is taken before the old is dropped, so re-adopting
    // the pointer already held cannot release the last reference to it.
    void copy_from(T* other) {
        if (other) other->AddRef();
        reset();
        ptr_ = other;
    }

    // For APIs that write a new reference into an out-parameter.
    T** put() {
        reset();
        return &ptr_;
    }

    void** put_void() { return reinterpret_cast<void**>(put()); }

    [[nodiscard]] T*   get() const { return ptr_; }
    T*                 operator->() const { return ptr_; }
    explicit           operator bool() const { return ptr_ != nullptr; }

private:
    T* ptr_ = nullptr;
};

}  // namespace coax::win

#pragma once

#include <unknwn.h>

#include <utility>

namespace kbun {

template <typename T>
class ComPtr {
public:
    ComPtr() noexcept = default;
    ComPtr(std::nullptr_t) noexcept {}

    explicit ComPtr(T* value) noexcept : value_(value) {
        InternalAddRef();
    }

    ComPtr(const ComPtr& other) noexcept : value_(other.value_) {
        InternalAddRef();
    }

    template <typename U>
    ComPtr(const ComPtr<U>& other) noexcept : value_(other.Get()) {
        InternalAddRef();
    }

    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}

    ~ComPtr() {
        InternalRelease();
    }

    ComPtr& operator=(const ComPtr& other) noexcept {
        if (this != &other) {
            ComPtr copy(other);
            Swap(copy);
        }
        return *this;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            InternalRelease();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    T* Get() const noexcept { return value_; }
    T* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

    T** Put() noexcept {
        InternalRelease();
        value_ = nullptr;
        return &value_;
    }

    void** PutVoid() noexcept {
        return reinterpret_cast<void**>(Put());
    }

    T* Detach() noexcept {
        return std::exchange(value_, nullptr);
    }

    void Attach(T* value) noexcept {
        if (value_ != value) {
            InternalRelease();
            value_ = value;
        }
    }

    void Reset() noexcept {
        InternalRelease();
        value_ = nullptr;
    }

    void Swap(ComPtr& other) noexcept {
        std::swap(value_, other.value_);
    }

private:
    void InternalAddRef() noexcept {
        if (value_) value_->AddRef();
    }

    void InternalRelease() noexcept {
        if (value_) value_->Release();
    }

    T* value_ = nullptr;
};

}  // namespace kbun


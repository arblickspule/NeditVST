#pragma once

// Lock-free publication of immutable shared state (state snapshots, loaded
// samples): UI thread publishes a fresh object, audio thread acquires once
// per block; the held shared_ptr keeps that generation alive.
//
// std::atomic<std::shared_ptr<T>> is a GNU libstdc++ / MS-STL extension and
// is REJECTED by libc++ (shared_ptr is not trivially copyable, so the C++
// standard -- which removed the atomic smart-pointer proposal -- makes the
// specialization ill-formed there). The portable fallback is the standard
// <memory> free functions atomic_load_explicit / atomic_store_explicit,
// which have identical semantics (single point of atomicity, lock-free on
// libc++'s inline spinlock). Those two functions are formally deprecated in
// C++20, so the fallback silences the deprecation notice.

#include <atomic>
#include <memory>
#include <utility>

namespace nedit::engine
{

#if defined(_LIBCPP_VERSION)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

template <typename T>
class AtomicSharedPtr
{
public:
    AtomicSharedPtr() = default;

    explicit AtomicSharedPtr (std::shared_ptr<T> ptr) noexcept
        : ptr_ (std::move (ptr))
    {
    }

    AtomicSharedPtr (const AtomicSharedPtr&) = delete;
    AtomicSharedPtr& operator= (const AtomicSharedPtr&) = delete;

    void store (std::shared_ptr<T> ptr, std::memory_order order) noexcept
    {
#if defined(_LIBCPP_VERSION)
        std::atomic_store_explicit (&ptr_, std::move (ptr), order);
#else
        ptr_.store (std::move (ptr), order);
#endif
    }

    [[nodiscard]] std::shared_ptr<T> load (std::memory_order order) const noexcept
    {
#if defined(_LIBCPP_VERSION)
        return std::atomic_load_explicit (&ptr_, order);
#else
        return ptr_.load (order);
#endif
    }

private:
#if defined(_LIBCPP_VERSION)
    mutable std::shared_ptr<T> ptr_;
#else
    std::atomic<std::shared_ptr<T>> ptr_;
#endif
};

#if defined(_LIBCPP_VERSION)
#pragma clang diagnostic pop
#endif

} // namespace nedit::engine
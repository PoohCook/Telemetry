// Fixed-capacity, lock-free, single-producer / single-consumer ring buffer.
// The classic ISR-to-task (or thread-to-thread) queue, with no heap allocation.
//
// Rules: exactly one thread calls try_push/try_emplace, exactly one thread
// calls pop. This task tests it single-threaded; Task 6 adds the second thread.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>       // std::construct_at (C++20), std::destroy_at (C++17)
#include <new>          // std::launder (C++17)
#include <optional>
#include <type_traits>
#include <utility>

namespace tlm {

// Power-of-two capacity lets the index wrap with a mask instead of a modulo.
template <std::size_t N>
concept PowerOfTwo = N >= 2 && (N & (N - 1)) == 0;

// Constraints say what T must support. A nothrow move keeps pop() noexcept and
// means an element can never be half-moved when an exception flies.
template <typename T, std::size_t N>
    requires PowerOfTwo<N> && std::is_nothrow_move_constructible_v<T>
class SpscRing {
public:
    SpscRing() noexcept = default;

    // The producer and consumer both hold references to the ring, so it must
    // never be copied or moved out from under them.
    SpscRing(const SpscRing&)            = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&)                 = delete;
    SpscRing& operator=(SpscRing&&)      = delete;

    // RAII: destroy whatever is still queued.
    ~SpscRing()
    {
        while (pop()) {}
    }

    // ---- Producer side -----------------------------------------------------

    // Construct an element in place from `args`. Returns false if full.
    template <typename... Args>
    [[nodiscard]] bool try_emplace(Args&&... args)
        noexcept(std::is_nothrow_constructible_v<T, Args...>)
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);  // only we write head
        const std::size_t tail = tail_.load(std::memory_order_acquire);  // see the consumer's frees
        if (head - tail == N) {
            return false;
        }
        std::construct_at(slot(head), std::forward<Args>(args)...);
        head_.store(head + 1, std::memory_order_release);                // publish the element
        return true;
    }

    // Copy or move an existing value in. `U&&` is a forwarding reference:
    // an lvalue is copied, a temporary or std::move'd value is moved.
    template <typename U>
        requires std::is_constructible_v<T, U&&>
    [[nodiscard]] bool try_push(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)
    {
        return try_emplace(std::forward<U>(value));
    }

    // ---- Consumer side -----------------------------------------------------

    // Move the oldest element out, or std::nullopt if empty.
    [[nodiscard]] std::optional<T> pop() noexcept
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);  // only we write tail
        const std::size_t head = head_.load(std::memory_order_acquire);  // see the producer's writes
        if (tail == head) {
            return std::nullopt;
        }
        T* const p = slot(tail);
        std::optional<T> out{std::move(*p)};
        std::destroy_at(p);
        tail_.store(tail + 1, std::memory_order_release);                // free the slot
        return out;
    }

    // ---- Observers (exact single-threaded; a snapshot when concurrent) -----

    [[nodiscard]] std::size_t size() const noexcept
    {
        return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] bool full() const noexcept { return size() == N; }
    [[nodiscard]] static constexpr std::size_t capacity() noexcept { return N; }

private:
    // Raw, aligned bytes: T is constructed only when pushed, so T doesn't need
    // a default constructor and empty slots cost no constructor calls.
    struct Slot {
        alignas(T) std::byte bytes[sizeof(T)];
    };

    T* slot(std::size_t index) noexcept
    {
        // The counters run forever; the mask maps them onto the array.
        // std::launder: tell the compiler a T really lives in these bytes now.
        return std::launder(reinterpret_cast<T*>(storage_[index & (N - 1)].bytes));
    }

    // Free-running counters. head - tail is the element count; unsigned
    // wraparound at 2^64 keeps that subtraction correct.
    // alignas(64): separate cache lines, so the two threads don't false-share.
    alignas(64) std::atomic<std::size_t> head_{0};   // written only by the producer
    alignas(64) std::atomic<std::size_t> tail_{0};   // written only by the consumer
    std::array<Slot, N> storage_;
};

} // namespace tlm

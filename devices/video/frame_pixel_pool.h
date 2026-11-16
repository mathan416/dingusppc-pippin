// Project: DingusPPC-Pippin
// File: devices/video/frame_pixel_pool.h
// Purpose: Reusable frame pixel buffers for presentation.
// Author: Iain Bennett
// Copyright (c) 2026 Iain Bennett
// SPDX-License-Identifier: GPL-3.0-or-later
// Change log:
//   2026-11-15 - Q219 baseline: added for the Pippin port (work from Q48 to Q219, before version control).
// Full history: PIPPIN.md and Git history.
// Private Pippin pilot: three reusable pixel buffers, no SDL or guest state.
#ifndef PIPPIN_FRAME_PIXEL_POOL_H
#define PIPPIN_FRAME_PIXEL_POOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pippin_frame_pool {

class FramePixelPool {
public:
    static constexpr size_t slot_count = 3;
    enum class SlotState : unsigned { free, writing, ready, reading };
    struct Stats {
        uint64_t allocations = slot_count;
        uint64_t acquired = 0, published = 0, reads = 0, released = 0, exhausted = 0;
        std::array<unsigned, 4> states{};
    };
    class WriteLease;
    class ReadyLease;
    class ReadLease;

private:
    struct Slot {
        std::vector<uint32_t> pixels;
        SlotState state = SlotState::free;
        uint64_t serial = 0;
    };
    struct State {
        std::mutex mutex;
        const int width, height;
        const size_t count;
        std::array<Slot, slot_count> slots;
        uint64_t next_serial = 0;
        Stats stats;
        static size_t checked_size(int width, int height) {
            if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() / 4 ||
                size_t(width) > std::numeric_limits<size_t>::max() / size_t(height) /
                                    sizeof(uint32_t) / slot_count)
                throw std::invalid_argument("Invalid three-buffer framebuffer dimensions");
            return size_t(width) * size_t(height);
        }
        State(int w, int h) : width(w), height(h), count(checked_size(w, h)) {
            // Integer objects are initialized once per mode, not per frame.
            for (auto& slot : slots) slot.pixels.resize(count);
        }
    };
    class Lease {
        friend class FramePixelPool;
        std::shared_ptr<State> state_;
        size_t index_ = 0;
        uint64_t serial_ = 0;
        SlotState expected_ = SlotState::free;
        Lease(std::shared_ptr<State> state, size_t index, uint64_t serial)
            : state_(std::move(state)), index_(index), serial_(serial), expected_(SlotState::writing) {}
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept
            : state_(std::move(other.state_)), index_(other.index_), serial_(other.serial_), expected_(other.expected_) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) {
                release();
                state_ = std::move(other.state_); index_ = other.index_;
                serial_ = other.serial_; expected_ = other.expected_;
            }
            return *this;
        }
        ~Lease() { release(); }
        void release() noexcept {
            if (!state_) return;
            // Keep State alive until after the lock guard is destroyed.
            auto state = std::move(state_);
            std::lock_guard<std::mutex> lock(state->mutex);
            auto& slot = state->slots[index_];
            if (slot.serial != serial_ || slot.state != expected_) std::terminate();
            slot.state = SlotState::free;
            ++state->stats.released;
        }
        void transition(SlotState from, SlotState to) {
            if (!state_ || expected_ != from) throw std::logic_error("Empty or invalid frame-pixel lease transition");
            std::lock_guard<std::mutex> lock(state_->mutex);
            auto& slot = state_->slots[index_];
            if (slot.serial != serial_ || slot.state != from) throw std::logic_error("Frame-pixel lease ownership mismatch");
            slot.state = expected_ = to;
            if (to == SlotState::ready) ++state_->stats.published;
            if (to == SlotState::reading) ++state_->stats.reads;
        }
        explicit operator bool() const noexcept { return bool(state_); }
        uint32_t* pixels() const noexcept { return state_ ? state_->slots[index_].pixels.data() : nullptr; }
        size_t size() const noexcept { return state_ ? state_->count : 0; }
        int width() const noexcept { return state_ ? state_->width : 0; }
        int height() const noexcept { return state_ ? state_->height : 0; }
        int pitch_bytes() const noexcept { return width() * 4; }
    };
    std::shared_ptr<State> state_;

public:
    class WriteLease {
        friend class FramePixelPool;
        Lease lease_;
        explicit WriteLease(Lease lease) : lease_(std::move(lease)) {}
    public:
        WriteLease() = default;
        WriteLease(const WriteLease&) = delete;
        WriteLease& operator=(const WriteLease&) = delete;
        WriteLease(WriteLease&&) noexcept = default;
        WriteLease& operator=(WriteLease&&) noexcept = default;
        explicit operator bool() const noexcept { return bool(lease_); }
        uint32_t* pixels() const noexcept { return lease_.pixels(); }
        size_t size() const noexcept { return lease_.size(); }
        int width() const noexcept { return lease_.width(); }
        int height() const noexcept { return lease_.height(); }
        int pitch_bytes() const noexcept { return lease_.pitch_bytes(); }
        ReadyLease publish() &&;
    };
    class ReadyLease {
        friend class WriteLease;
        Lease lease_;
        explicit ReadyLease(Lease lease) : lease_(std::move(lease)) {}
    public:
        ReadyLease() = default;
        ReadyLease(const ReadyLease&) = delete;
        ReadyLease& operator=(const ReadyLease&) = delete;
        ReadyLease(ReadyLease&&) noexcept = default;
        ReadyLease& operator=(ReadyLease&&) noexcept = default;
        explicit operator bool() const noexcept { return bool(lease_); }
        size_t size() const noexcept { return lease_.size(); }
        int width() const noexcept { return lease_.width(); }
        int height() const noexcept { return lease_.height(); }
        int pitch_bytes() const noexcept { return lease_.pitch_bytes(); }
        ReadLease read() &&;
    };
    class ReadLease {
        friend class ReadyLease;
        Lease lease_;
        explicit ReadLease(Lease lease) : lease_(std::move(lease)) {}
    public:
        ReadLease() = default;
        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;
        ReadLease(ReadLease&&) noexcept = default;
        ReadLease& operator=(ReadLease&&) noexcept = default;
        explicit operator bool() const noexcept { return bool(lease_); }
        const uint32_t* pixels() const noexcept { return lease_.pixels(); }
        size_t size() const noexcept { return lease_.size(); }
        int width() const noexcept { return lease_.width(); }
        int height() const noexcept { return lease_.height(); }
        int pitch_bytes() const noexcept { return lease_.pitch_bytes(); }
    };

    explicit FramePixelPool(int width, int height) : state_(std::make_shared<State>(width, height)) {}
    FramePixelPool(const FramePixelPool&) = delete;
    FramePixelPool& operator=(const FramePixelPool&) = delete;
    FramePixelPool(FramePixelPool&&) noexcept = default;
    FramePixelPool& operator=(FramePixelPool&&) noexcept = default;
    int width() const noexcept { return state_->width; }
    int height() const noexcept { return state_->height; }
    WriteLease try_acquire() {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for (size_t i = 0; i < slot_count; ++i) {
            auto& slot = state_->slots[i];
            if (slot.state != SlotState::free) continue;
            if (state_->next_serial == std::numeric_limits<uint64_t>::max())
                throw std::overflow_error("Frame-pixel lease serial exhausted");
            slot.state = SlotState::writing;
            slot.serial = ++state_->next_serial;
            ++state_->stats.acquired;
            return WriteLease(Lease(state_, i, slot.serial));
        }
        ++state_->stats.exhausted;
        return WriteLease();
    }
    Stats stats() const {
        std::lock_guard<std::mutex> lock(state_->mutex);
        Stats result = state_->stats;
        for (const auto& slot : state_->slots) ++result.states[unsigned(slot.state)];
        return result;
    }
};

inline FramePixelPool::ReadyLease FramePixelPool::WriteLease::publish() && {
    lease_.transition(SlotState::writing, SlotState::ready);
    return ReadyLease(std::move(lease_));
}
inline FramePixelPool::ReadLease FramePixelPool::ReadyLease::read() && {
    lease_.transition(SlotState::ready, SlotState::reading);
    return ReadLease(std::move(lease_));
}

} // namespace pippin_frame_pool
#endif

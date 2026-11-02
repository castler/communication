/********************************************************************************
 * Copyright (c) 2026 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_LOLA_SHM_HARNESS_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_LOLA_SHM_HARNESS_H

#include <cstdint>
#include <memory>
#include <string>

namespace score::mw::com::impl::lola::fuchsia::harness
{

constexpr std::uint16_t kServiceId{0x1234U};
constexpr std::uint16_t kInstanceId{1U};
constexpr std::uint16_t kSlots{2U};
constexpr std::uint8_t kMaxSubscribers{4U};
constexpr std::int32_t kSkeletonPid{4242};

struct SampleData
{
    std::uint64_t magic;
    std::uint64_t counter;
    char text[32];
};

constexpr std::uint64_t kSampleMagic{0x4C4F4C4153484D31ULL};

enum class Quality
{
    kQm,
    kAsilB,
};

/// \brief Skeleton side of one LoLa service instance with one event, created by the real SkeletonMemoryManager: the
///        data, QM control and (optionally) ASIL-B control regions are created through SharedMemoryFactory (the
///        injected VMO factory on Fuchsia). Only the runtime (pid, shm size mode) and the event binding are faked.
class SkeletonSide
{
  public:
    SkeletonSide(const bool with_asil_b);
    ~SkeletonSide();
    SkeletonSide(const SkeletonSide&) = delete;
    SkeletonSide& operator=(const SkeletonSide&) = delete;

    /// \brief CreateSharedMemory + event control/data of the one event, then writes `sample` into slot 0.
    bool CreateAndPublish(const SampleData& sample);

    /// \brief Tries to subscribe `slots` slots in the control region of `quality` from the skeleton's local view, i.e.
    ///        what a proxy in another process subscribed through its own mapping becomes visible here. Returns the
    ///        SubscribeResult as an integer (0 success, 1 max subscribers, 2 slot overflow, 3 retry failure); on
    ///        success the subscription is undone again.
    int ProbeSubscribe(const Quality quality, const std::uint16_t slots);

    /// \brief Skeleton-side update of slot 0 (data region is written by the owner only).
    void WriteSample(const SampleData& sample);

    /// \brief Publishes `counter` the way SkeletonEvent::Send does: EventDataControlComposite::AllocateNextSlot, write
    ///        the sample into the slot, EventReady with an increasing timestamp. Retries while no slot is free.
    bool Publish(const std::uint64_t counter);

    void Remove();

    std::string DataPath() const;
    std::string ControlPath(const Quality quality) const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// \brief Proxy side as in lola::Proxy: control region opened read-write, data region read-only, both via
///        SharedMemoryFactory::Open (no runtime needed, it only maps the structures).
class ProxySide
{
  public:
    /// \param transaction_log_id identifies the proxy in the transaction log set of the event (as the application id
    ///        does in LoLa); must differ between the proxy processes of one instance.
    explicit ProxySide(const std::uint32_t transaction_log_id = 1U);
    ~ProxySide();
    ProxySide(const ProxySide&) = delete;
    ProxySide& operator=(const ProxySide&) = delete;

    /// \brief Opens the control region of `quality` and the data region. false if a region is not reachable.
    bool Open(const Quality quality);

    std::int32_t SkeletonPid() const;
    bool ReadSample(SampleData& out) const;
    /// \brief Subscribes `slots` in the control region mapped by this proxy; returns the SubscribeResult as integer.
    int Subscribe(const std::uint16_t slots);

    /// \brief What a receive handler does: references the newest slot newer than `last_timestamp` through
    ///        ConsumerEventDataControlLocalView, copies the sample and releases the slot. Returns false if there is
    ///        nothing new; otherwise `last_timestamp` is advanced and `counter` is the sample's counter.
    bool ReceiveNewest(std::uint32_t& last_timestamp, std::uint64_t& counter);

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// \brief For the mutex/condvar test: the paths used for the proxy-visible regions of the LoLa instance.
std::string DataPathOf();
std::string ControlPathOf(const Quality quality);

}  // namespace score::mw::com::impl::lola::fuchsia::harness

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_LOLA_SHM_HARNESS_H

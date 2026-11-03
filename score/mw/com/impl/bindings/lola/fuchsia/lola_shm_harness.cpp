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
#include "score/mw/com/impl/bindings/lola/fuchsia/lola_shm_harness.h"

#include "score/mw/com/impl/bindings/lola/element_fq_id.h"
#include "score/mw/com/impl/bindings/lola/consumer_event_data_control_local_view.h"
#include "score/mw/com/impl/bindings/lola/event_control.h"
#include "score/mw/com/impl/bindings/lola/event_data_control_composite.h"
#include "score/mw/com/impl/bindings/lola/provider_event_data_control_local_view.h"
#include "score/mw/com/impl/bindings/lola/event_data_storage.h"
#include "score/mw/com/impl/bindings/lola/i_runtime.h"
#include "score/mw/com/impl/bindings/lola/service_data_control.h"
#include "score/mw/com/impl/bindings/lola/service_data_storage.h"
#include "score/mw/com/impl/bindings/lola/shm_path_builder.h"
#include "score/mw/com/impl/bindings/lola/transaction_log_registration_guard.h"
#include "score/mw/com/impl/bindings/lola/transaction_log_set.h"
#include "score/mw/com/impl/bindings/lola/skeleton_event_properties.h"
#include "score/mw/com/impl/bindings/lola/skeleton_memory_manager.h"
#include "score/mw/com/impl/configuration/lola_service_instance_deployment.h"
#include "score/mw/com/impl/configuration/lola_service_instance_id.h"
#include "score/mw/com/impl/configuration/lola_service_type_deployment.h"
#include "score/mw/com/impl/i_runtime.h"
#include "score/mw/com/impl/runtime.h"
#include "score/mw/com/impl/skeleton_event_binding.h"

#include "score/memory/shared/managed_memory_resource.h"
#include "score/memory/shared/shared_memory_factory.h"

#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <utility>

namespace score::mw::com::impl::lola::fuchsia::harness
{
namespace
{

constexpr std::size_t kRegionSize{64U * 1024U};
constexpr std::uint16_t kElementId{1U};

QualityType ToQualityType(const Quality quality)
{
    return quality == Quality::kQm ? QualityType::kASIL_QM : QualityType::kASIL_B;
}

ElementFqId FqId()
{
    return ElementFqId{kServiceId, kElementId, kInstanceId, ServiceElementType::EVENT};
}

[[noreturn]] void NotNeeded()
{
    std::abort();
}

/// Provides what the shm code path asks the runtime for (shm sizing mode, pid); everything else is not reached.
class FakeLolaRuntime final : public lola::IRuntime
{
  public:
    BindingType GetBindingType() const noexcept override
    {
        return BindingType::kLoLa;
    }
    IServiceDiscoveryClient& GetServiceDiscoveryClient() & noexcept override
    {
        NotNeeded();
    }
    tracing::IBindingTracingRuntime* GetTracingRuntime() noexcept override
    {
        return nullptr;
    }
    lola::IMessagePassingService& GetLolaMessaging() & noexcept override
    {
        NotNeeded();
    }
    bool HasAsilBSupport() const noexcept override
    {
        return true;
    }
    ShmSizeCalculationMode GetShmSizeCalculationMode() const noexcept override
    {
        return ShmSizeCalculationMode::kAnalysis;
    }
    RollbackSynchronization& GetRollbackSynchronization() & noexcept override
    {
        NotNeeded();
    }
    pid_t GetPid() const noexcept override
    {
        return kSkeletonPid;
    }
    IncarnationId GetIncarnation() const noexcept override
    {
        return IncarnationId::FromPid(kSkeletonPid);
    }
    const IIncarnationAuthority& GetIncarnationAuthority() const noexcept override
    {
        NotNeeded();
    }
    GlobalConfiguration::ApplicationId GetApplicationId() const noexcept override
    {
        return 1U;
    }
};

class FakeImplRuntime final : public impl::IRuntime
{
  public:
    explicit FakeImplRuntime(FakeLolaRuntime& lola_runtime) : lola_runtime_{lola_runtime} {}

    std::vector<InstanceIdentifier> resolve(const InstanceSpecifier&) const override
    {
        NotNeeded();
    }
    IBindingRuntime* GetBindingRuntime(const BindingType binding) const noexcept override
    {
        return binding == BindingType::kLoLa ? &lola_runtime_ : nullptr;
    }
    IServiceDiscovery& GetServiceDiscovery() & noexcept override
    {
        NotNeeded();
    }
    tracing::ITracingRuntime* GetTracingRuntime() const noexcept override
    {
        return nullptr;
    }
    const tracing::ITracingFilterConfig* GetTracingFilterConfig() const override
    {
        return nullptr;
    }

  private:
    FakeLolaRuntime& lola_runtime_;
};

/// Only stands for "one event" so that SkeletonMemoryManager sizes the containers for it.
class FakeEventBinding final : public SkeletonEventBinding
{
  public:
    Result<void> Send(impl::SampleAllocateePtr<void>, std::optional<SendTraceCallback>) override
    {
        NotNeeded();
    }
    Result<impl::SampleAllocateePtr<void>> Allocate(SampleAllocateeGuard) override
    {
        NotNeeded();
    }
    Result<impl::SamplePtr<void>> GetLatestSample(QualityType) override
    {
        NotNeeded();
    }
    Result<void> PrepareOffer(const std::optional<InitializeSampleCallback>&) override
    {
        NotNeeded();
    }
    void PrepareStopOffer() override
    {
        NotNeeded();
    }
    memory::DataTypeSizeInfo GetEventDataTypeSizeInfo() const override
    {
        return memory::DataTypeSizeInfo{sizeof(SampleData), alignof(SampleData)};
    }
    BindingType GetBindingType() const noexcept override
    {
        return BindingType::kLoLa;
    }
    void SetSkeletonEventTracingData(impl::tracing::SkeletonEventTracingData) noexcept override {}
};

LolaServiceInstanceDeployment MakeDeployment()
{
    LolaServiceInstanceDeployment deployment{std::optional<LolaServiceInstanceId>{LolaServiceInstanceId{kInstanceId}}};
    deployment.shared_memory_size_ = kRegionSize;
    deployment.control_qm_memory_size_ = kRegionSize;
    deployment.control_asil_b_memory_size_ = kRegionSize;
    return deployment;
}

}  // namespace

class SkeletonSide::Impl
{
  public:
    explicit Impl(const bool with_asil_b)
        : fake_lola_runtime{},
          fake_runtime{fake_lola_runtime},
          deployment{MakeDeployment()},
          type_deployment{kServiceId},
          path_builder{kServiceId},
          manager{with_asil_b ? QualityType::kASIL_B : QualityType::kASIL_QM,
                  path_builder,
                  deployment,
                  type_deployment,
                  kInstanceId,
                  kServiceId}
    {
        impl::Runtime::InjectMock(&fake_runtime);
    }

    ~Impl()
    {
        manager.RemoveSharedMemory();
        impl::Runtime::InjectMock(nullptr);
    }

    FakeLolaRuntime fake_lola_runtime;
    FakeImplRuntime fake_runtime;
    LolaServiceInstanceDeployment deployment;
    LolaServiceTypeDeployment type_deployment;
    ShmPathBuilder path_builder;
    SkeletonMemoryManager manager;
    FakeEventBinding event_binding;
    EventControl* control_qm{nullptr};
    EventControl* control_asil_b{nullptr};
    std::optional<ProviderEventDataControlLocalView<>> provider_qm;
    std::optional<ProviderEventDataControlLocalView<>> provider_asil_b;
    std::optional<EventDataControlComposite<>> composite;
    EventSlotStatus::EventTimeStamp timestamp{EventSlotStatus::INVALID_TIMESTAMP};
};

SkeletonSide::SkeletonSide(const bool with_asil_b) : impl_{std::make_unique<Impl>(with_asil_b)} {}

SkeletonSide::~SkeletonSide() = default;

bool SkeletonSide::CreateAndPublish(const SampleData& sample)
{
    SkeletonBinding::SkeletonEventBindings events{};
    SkeletonBinding::SkeletonFieldBindings fields{};
    events.emplace("HarnessEvent", std::ref(static_cast<SkeletonEventBinding&>(impl_->event_binding)));
    if (!impl_->manager.CreateSharedMemory(events, fields, std::nullopt).has_value())
    {
        return false;
    }
    const SkeletonEventProperties properties{kSlots, 0U, 0U, false, kMaxSubscribers, true};
    const auto controls = impl_->manager.CreateEventControlsInCreatedSharedMemory(FqId(), properties);
    impl_->control_qm = &controls.first.get();
    impl_->control_asil_b = controls.second;
    static_cast<void>(impl_->manager.CreateEventDataInCreatedSharedMemory(
        FqId(), properties, memory::DataTypeSizeInfo{sizeof(SampleData), alignof(SampleData)}));
    WriteSample(sample);
    impl_->provider_qm.emplace(impl_->control_qm->data_control);
    ProviderEventDataControlLocalView<>* asil_b_view{nullptr};
    if (impl_->control_asil_b != nullptr)
    {
        asil_b_view = &impl_->provider_asil_b.emplace(impl_->control_asil_b->data_control);
    }
    impl_->composite.emplace(*impl_->provider_qm, asil_b_view);
    return true;
}

bool SkeletonSide::Publish(const std::uint64_t counter)
{
    auto allocation = impl_->composite->AllocateNextSlot();
    for (int retries = 0; !allocation.allocated_slot_index.has_value() && retries < 1000; ++retries)
    {
        allocation = impl_->composite->AllocateNextSlot();
    }
    if (!allocation.allocated_slot_index.has_value())
    {
        return false;
    }
    const auto slot = allocation.allocated_slot_index.value();
    auto& data = impl_->manager.RetrieveEventDataFromOpenedSharedMemory(FqId());
    SampleData sample{};
    sample.magic = kSampleMagic;
    sample.counter = counter;
    std::memcpy(data.GetTypeErasedDataSlot(slot, sizeof(SampleData)), &sample, sizeof(SampleData));
    ++impl_->timestamp;
    impl_->composite->EventReady(slot, impl_->timestamp);
    return true;
}

void SkeletonSide::WriteSample(const SampleData& sample)
{
    auto& data = impl_->manager.RetrieveEventDataFromOpenedSharedMemory(FqId());
    std::memcpy(data.GetTypeErasedDataSlot(0U, sizeof(SampleData)), &sample, sizeof(SampleData));
}

int SkeletonSide::ProbeSubscribe(const Quality quality, const std::uint16_t slots)
{
    EventControl* const control = (quality == Quality::kQm) ? impl_->control_qm : impl_->control_asil_b;
    if (control == nullptr)
    {
        return -1;
    }
    const auto result = control->subscription_control.Subscribe(slots);
    if (result == SubscribeResult::kSuccess)
    {
        control->subscription_control.Unsubscribe(slots);
    }
    return static_cast<int>(result);
}

void SkeletonSide::Remove()
{
    impl_->manager.RemoveSharedMemory();
}

std::string SkeletonSide::DataPath() const
{
    return DataPathOf();
}

std::string SkeletonSide::ControlPath(const Quality quality) const
{
    return ControlPathOf(quality);
}

class ProxySide::Impl
{
  public:
    std::shared_ptr<memory::shared::ManagedMemoryResource> control;
    std::shared_ptr<memory::shared::ManagedMemoryResource> data;
    ServiceDataStorage* storage{nullptr};
    ServiceDataControl* service_control{nullptr};
    std::unique_ptr<ConsumerEventDataControlLocalView<>> view;
    std::optional<TransactionLogRegistrationGuard> log_guard;
    std::uint32_t transaction_log_id{1U};
};

ProxySide::ProxySide(const std::uint32_t transaction_log_id) : impl_{std::make_unique<Impl>()}
{
    impl_->transaction_log_id = transaction_log_id;
}

ProxySide::~ProxySide() = default;

bool ProxySide::Open(const Quality quality)
{
    impl_->control = memory::shared::SharedMemoryFactory::Open(ControlPathOf(quality), true);
    impl_->data = memory::shared::SharedMemoryFactory::Open(DataPathOf(), false);
    if ((impl_->control == nullptr) || (impl_->data == nullptr))
    {
        return false;
    }
    impl_->service_control = static_cast<ServiceDataControl*>(impl_->control->getUsableBaseAddress());
    impl_->storage = static_cast<ServiceDataStorage*>(impl_->data->getUsableBaseAddress());
    return (impl_->service_control != nullptr) && (impl_->storage != nullptr);
}

std::int32_t ProxySide::SkeletonPid() const
{
    return static_cast<std::int32_t>(impl_->storage->GetSkeletonPid());
}

bool ProxySide::ReadSample(SampleData& out) const
{
    const auto& storage = std::as_const(*impl_->storage);
    const void* const slot = storage.GetEventDataStorage(FqId()).GetTypeErasedDataSlot(0U, sizeof(SampleData));
    if (slot == nullptr)
    {
        return false;
    }
    std::memcpy(&out, slot, sizeof(SampleData));
    return true;
}

int ProxySide::Subscribe(const std::uint16_t slots)
{
    auto* const found = impl_->service_control->event_controls_.find(FqId());
    if (found == impl_->service_control->event_controls_.end())
    {
        return -1;
    }
    return static_cast<int>(found->second.subscription_control.Subscribe(slots));
}

bool ProxySide::ReceiveNewest(std::uint32_t& last_timestamp, std::uint64_t& counter)
{
    if (impl_->view == nullptr)
    {
        auto* const found = impl_->service_control->event_controls_.find(FqId());
        if (found == impl_->service_control->event_controls_.end())
        {
            return false;
        }
        impl_->view = std::make_unique<ConsumerEventDataControlLocalView<>>(found->second.data_control);
        // Like a subscribed proxy event: the consumer view logs its slot references in the proxy's transaction log.
        auto guard = found->second.transaction_log_set_.RegisterProxyElement(impl_->transaction_log_id, *impl_->view);
        if (!guard.has_value())
        {
            return false;
        }
        impl_->log_guard.emplace(std::move(guard).value());
    }
    const auto slot = impl_->view->ReferenceNextEvent(last_timestamp);
    if (!slot.has_value())
    {
        return false;
    }
    const auto& storage = std::as_const(*impl_->storage);
    SampleData sample{};
    std::memcpy(&sample,
                storage.GetEventDataStorage(FqId()).GetTypeErasedDataSlot(*slot, sizeof(SampleData)),
                sizeof(SampleData));
    last_timestamp = (*impl_->view)[*slot].GetTimeStamp();
    impl_->view->DereferenceEvent(*slot);
    counter = sample.counter;
    return true;
}

std::string DataPathOf()
{
    return ShmPathBuilder{kServiceId}.GetDataChannelShmName(kInstanceId);
}

std::string ControlPathOf(const Quality quality)
{
    return ShmPathBuilder{kServiceId}.GetControlChannelShmName(kInstanceId, ToQualityType(quality));
}

}  // namespace score::mw::com::impl::lola::fuchsia::harness

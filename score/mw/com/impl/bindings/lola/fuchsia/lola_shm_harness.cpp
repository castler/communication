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
#include "score/mw/com/impl/bindings/lola/event_control.h"
#include "score/mw/com/impl/bindings/lola/event_data_storage.h"
#include "score/mw/com/impl/bindings/lola/i_runtime.h"
#include "score/mw/com/impl/bindings/lola/service_data_control.h"
#include "score/mw/com/impl/bindings/lola/service_data_storage.h"
#include "score/mw/com/impl/bindings/lola/shm_path_builder.h"
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
        NotNeeded();
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
};

ProxySide::ProxySide() : impl_{std::make_unique<Impl>()} {}

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

std::string DataPathOf()
{
    return ShmPathBuilder{kServiceId}.GetDataChannelShmName(kInstanceId);
}

std::string ControlPathOf(const Quality quality)
{
    return ShmPathBuilder{kServiceId}.GetControlChannelShmName(kInstanceId, ToQualityType(quality));
}

}  // namespace score::mw::com::impl::lola::fuchsia::harness

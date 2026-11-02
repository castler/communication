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
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_VMO_SHM_RUNTIME_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_VMO_SHM_RUNTIME_H

#include "score/memory/shared/fuchsia/i_vmo_handle_provider.h"
#include "score/memory/shared/fuchsia/vmo_shared_memory_factory.h"
#include "score/memory/shared/lock_provider/i_lock_provider.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_client.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fidl_vmo_handle_server.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/fuchsia_event_notifier.h"
#include "score/mw/com/impl/bindings/lola/fuchsia/vmo_role.h"

#include <fidl/score.mw.com/cpp/fidl.h>
#include <lib/async/dispatcher.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace score::mw::com::impl::lola::fuchsia
{

/// \brief Connects LoLa's shared-memory path to Fuchsia: SharedMemoryFactory::* creates and opens VMOs, the skeleton
///        side publishes them through FidlVmoHandleServer (served as `score.mw.com.Endpoint`), the proxy side fetches
///        them through a FidlVmoHandleClient. Replaces shm_open for LoLa's data, QM control and ASIL-B control region.
/// \details The runtime installs itself in SharedMemoryFactory (InjectMock) and, if given, in LockProvider. Lifetime:
///          create it before the first LoLa skeleton/proxy, keep it alive until the last one is gone, then destroy it
///          (the destructor restores the defaults). LoLa has no event loop of its own, so the application owns the
///          dispatcher and registers `server().CreateHandler(application_id)` in its outgoing directory.
///
///          Proxy side: the first Acquire() of a service instance connects (EndpointConnector) and attaches; data, QM
///          control and ASIL-B control arrive with that one Attach. Opening a role twice re-attaches.
///          Not covered by Attach: regions that the proxy creates (method channels, "lola-methods-*") stay local.
///
///          Events: every attached provider is registered with the notifier(), which also becomes the process-wide
///          IEventNotifier used by the event-notifying message passing service instance (see
///          event_notifying_service_instance.h).
class VmoShmRuntime final
{
  public:
    using EndpointConnector = std::function<std::optional<fidl::ClientEnd<score_mw_com::Endpoint>>()>;

    /// \brief Connects to the routed `/svc/score.mw.com.Endpoint` of the component.
    static std::optional<fidl::ClientEnd<score_mw_com::Endpoint>> ConnectRoutedEndpoint();

    VmoShmRuntime(async_dispatcher_t* dispatcher,
                  EndpointConnector connector = ConnectRoutedEndpoint,
                  score::memory::shared::ILockProvider* lock_provider = nullptr);
    ~VmoShmRuntime();

    VmoShmRuntime(const VmoShmRuntime&) = delete;
    VmoShmRuntime& operator=(const VmoShmRuntime&) = delete;

    FidlVmoHandleServer& server() noexcept { return *server_; }
    score::memory::shared::fuchsia::VmoSharedMemoryFactory& factory() noexcept { return *factory_; }
    /// \brief Event notification of this process (installed as IEventNotifier::Current() for the lifetime of the
    ///        runtime): skeleton NotifyEvent signals the subscribers, proxy receive handlers are woken by the
    ///        notification eventpair of the attached provider.
    FuchsiaEventNotifier& notifier() noexcept { return *notifier_; }

    /// \brief "/lola-data-<svc>-<inst>", "/lola-ctl-<svc>-<inst>" and "/lola-ctl-<svc>-<inst>-b" share one instance key.
    static std::string InstanceKeyOf(const std::string& region_key);

  private:
    class CompositeProvider;

    std::shared_ptr<FidlVmoHandleServer> server_;
    std::unique_ptr<FuchsiaEventNotifier> notifier_;
    std::shared_ptr<CompositeProvider> provider_;
    std::unique_ptr<score::memory::shared::fuchsia::VmoSharedMemoryFactory> factory_;
    bool lock_provider_installed_{false};
};

}  // namespace score::mw::com::impl::lola::fuchsia

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_FUCHSIA_VMO_SHM_RUNTIME_H

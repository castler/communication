# LoLa on Fuchsia — Architecture Concept

| | |
|---|---|
| **Status** | Draft / proposal (RFC). Parts are implemented and verified, parts are design only. See [Status of this document](#13-status-of-this-document-what-is-verified-and-what-is-not). |
| **Scope** | Running `mw::com` (LoLa) and `score/message_passing` on the Fuchsia operating system |
| **Audience** | Engineers who need to understand the concept and its impact on the current code base before deciding on or implementing it |

## Table of contents

1. [Purpose and summary](#1-purpose-and-summary)
2. [LoLa today: what is OS-specific?](#2-lola-today-what-is-os-specific)
3. [Fuchsia primer (only what we need)](#3-fuchsia-primer-only-what-we-need)
4. [Goals, non-goals, principles](#4-goals-non-goals-principles)
5. [Concept overview](#5-concept-overview)
6. [Detailed design](#6-detailed-design)
7. [End-to-end examples](#7-end-to-end-examples)
8. [Alternatives considered](#8-alternatives-considered)
9. [Pros and cons of the recommended concept](#9-pros-and-cons-of-the-recommended-concept)
10. [Impact on the current implementation](#10-impact-on-the-current-implementation)
11. [Migration plan](#11-migration-plan)
12. [Risks and open questions](#12-risks-and-open-questions)
13. [Status of this document: what is verified and what is not](#13-status-of-this-document-what-is-verified-and-what-is-not)
14. [Glossary](#14-glossary)
15. [Appendix A: Experiment results](#a-experiment-results)

---

## 1. Purpose and summary

LoLa moves event/field samples between processes with **zero copy through shared memory** and uses a small amount of
**message passing** for control (subscription, notification registration, method calls). Today both mechanisms are
implemented on top of POSIX (Linux) and QNX primitives.

Fuchsia has neither POSIX shared-memory objects with a global namespace, nor `flock`-style cross-process file locks as
a reliable primitive, nor process credentials (pid/uid) on its IPC channels. Instead it offers:

* **FIDL** over **channels** for typed IPC,
* **VMOs** (virtual memory objects) as the shared-memory primitive, transferable as handles,
* a **component framework** with **capability routing** that decides *who may talk to whom*.

**Recommendation in one sentence:** keep LoLa's proven data plane (zero-copy slots in shared memory, atomics for
synchronisation) and re-host it on VMOs; replace the POSIX/QNX control plane with FIDL; and move *who is allowed to
connect* out of uid lists and into Fuchsia capability routing.

```text
                         LoLa on Linux / QNX                  LoLa on Fuchsia (proposed)
                  ┌─────────────────────────────┐      ┌─────────────────────────────────┐
 Data plane       │ POSIX shm / typed memory    │      │ VMO (mapped by skeleton+proxies)│
                  │ (shm_open + mmap)           │      │                                 │
                  ├─────────────────────────────┤      ├─────────────────────────────────┤
 Control plane    │ unix-domain sockets / QNX   │      │ FIDL (score.messagepassing +    │
                  │ dispatch (message_passing)  │      │ attach/discovery protocols)     │
                  ├─────────────────────────────┤      ├─────────────────────────────────┤
 Access control   │ uid/pid + allowed_consumer  │      │ Component capability routing    │
                  │ lists, shm file ACLs        │      │ (offer/expose/use, `as` rename) │
                  ├─────────────────────────────┤      ├─────────────────────────────────┤
 Liveness /       │ flock'ed marker files, pid, │      │ eventpair/peer-closed signals,  │
 partial restart  │ /tmp flag files, inotify    │      │ generation counter, broker      │
                  └─────────────────────────────┘      └─────────────────────────────────┘
```

---

## 2. LoLa today: what is OS-specific?

The binding-independent `mw::com::impl` layer talks to a binding through the interfaces `ProxyBinding`,
`SkeletonBinding`, `ProxyEventBindingBase`, `SkeletonEventBindingBase` and `IBindingRuntime`
(`score/mw/com/impl/*_binding.h`, `i_binding_runtime.h`). `BindingType` currently knows `kLoLa`, `kFake`, `kSomeIp`.
The LoLa binding is in `score/mw/com/impl/bindings/lola/`.

The following table lists every place where LoLa touches OS-specific functionality, and what that means on Fuchsia.
It is the basis for [section 10](#10-impact-on-the-current-implementation).

| Concern | How it works today | Where | Fuchsia problem |
|---|---|---|---|
| **Data/control shared memory** | Named shared-memory objects (`data`, `control` per QoS level, optional `method` channel) created via `score::memory::shared::SharedMemoryFactory` / `SharedMemoryResource`, which uses `shm_open`, `ftruncate`, `mmap`, `fchmod`, a lock file for create-or-open, and typed memory on QNX. Contents are addressed through `OffsetPtr` and allocated through a `memory_resource`. | `bindings/lola/skeleton_memory_manager.cpp`, `proxy.cpp`; `score/memory/shared/shared_memory_resource.cpp` | No global shm namespace and (to be verified) no `shm_open`. Must be replaced by VMOs that are *handed over*. |
| **Shm access rights** | `os::Acl` / file mode bits, using the skeleton's uid (`UserPermissions`). | `skeleton_memory_manager.cpp`, `proxy.cpp` (~L772) | No uids. Rights are properties of VMO handles. |
| **Partial restart / usage detection** | A *usage marker* file under `/tmp/mw_com_lola/partial_restart/` (`/tmp_discovery/` on QNX). The skeleton tries an **exclusive `flock`**; proxies hold a **shared `flock`**. If the skeleton gets the lock, no proxy uses the old instance and it may clean up. The skeleton pid is kept in `ServiceDataStorage`; transaction logs are rolled back per application id. | `skeleton.cpp`, `proxy.cpp` (`ExecutePartialRestartLogic`), `partial_restart_path_builder.cpp`, `service_data_storage.cpp`, `transaction_log_rollback_executor.cpp`, `dependability/partial_restart/` | No shared filesystem, no cross-component `flock`. Needs another liveness mechanism. |
| **Service discovery** | *Flag files* (`existence-<service>-<instance>`) in a shared directory, watched with **inotify** by `FlagFileCrawler` / `ServiceDiscoveryClient` (worker thread). | `bindings/lola/service_discovery/` | No shared directory, no inotify. Needs a discovery service. The seam `IServiceDiscoveryClient` already exists. |
| **Control messaging** | `score/message_passing` (unix-domain sockets, or QNX dispatch). Server names derived from `CreateMessagePassingName(asil, pid)`. | `bindings/lola/messaging/` | **Done** for the transport (see [6.1](#61-control-plane-message_passing-over-fidl-implemented)); identity (pid/uid) still open. |
| **Peer identity** | The pid of a message-passing peer is the routing key (`node id`), stored as connection user data, used as `sender_pid` and to derive server names. The uid is checked against the per-instance `allowed_consumer` lists (`strict_permissions`). | `messaging/message_passing_service_instance.cpp`, `skeleton.cpp` (`GetAllowedConsumers`), `runtime.cpp` | Fuchsia channels carry **no credentials**. The backend currently reports `{0,0,0}`. |
| **Process-global paths** | `/tmp`, `/tmp_discovery`, shm names. | `path_builder`, `partial_restart_path_builder` | No global paths. |
| **Build** | Platform selects live in dependencies (`score/message_passing/BUILD`, `score/memory/shared/BUILD`). LoLa's own BUILD has no `@platforms//os:fuchsia` handling. Component tests are Linux-only. | `bindings/lola/BUILD`, `bindings/lola/test/BUILD` | New Fuchsia targets and packages. |

What is **not** OS specific and therefore reusable unchanged: the slot allocation and reference counting
(`EventDataControl`, `EventSlotStatus`, `SlotCollector`, …), the subscription state machine, transaction logs
(as a data structure), the local views, the generic proxy/skeleton framework, and the configuration model. They are
atomics on memory that happens to be shared (see
[shared memory layout](../shared_mem_layout/README.md)).

---

## 3. Fuchsia primer (only what we need)

| Fuchsia concept | Meaning | Why it matters here |
|---|---|---|
| **Component** | Unit of isolation, started by the component manager from a manifest (CML). Own namespace, no global filesystem. | A skeleton and each proxy application will be separate components. |
| **Channel** | Bidirectional, message-oriented IPC primitive carrying bytes and **handles**. Max message size **64 KiB**, max **64 handles**. | FIDL runs over channels. Large payloads need VMOs. |
| **FIDL** | Interface definition language and wire format on channels. Protocols have methods, one-way events, hanging-get patterns. | Our control plane. |
| **VMO** | Virtual memory object: a chunk of pageable memory, referenced by **handles** with **rights** (read, write, map, duplicate, transfer…). Mapped into address spaces with `zx_vmar_map`. | Replacement for POSIX shared memory. |
| **Handle semantics** | Sending a handle over a channel **moves** it. `zx_handle_duplicate` creates another handle to the same object (optionally with fewer rights); no data is copied. | One VMO, one duplicate per consumer. |
| **Eventpair / peer-closed signals** | When one end of a channel/eventpair/socket is closed (also on process death), the other end observes `PEER_CLOSED`. | Liveness detection without files or pids. |
| **Capability routing** | A component can only use capabilities that are **offered** to it by its parent (or exposed by a child), as declared in manifests. Routing can **rename** (`as`). | This is Fuchsia's access control. |
| **Server cannot see the client** | A FIDL server learns nothing about which component connected, unless the routing makes it distinguishable. | Drives the identity design in [6.4](#64-identity-and-access-control). |

---

## 4. Goals, non-goals, principles

**Goals**

1. Applications written against `mw::com` run unchanged on Fuchsia (same API, same configuration model).
2. Keep the performance profile: **zero-copy, syscall-free hot path** for samples (no per-sample FIDL traffic).
3. Reuse as much of LoLa as possible; confine Fuchsia code to well-defined seams.
4. Access control and identity use the platform's mechanism and are **explicit and reviewable** (safety).
5. Preserve partial-restart semantics (a restarted skeleton/proxy recovers without restarting its peers).

**Non-goals (for the first iteration)**

* Binary compatibility of the shared-memory layout across Fuchsia and Linux/QNX.
* Cross-host communication (SOME/IP is a different binding).
* A native FIDL representation of the user's payload types (see [6.7](#67-typed-interfaces-and-idl-options)).

**Principles**

* *Data plane in VMOs, control plane in FIDL.* Never put per-sample traffic on a channel.
* *Let the OS decide who may connect; let LoLa decide what a connected peer gets.*
* *Identity is an application-level concept, not a pid/uid.*
* *Every OS-specific behaviour sits behind an interface that already exists or is introduced once, with Linux/QNX as the first two implementations.*

---

## 5. Concept overview

### 5.1 Layers

```text
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ Application (mw::com API: Skeleton / Proxy / events / fields / methods)  │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ mw::com::impl (binding independent)                                      │
 ├──────────────────────────────────────────────────────────────────────────┤
 │ LoLa binding: slots, subscriptions, transaction logs (OS independent)    │
 ├───────────────┬────────────────┬──────────────────┬──────────────────────┤
 │ Shared-memory │ Peer liveness  │ Service          │ Messaging            │
 │ provider      │ / restart      │ discovery        │ (message_passing)    │
 │ (seam 1)      │ (seam 2)       │ (seam 3)         │ (seam 4)             │
 ├───────────────┼────────────────┼──────────────────┼──────────────────────┤
 │ Linux: shm    │ Linux: flock   │ Linux: flag file │ Linux: unix domain   │
 │ QNX:   typed  │ QNX:   flock   │ + inotify        │ QNX:   dispatch      │
 │ Fuchsia: VMO  │ Fuchsia: signal│ Fuchsia: FIDL    │ Fuchsia: FIDL        │
 │   (new)       │ + generation   │   discovery (new)│   (implemented)      │
 │               │   (new)        │                  │                      │
 └───────────────┴────────────────┴──────────────────┴──────────────────────┘
 Cross-cutting: identity & access control (seam 5): uid/pid lists  ⇄  capability routing
```

Four runtime seams plus one cross-cutting concern. Seam 4 is done. Seams 1–3 each have an existing abstraction in
the code that can host the new implementation (`ISharedMemoryFactory`, `IPartialRestartPathBuilder` / the lock
helpers, `IServiceDiscoveryClient`), but the *abstraction level may need to be raised* (see section 10).

### 5.2 Runtime topology

```text
 ┌── realm (integrator-authored/generated CML: the access policy) ─────────────────────────┐
 │                                                                                         │
 │  ┌────────────┐  FIDL (discovery)   ┌──────────────────┐                                │
 │  │ discovery  │◄────────────────────┤ skeleton comp.   │                                │
 │  └─────▲──────┘                     │  - owns VMOs     │                                │
 │        │                            │  - serves        │   FIDL: Attach, events         │
 │        │ FIDL (find/watch)          │    per-consumer  │◄───────────┐                   │
 │  ┌─────┴──────┐                     │    endpoints     │            │                   │
 │  │ proxy app A│─────────────────────►                  │            │                   │
 │  └─────┬──────┘   Attach(): VMO dup └──────────────────┘            │                   │
 │        │                                                           │                   │
 │        └──────────── mapped VMO (data: read-only, control: rw) ────┘                   │
 │  ┌────────────┐                                                                         │
 │  │ proxy app B│  (same, with its own endpoint and its own VMO duplicates)              │
 │  └────────────┘                                                                         │
 │  proxy app C: no capability routed → cannot connect                                     │
 └─────────────────────────────────────────────────────────────────────────────────────────┘
```

---

## 6. Detailed design

### 6.1 Control plane: `message_passing` over FIDL (implemented)

`score/message_passing` now has a Fuchsia backend, **separate from `unix_domain`** (which stays Linux-only):

* `score/message_passing/fuchsia/` — `FuchsiaEngine`, `FuchsiaServer`, client/server factories.
* Selected via `#elif defined(__Fuchsia__)` in `engine.h`, `client_factory.h`, `server_factory.h` and via a
  `select()` on `@platforms//os:fuchsia` in `score/message_passing/BUILD`.
* There is no `AF_UNIX`. A client creates a **Zircon stream socket pair** and sends one end to the server through the
  FIDL protocol `score.messagepassing.Connector.Connect(identifier, socket)`
  (`fuchsia/connector.fidl`). After that, framing (code + 16-bit size + payload) runs over the socket and the
  descriptors are polled exactly like the other backends.
* A `FuchsiaConnectionRegistry` (leaked singleton running an `async::Loop` thread) hosts the `Connector` service and
  keeps the in-process shortcut when client and server live in the same process.
* Verified on FEMU: in-process echo and cross-component echo (separate components, routed capability).

Message passing remains the transport for LoLa's *control* traffic only: subscription, notification registration,
method calls. It is **not** used for samples.

Known limitation: the server cannot learn the peer identity, so `GetClientIdentity()` returns `{0,0,0}`. See 6.4.

### 6.2 Data plane: shared memory on VMOs

LoLa's data and control objects are already `SharedMemoryResource` instances accessed via `OffsetPtr` (offsets
relative to the mapping base, so any base address works). The OS dependency is concentrated in how a
`SharedMemoryResource` is **created / opened / mapped**. On Fuchsia that becomes:

| Linux / QNX | Fuchsia |
|---|---|
| `shm_open(name)` + `ftruncate` + `mmap` | `zx_vmo_create(size)` + `zx_vmar_map` |
| "open by name" (global namespace) | "receive a VMO handle" (explicit handout) |
| file mode bits / ACL by uid | handle rights on each duplicate |
| lock file for create-or-open race | not needed: the creator owns the VMO, consumers only get handles |

Design rules:

1. **One data VMO and one control VMO per service instance** (and QoS level), mirroring today's objects.
   * **Data VMO**: skeleton writes, proxies map **read-only** (duplicate with `READ|MAP|TRANSFER` rights, no `WRITE`).
   * **Control VMO**: proxies **must write** (slot reference counts, subscription state, transaction logs), so it is
     mapped read/write by consumers. This is the same trust model LoLa already has on Linux (the control shm is
     writable by allowed consumers).
2. The skeleton hands out **one duplicate per proxy per VMO** during `Attach` (see [6.3](#63-attach-and-service-discovery)).
   After that, FIDL messages carry **no handles and no payload**, only small indices/sequence numbers where a
   notification is needed.
3. Consumers **map once** at attach and keep the mapping; hot-path code uses the existing *local views*
   (`ConsumerEventDataControlLocalView`, …), which already resolve `OffsetPtr`s once.
4. Sending a handle **moves** it. To keep using a VMO the skeleton always sends **duplicates** and keeps its original.
   A new VMO per sample would be possible (`zx_vmo_create_child`) but costs allocation and mapping per message and is
   **not** the design.

```text
 skeleton                                  kernel                          proxy
 --------                                  ------                          -----
 zx_vmo_create(data) ──────────────────────► VMO object (pages)
 zx_vmo_create(control)
 zx_vmar_map(rw)                                                          Attach() request
 write slot, atomics ...                                                       │
 zx_handle_duplicate(data, R|M|T) ──► FIDL reply carries handle(s) ────────────►
 zx_handle_duplicate(control, R|W|M|T)                                     zx_vmar_map(ro / rw)
                                                                           read slots in place
                                                                           CAS on control atomics
```

**Control-block details to decide:** the control VMO today also contains the `application_id_pid_mapping_`; its
meaning changes (see 6.5). `ServiceDataStorage::skeleton_pid_` is replaced by a *skeleton generation*.

### 6.3 Attach and service discovery

Today a proxy finds a service instance by watching flag files and then opens shared memory *by name*. On Fuchsia
neither exists, so two FIDL protocols take over.

**Discovery** replaces flag files + inotify and plugs in behind `IServiceDiscoveryClient`:

* Skeletons announce `Offer(id, endpoint)` / withdraw by closing the channel (peer-closed = `StopOffer`, even on crash).
* Proxies call `Find(id)` (one-shot) or `Watch(id)` (hanging get) and receive the offered endpoint.

**Attach** replaces "open shm by name":

* `Attach(application_id, event_channel)` returns the data/control VMO duplicates, the layout metadata, the skeleton
  `generation` and a liveness eventpair (see 6.5).
* The returned endpoint is **per consumer** (see 6.4), so the skeleton knows which consumer it is serving.

A FIDL sketch (not compiled; names are illustrative):

```fidl
library score.mw.com;

using zx;

type ServiceInstanceId = struct {
    service_id uint64;
    instance_id uint32;
    major_version uint8;
    minor_version uint8;
};

type AttachError = flexible enum : uint32 {
    NOT_FOUND = 1;
    NOT_ALLOWED = 2;
    INCOMPATIBLE_VERSION = 3;
};

closed protocol Endpoint {
    Attach(resource struct {
        application_id uint64;
        events client_end:EventChannel;
    }) -> (resource struct {
        data_vmo zx.Handle:VMO;           // read + map only
        control_vmo zx.Handle:VMO;        // read + write + map
        layout vector<uint8>:4096;        // opaque layout descriptor (offsets of the anchor objects)
        generation uint64;                // changes with every skeleton restart
        liveness zx.Handle:EVENTPAIR;     // peer-closed == skeleton died
    }) error AttachError;
};

closed protocol EventChannel {
    // One-way: pure wake-up. The sample itself is in the VMO.
    strict -> OnEvent(struct { element_index uint32; slot uint32; sequence uint64; });
    strict -> OnSkeletonRestart(struct { generation uint64; });
};

closed protocol ServiceDiscovery {
    Offer(resource struct { id ServiceInstanceId; endpoint client_end:Endpoint; }) -> ();
    Find(struct { id ServiceInstanceId; }) -> (resource struct { endpoint client_end:Endpoint; })
        error AttachError;
    Watch(struct { id ServiceInstanceId; }) -> (struct { offered bool; });
};
```

Subscribe/unsubscribe, notification registration and method calls continue to run through `message_passing`
(or may later move to this protocol as typed FIDL messages; see [open questions](#12-risks-and-open-questions)).

### 6.4 Identity and access control

**The problem.** LoLa uses the peer's pid and uid for three things: (1) the pid as routing key and as part of server
names, (2) the uid checked against `allowed_consumer`/`allowed_provider` lists, (3) OS-level shm ACLs. Zircon channels
and sockets carry none of this, and a FIDL server cannot ask who connected. Returning `{0,0,0}` would silently turn
every authorisation check into "everybody is the same user".

**The concept.** Fuchsia's access control *is* capability routing: a component that has not been offered a capability
cannot open it. So:

1. **Authorisation moves into the realm's CML.** `allowed_consumer` for an instance becomes the set of components to
   which the skeleton's endpoint is *offered*.
2. **Identity is carried by the endpoint, not by credentials.** The skeleton exposes **one endpoint per allowed
   consumer** (capability-as-identity). The realm offers it to the consumer and renames it with `as`, so the consumer
   always uses the same name. The skeleton serves each endpoint at its own path and therefore knows which consumer is
   behind a connection.
3. **`ClientIdentity` becomes an opaque type.** It supports equality, hashing, a log string and
   `IsAllowed(AllowedConsumers)`. On Linux/QNX it wraps pid/uid. On Fuchsia it wraps the *application id* derived
   from the endpoint. Call sites stop looking at `.pid`/`.uid` directly. This also removes the "fake zeros" problem.
4. **The pid still needs a replacement as peer key and name seed.** The *application id* (stable, from configuration)
   is the natural replacement; for generation tracking use the process koid / a generation counter (6.5).

Example (full realm in [7.1](#71-capability-routing-for-one-service)):

```json5
// skeleton exposes one capability per consumer ...
capabilities: [
    { protocol: "wheelspeed.for_app_a", path: "/svc/wheelspeed.for_app_a" },
    { protocol: "wheelspeed.for_app_b", path: "/svc/wheelspeed.for_app_b" },
],
// ... the realm offers each to exactly one consumer, renamed
offer: [
    { protocol: "wheelspeed.for_app_a", from: "#skeleton", to: "#app_a", as: "score.mw.com.WheelSpeed" },
    { protocol: "wheelspeed.for_app_b", from: "#skeleton", to: "#app_b", as: "score.mw.com.WheelSpeed" },
],
// app_c gets no offer -> its connection fails (PEER_CLOSED on first use).
```

**Semantics to define:** `strict_permissions` on Fuchsia. Proposal: *strict* means "the realm must contain an offer per
configured allowed consumer and the skeleton rejects connections arriving on endpoints that are not in its configured
list" (defence in depth); *non-strict* behaves as today (all connected consumers allowed). A **build-time check**
compares the generated realm offers with the mw::com configuration.

**Safety implication.** The realm CML becomes a safety-relevant artifact (review, traceability, change control), just
like the mw::com configuration is today. It should be **generated** from the configuration (one source of truth).

### 6.5 Partial restart and crash recovery

Today's mechanism (flock'ed usage marker, skeleton pid in shared memory, transaction-log rollback, outdated-node
notification) assumes a shared filesystem and stable pids. The *behaviour* that must be preserved is:

* A skeleton restart must not corrupt proxies that still hold slots; stale slot references must be rolled back.
* A proxy restart must not leak slot references held by its previous incarnation.
* Proxies must learn that the skeleton restarted and re-subscribe.

Fuchsia building blocks and proposed mapping:

| Needed | Today | Fuchsia proposal |
|---|---|---|
| "Is anyone still using this instance?" (skeleton before cleanup) | exclusive `flock` attempt on usage marker | Each attached proxy holds a liveness **eventpair** end that the skeleton keeps the peer of. When all peers are closed, the instance is unused. (No file lock needed.) |
| "Did the skeleton die?" (proxy) | stale lock / pid | `PEER_CLOSED` on the liveness eventpair and on the `Endpoint` channel. |
| Identify skeleton incarnation | `skeleton_pid_` in `ServiceDataStorage` | **Generation** counter (e.g. process koid or monotonic count), written into the control VMO and returned in `Attach`. |
| Identify proxy incarnation | pid in `application_id_pid_mapping_` | application id + generation; stale transaction logs rolled back when a newer generation attaches. |
| Keep memory alive across a skeleton restart | memory outlives the process because it is a named object | A VMO lives as long as **any** handle exists. Either proxies keep their mapping/handles (they do), or a small **broker component** holds the VMOs so a *restarted skeleton* can re-adopt them and run the existing rollback. Without a broker the pragmatic fallback is "recreate shared memory on skeleton restart", which gives up the partial-restart feature for skeleton restarts. |

The broker is the largest piece of new design in this document. It is also the part with the most uncertainty
(see [risks](#12-risks-and-open-questions)). Phase 1 can ship without it (recreate-on-restart) while keeping the
interface for a broker.

**Open:** crash recovery for slots held by a dead proxy relies on the transaction log keyed by the application id;
this keeps working only if the application id is stable and unique per proxy. Requirements on configuration follow.

### 6.6 Notification path

Event *data* is in the VMO. Event *wake-ups* (receive handlers) travel over the existing control channel
(`message_passing`) or the proposed `EventChannel`. Rules:

* One-way, no ack: the publisher must never block on a slow subscriber.
* Coalescing is acceptable (a notification means "something new in slot ≥ n"), as today.
* Pure polling consumers (`GetNewSamples` without a handler) never use FIDL after `Attach`.
* Cost: one channel write per *notified* subscriber per notification. That is O(m) for m subscribers with handlers, but
  carries only a few bytes, versus O(m × payload) if the sample itself were sent.

### 6.7 Typed interfaces and IDL options

Three levels of typing for the *FIDL side*. The shared-memory payload types remain C++ types in all of them.

| Option | Description | Pro | Con |
|---|---|---|---|
| **A. Generic protocol** (recommended start) | One protocol (`Endpoint`) for all services; the service is data (`ServiceInstanceId`, layout). | One FIDL library; no generator. | CML capabilities are untyped w.r.t. service type unless a naming convention is used. |
| **B. Generic protocol + typed marker capability** | As A, plus an empty protocol per service interface used only for CML routing. | Typed routing and review at low cost. | Needs a small generator or hand-written markers. |
| **C. Fully typed per-service FIDL (FIDL-first)** | `.fidl` per service interface is the source of truth; a generator over the `fidlc` JSON IR emits mw::com C++ types, FIDL, CML and layout constants. Payloads restricted to fixed-layout, trivially copyable types. | One source of truth; strongest checking. | Needs a generator; FIDL events (messages) vs mw::com events (shared slots) is a semantic mismatch carried by attributes; Linux/QNX need a non-FIDL C++ backend; migration of existing interfaces. |

FIDL-generated *wire* C++ types for primitive-only structs may be directly usable as payloads on Fuchsia, but
generated plain structs are the safer default for multi-OS portability (the FIDL C++ bindings exist only in the
Fuchsia SDK and FIDL does not guarantee trivially-copyable layouts of its generated C++).

### 6.8 Realm composition and scaling

* The realm CML is **generated** from the mw::com configuration (one capability and one offer per allowed
  consumer and instance, plus discovery routes). Output must be stable, sorted and diffable; a build test compares it
  with the configuration.
* The number of offers is a **consumers × endpoints** matrix: e.g. 30 apps × 50 services ≈ 1,500 offers. Runtime
  connection counts (thousands of channels, VMO handles) are not the concern, because routing happens at `Attach`
  time and the hot path never touches FIDL.
* Reduction techniques: subrealms per subsystem, one capability per consumer per skeleton component with the instance
  selected in `Attach` (moves part of the check into code), dictionaries. **Service-instance filtering is not
  available** in CML on the pinned SDK (experiment A.2). Measured scale: no limit hit up to 10,000 offers, and
  steady-state connect cost does not depend on the number of offers (experiment A.3). Details in
  [Appendix A](#a-experiment-results).

---

## 7. End-to-end examples

### 7.1 Capability routing for one service

One skeleton (`wheelspeed_skeleton`), apps A and B allowed, app C not allowed. *(Illustrative; the syntax is checked
by the experiment in [Appendix A](#a-experiment-results).)*

```json5
// wheelspeed_skeleton.cml
{
    program: { runner: "elf", binary: "bin/skeleton" },
    capabilities: [
        { protocol: "wheelspeed.for_app_a", path: "/svc/wheelspeed.for_app_a" },
        { protocol: "wheelspeed.for_app_b", path: "/svc/wheelspeed.for_app_b" },
    ],
    use: [ { protocol: "score.mw.com.ServiceDiscovery" } ],
    expose: [
        { protocol: "wheelspeed.for_app_a", from: "self" },
        { protocol: "wheelspeed.for_app_b", from: "self" },
    ],
}
```

```json5
// app_a.cml (app_b.cml, app_c.cml identical)
{
    program: { runner: "elf", binary: "bin/app" },
    use: [
        { protocol: "score.mw.com.WheelSpeed" },
        { protocol: "score.mw.com.ServiceDiscovery" },
    ],
}
```

```json5
// realm.cml
{
    children: [
        { name: "discovery",           url: "fuchsia-pkg://example/discovery#meta/discovery.cm" },
        { name: "wheelspeed_skeleton", url: "fuchsia-pkg://example/wheelspeed#meta/skeleton.cm" },
        { name: "app_a", url: "fuchsia-pkg://example/app_a#meta/app.cm" },
        { name: "app_b", url: "fuchsia-pkg://example/app_b#meta/app.cm" },
        { name: "app_c", url: "fuchsia-pkg://example/app_c#meta/app.cm" },
    ],
    offer: [
        { protocol: "wheelspeed.for_app_a", from: "#wheelspeed_skeleton", to: "#app_a",
          as: "score.mw.com.WheelSpeed" },
        { protocol: "wheelspeed.for_app_b", from: "#wheelspeed_skeleton", to: "#app_b",
          as: "score.mw.com.WheelSpeed" },
        // nothing is offered to app_c
        { protocol: "score.mw.com.ServiceDiscovery", from: "#discovery",
          to: [ "#wheelspeed_skeleton", "#app_a", "#app_b", "#app_c" ] },
    ],
}
```

Result: A and B open `score.mw.com.WheelSpeed` and arrive on *different* skeleton endpoints; C cannot reach the
skeleton (its first call fails with `PEER_CLOSED`, see A.1). The `offer` list *is* the `allowed_consumer` list.
The measured experiment confirmed this routing pattern; `as` must be used with exactly **one** protocol per `offer`
entry (A.1).

### 7.2 Offer → find → attach → read a sample

```text
 skeleton              discovery               proxy                    kernel / VMO
    │ create VMOs (data, control)                                             │
    │ Offer(id, endpoint_for_A) ──►│                                          │
    │                              │◄── Find/Watch(id) ───────────────────────│ proxy
    │                              │──── endpoint_for_A ────────────────────►│
    │◄──────────────────────────────── Attach(app_id=A, events) ─────────────│
    │ identity := "A" (from endpoint path)
    │ check A allowed for this instance
    │ dup(data, R|M|T), dup(control, R|W|M|T), generation, liveness
    │────────────────────────── reply: handles + layout ────────────────────►│
    │                                                  map data (ro), control (rw)
    │ AllocateNextSlot; write sample; EventReady (atomics)                    │
    │──────────── OnEvent(slot, seq)  (only if handler registered) ──────────►│
    │                                                  read slot IN PLACE; CAS refcount
```

### 7.3 Skeleton crash and restart (with broker)

```text
 skeleton#1 dies ──► peer-closed on liveness eventpairs ──► proxies: state "service unavailable"
 broker still holds data/control VMOs (and the transaction logs inside)
 skeleton#2 starts ──► adopts VMOs from broker ──► generation++ ──► rolls back stale transaction logs
 skeleton#2 Offer(...) ──► discovery ──► proxies re-Attach / OnSkeletonRestart(generation) ──► re-subscribe
 (without a broker: skeleton#2 creates fresh VMOs; proxies re-attach to the new memory;
  slot references in the old memory are simply discarded)
```

### 7.4 What an application sees

Nothing changes in application code (illustrative, API names abbreviated):

```cpp
// provider component
auto skeleton = WheelSpeedSkeleton::Create(InstanceSpecifier::Create("app/wheelspeed").value()).value();
auto slot = skeleton.speed.Allocate().value();   // slot in the VMO
skeleton.OfferService();

// consumer component
auto handles = WheelSpeedProxy::FindService(InstanceSpecifier::Create("app/wheelspeed").value()).value();
auto proxy = WheelSpeedProxy::Create(handles.front()).value();
proxy.speed.Subscribe(4);
proxy.speed.GetNewSamples([](auto sample) { /* zero-copy read from the VMO */ }, 4);
```

The instance specifier → deployment mapping (configuration) additionally selects the Fuchsia binding parameters.

---

## 8. Alternatives considered

| # | Alternative | Summary | Verdict |
|---|---|---|---|
| **R** | **Recommended**: LoLa data plane on VMOs, FIDL control plane, capability-based access control | See above. | ✔ |
| **P** | **Pure FIDL transport** (samples sent as FIDL messages, no shared memory) | Idiomatic Fuchsia. Subscribers receive copies. | ✘ for LoLa's use cases: O(m) copies per sample, syscalls, 64 KiB message limit, no zero-copy. Acceptable only for small low-rate data. |
| **H** | **Hybrid with VMO-per-sample** | A new/child VMO per message. | ✘ allocation/mapping per sample; only for rare, large one-shot transfers. |
| **E** | **POSIX emulation** (keep `shm_open` + flock + files, supply a POSIX layer) | Minimal code change if the platform libc offers it. | Not viable to rely on: no global shm namespace/shared filesystem across components; breaks the isolation model; also keeps pid/uid assumptions. (Unless using a Linux-compat layer such as Starnix — a different product decision, not evaluated here.) |
| **B** | **Broker-owned memory** (a broker creates and owns all VMOs; skeleton asks it) | Strong lifetime model, easy partial restart. | Optional add-on, not a replacement. Extra component in the safety path. |

---

## 9. Pros and cons of the recommended concept

### Pros

* **Performance preserved.** Hot path is zero-copy and syscall-free; FIDL cost is only at attach and for
  notifications. Fan-out cost to m subscribers is O(1) for data. Measured on FEMU (A.4): publisher cost per sample
  was independent of m for the VMO (0.1 µs at 64 B, about 16 µs at 60 KiB, which is the `memcpy`), versus 80 µs
  (64 B) to 7.5 ms (60 KiB) for FIDL with 8 subscribers.
* **Maximum reuse.** Slot logic, subscription state machine, transaction logs, local views, generic layer and the
  configuration model stay. The OS-specific surface is small and already has seams.
* **Platform-native security.** Access control is enforced by the component framework, not by code that can have a bug.
  Handle rights give a read-only data view without ACLs.
* **No filesystem coupling.** No `/tmp` flag files, no `flock`, no inotify, no pid reuse races.
* **Crash detection is a first-class OS signal** (`PEER_CLOSED`), simpler and more robust than lock probing.
* **Incremental.** Seam 4 (messaging) is already done and tested on FEMU. Each further seam can be delivered and
  tested separately.
* **Reviewable policy.** The realm is a single declarative artifact describing who may talk to whom.

### Cons / costs

* **Semantic shift for identity.** pid/uid disappear; `ClientIdentity`, `allowed_consumer` and `strict_permissions`
  need redefinition and an opaque identity type. Touches code outside the Fuchsia directory.
* **Safety artifact moves to CML.** The realm must be generated, reviewed and qualified; a mismatch between realm and
  mw::com configuration is a new failure mode.
* **Partial restart needs new design** (liveness eventpairs, generation, optionally a broker). The broker adds a
  component and a new failure mode; without it skeleton restarts lose partial restart.
* **Control VMO is writable by consumers**, as on Linux. A misbehaving proxy can corrupt control state. Same trust
  model as today, but it is not improved by Fuchsia.
* **Two OS-specific mechanisms to maintain** for memory (POSIX shm + VMO) behind one interface; more test matrix.
* **Routing size.** consumers × endpoints offers; needs a generator and subrealm structure.
* **New tooling dependency** (Fuchsia SDK, FEMU, `cmc`/`fidlc`, product bundle) in CI; no Fuchsia CI job exists yet.
* **Unverified assumptions** (see sections 12 and 13).

### Trade-off summary against the main alternative (pure FIDL)

| | Recommended (VMO + FIDL) | Pure FIDL |
|---|---|---|
| Publisher cost per sample | independent of subscriber count | one channel write per subscriber |
| Subscriber read | in place, no syscall | kernel copy + decode |
| Max payload | slot size | 64 KiB per message |
| Complexity | higher (VMO lifecycle, restart) | lower |
| Isolation | consumers write control VMO | no shared writable memory |
| Reuse of LoLa | high | low (new binding) |

---

## 10. Impact on the current implementation

Legend: **U** unchanged · **A** needs an abstraction/refactoring (benefits Linux/QNX too) · **N** new Fuchsia code ·
**C** configuration/documentation.

### 10.1 By component

| Area (path) | Impact | What changes |
|---|---|---|
| `score/message_passing/fuchsia/` | **N (done)** | Backend, `Connector` FIDL, factories, echo tests. Remaining: identity strategy, backend-neutral gtests, CI. |
| `score/message_passing/{engine,client_factory,server_factory}.h`, `BUILD` | **U/A (done)** | Backend selection. |
| `score/message_passing/unix_domain/` | **U** | Stays Linux-only. |
| `score/message_passing` identity API (`GetClientIdentity`, `ClientIdentity{pid,uid,gid}`) | **A** | Make `ClientIdentity` opaque (equality, hash, log string, `IsAllowed`). Linux/QNX implementations wrap pid/uid/gid. Changes callers in LoLa messaging. |
| `score/memory/shared/` (`SharedMemoryResource`, `SharedMemoryFactory`, `LockFile`, `FlockMutex`) | **A + N** | Introduce/confirm an OS-neutral creation/open interface (`ISharedMemoryFactory` exists). Add a VMO-backed `SharedMemoryResource` variant created from a handle instead of a path. Create-or-open and lock-file logic must not be required on the VMO path. `UserPermissions` maps to handle rights. |
| `bindings/lola/skeleton_memory_manager.*`, `skeleton.cpp`, `proxy.cpp` (shm open/create) | **A** | Stop assuming "open by name"; go through an attach-style acquisition API that the POSIX implementation satisfies by name-opening and the Fuchsia implementation by handle exchange. |
| `bindings/lola/{partial_restart_path_builder,i_partial_restart_path_builder}.*`, usage marker + `flock` in `skeleton.cpp`/`proxy.cpp`, `service_data_storage.*`, `application_id_pid_mapping*`, `transaction_log_rollback_executor.*` | **A + N** | Introduce a *peer liveness / instance usage* interface with the current file+flock implementation and a Fuchsia implementation (eventpair + generation). `skeleton_pid_` becomes a generation/incarnation id. `application_id_pid_mapping` becomes `application_id → incarnation`. Rollback logic itself unchanged. |
| `bindings/lola/service_discovery/` (`FlagFileCrawler`, inotify) | **A + N** | Reuse `IServiceDiscoveryClient`; add a FIDL-based client. Flag-file code stays for Linux/QNX. |
| `bindings/lola/messaging/` (`CreateMessagePassingName(asil, pid)`, `sender_pid`, allowed-uid checks) | **A** | Replace pid with the opaque node/peer id; allowed-consumer check goes through `ClientIdentity::IsAllowed`. Naming seeded by application id instead of pid on Fuchsia. |
| `bindings/lola/runtime.cpp` (runtime key = pid/uid) | **A** | Runtime key from application id / incarnation. |
| `bindings/lola/{event_data_control,event_slot_status,slot_collector,…}`, subscription state machine, local views, transaction log data structures | **U** | Atomics in shared memory; unaffected. |
| `impl/*_binding.h`, `impl/plumbing/*`, `BindingType` | **U (options A/B) / A (option C)** | With the generic-protocol recommendation no new `BindingType` is needed: Fuchsia is a *platform* variant of the LoLa binding. A separate binding type would only be needed if the data plane stopped being LoLa. |
| `impl/configuration/` (`allowed_consumer`, `allowed_provider`, `strict_permissions`, shm sizes) | **C + A** | Redefine meaning of consumer entries as *application ids*; define `strict_permissions` on Fuchsia; add deployment fields for realm generation. Parsing largely unchanged. |
| Realm/CML generation (new tool) | **N** | Generate capabilities, offers and discovery routes from configuration; build-time consistency test. |
| Broker component | **N (phase 6)** | Holds VMOs for partial restart. |
| `bindings/lola/BUILD`, `score/memory/shared/BUILD` | **A** | `select()` on `@platforms//os:fuchsia` for the Fuchsia seam implementations; Fuchsia packages (`fuchsia_package`). |
| Tests | **A + N** | Many LoLa component tests are Linux-only; need Fuchsia ITF variants or backend-neutral tests. Add liveness/identity/shm-provider unit tests with mocks. New FEMU integration tests per seam. |
| CI | **N** | Fuchsia job modelled on `build_and_test_qnx.yml` (FEMU instead of QEMU+QNX). None exists today. |
| Docs, safety analysis, requirements | **C** | New platform description; update FMEA for the new failure modes (realm mismatch, broker, VMO handle leaks); trace identity changes. |

### 10.2 What stays untouched (important for risk)

The *behaviour-critical* algorithms — slot reservation/reference counting, subscription state machine, transaction-log
rollback logic, local views and the generic proxy/skeleton framework — remain as they are. All changes are at the
edges where LoLa meets the OS.

### 10.3 Behavioural differences visible to integrators

* "Allowed consumer" is configured as an **application identifier** (and a realm route), not a uid.
* Shared memory is **not visible** in a filesystem; debugging tools that list `/dev/shm` do not apply.
* A consumer without a route is **not** rejected at startup: the component starts and the first FIDL call fails with
  `ZX_ERR_PEER_CLOSED` (measured, A.1). The LoLa client must map this to "service not available / not allowed".
* A skeleton restart may drop partial-restart state if no broker is deployed.

---

## 11. Migration plan

| Phase | Deliverable | Depends on | Verification |
|---|---|---|---|
| **0 (done)** | Fuchsia SDK/FEMU in ITF; baselibs compile; message_passing Fuchsia backend with FIDL `Connector`; echo tests | – | FEMU ITF tests (in-process + cross-component) |
| **1** | Identity: opaque `ClientIdentity`; Fuchsia identity from endpoint; define `strict_permissions` | 0 | Unit tests (Linux/QNX unchanged); FEMU test with 2 consumers + 1 unrouted |
| **2** | Shared-memory provider seam: VMO-backed `SharedMemoryResource`; `Attach` protocol; skeleton/proxy use attach-style acquisition on all platforms | 1 | Existing shm/LoLa tests on Linux; new FEMU test: write in skeleton, read in proxy |
| **3** | Discovery seam: FIDL discovery behind `IServiceDiscoveryClient` | 2 | Discovery FEMU tests; Linux crawler tests unchanged |
| **4** | Liveness/partial-restart seam: eventpair liveness, generation; recreate-on-restart fallback | 2, 3 | Crash/restart FEMU tests (kill skeleton / proxy) |
| **5** | Realm generator + consistency test; subrealm structure | 1 | Build test; scale test |
| **6** | Broker for VMO retention (partial restart parity) | 4 | Skeleton restart test with slot rollback |
| **7** | Fuchsia CI job; backend-neutral gtest suite; safety documentation | all | CI green on FEMU |

Phases 1–4 are the critical path for a working "hello LoLa" on Fuchsia.

---

## 12. Risks and open questions

| # | Item | Risk / question | Mitigation |
|---|---|---|---|
| 1 | **`shm`/`mmap`/`flock`/`fcntl` availability in the Fuchsia libc** | Unverified whether `score/memory/shared` and the OS wrappers compile on Fuchsia as is. | Spike: build `score/memory/shared` for Fuchsia and list missing symbols. |
| 2 | **Writable control VMO for consumers** | A faulty proxy can corrupt control state (as on Linux). | Keep current trust model for now; analyse in FMEA. Optional later: split per-consumer control areas. |
| 3 | **Broker design** | New component; who restarts it; what if it dies? | Phase it; fallback is recreate-on-restart. |
| 4 | **Routing cost at scale** | Measured: fine up to 10,000 offers (A.3). Remaining: compiled CML grows to MBs (100 apps × 100 offers: 2.7 MB root + 3.1 MB provider manifest). | Subrealms; generator; keep one run per release as regression. |
| 11 | **Unrouted `use` is detected late** | Connect succeeds, first call fails with `PEER_CLOSED`; no start failure (A.1). | Treat as "not offered"; add a startup self-check in the realm test. |
| 12 | **No instance filtering in CML** | One service capability exposes all instances to every consumer (A.2). | Per-consumer protocol capabilities (this design) or one provider component per instance. Dynamic offers via `Realm.CreateChild` untested. |
| 5 | **`strict_permissions` meaning** | Needs a decision with safety/requirements owners. | Proposal in 6.4. |
| 6 | **Stable application ids** | Transaction-log rollback relies on them; must be unique and stable across restarts. | Make a configuration requirement; validate at attach. |
| 7 | **Single FIDL async loop thread in the registry** | Throughput and failure isolation of the `Connector` service. | Measure; keep control traffic small. |
| 8 | **ASIL aspects** | Component framework and FIDL stack are not qualified by us; arguments for freedom from interference. | Safety concept for the platform layer is out of scope here. |
| 9 | **Messaging identifiers** | Name seeds currently use pid. | Replace with application id (phase 1). |
| 10 | **CI infrastructure** | Emulator, SDK size, caching, KVM availability. | Dedicated job; cache the hermetic SDK. |

---

## 13. Status of this document: what is verified and what is not

**Implemented and verified (committed on `feat/fuchsia-femu-integration-test`)**

* Hermetic Fuchsia SDK and product bundle, FEMU ITF plugin and hello-world test.
* `score_baselibs` compiles for Fuchsia (patch `third_party/score_baselibs/fuchsia_support.patch`).
* `message_passing` Fuchsia backend over FIDL, selected by platform; in-process and cross-component echo tests pass
  on FEMU; Linux build and tests unaffected.

**Design only (not built, not measured)**

* VMO-backed shared memory, `Attach`/discovery protocols, identity via endpoint capabilities, liveness/generation,
  broker, realm generation, typed IDL options.
* All CML and FIDL snippets in this document are illustrative sketches unless Appendix A says otherwise.

**Experiments** run on FEMU (branch `exp/fuchsia-cml-routing`) and reported in [Appendix A](#a-experiment-results):
per-consumer capability routing with `as` (works, with the late-failure behaviour noted), service-instance filtering
(not available in CML on the pinned SDK), routing scale up to 10,000 offers, and a FIDL-versus-VMO fan-out benchmark.

---

## 14. Glossary

| Term | Meaning |
|---|---|
| **LoLa** | The shared-memory `mw::com` binding. |
| **Skeleton / Proxy** | Provider-side / consumer-side object of a service interface. |
| **Slot** | One sample cell of an event in the data VMO. |
| **FIDL** | Fuchsia Interface Definition Language. |
| **CML** | Component Manifest Language (`.cml`, compiled by `cmc`). |
| **VMO** | Virtual Memory Object. |
| **Realm** | A component subtree with a parent that composes children and routes capabilities. |
| **Capability-as-identity** | One capability per consumer; the endpoint used identifies the consumer. |
| **Generation** | Incarnation counter of a skeleton or proxy; changes on every restart. |
| **Broker** | Optional component that holds VMOs so a restarted skeleton can re-adopt them. |
| **FEMU** | Fuchsia emulator (QEMU-based) used for integration tests. |
| **ITF** | The repo's integration test framework. |

---

## A. Experiment results

Measured on FEMU (SDK 33.20260919, KVM, `ffx test run`). Code and raw logs are in the experiment branch
`exp/fuchsia-cml-routing` under `experiments/fuchsia_cml/` (commits `023b6c77`, `42a73b23`). Absolute times come
from an emulated guest and are indicative only; the trends are the result. Each cell is a single run, no confidence
intervals.

### A.1 Per-consumer capabilities renamed with `as` (works)

Verified CML:

```json5
// provider
capabilities: [ { protocol: ["svc.for_app_a", "svc.for_app_b"] } ],
expose: [ { protocol: ["svc.for_app_a", "svc.for_app_b"], from: "self" } ],

// realm
offer: [
    { protocol: "svc.for_app_a", from: "#provider", as: "svc.consumer", to: "#app_a" },
    { protocol: "svc.for_app_b", from: "#provider", as: "svc.consumer", to: "#app_b" },
    // app_c, app_d: nothing
],

// consumer
use: [ { protocol: "svc.consumer" } ],
```

The provider serves each protocol at its own path (`AddUnmanagedProtocol(handler, "svc.for_app_a")`).

| Question | Observation |
|---|---|
| Does the provider know the caller? | **Yes.** app_a's request arrives on `svc.for_app_a`, app_b's on `svc.for_app_b`. The endpoint is the identity. |
| Can app_a reach app_b's endpoint? | No: `ZX_ERR_PEER_CLOSED`. |
| What happens to app_c (no offer, required `use`)? | **The component still starts.** `component::Connect` returns OK (it is asynchronous), the first FIDL call fails with `ZX_ERR_PEER_CLOSED (-24)`, and the framework logs `cannot use protocol ... because parent does not offer the capability`. Detection happens only at first use. Not tested: start through non-eager `ffx component start`. |
| `availability: "optional"` for a protocol? | Same behaviour as required (entry present in `/svc`, `PEER_CLOSED`). For a *service* capability, no `/svc/<service>` entry exists (`ENOENT`). |
| cmc rules found | `as` is only allowed with exactly one protocol per `offer`. Two offers with the same target name to the same child fail to compile (misleading error). So one `offer` per (consumer, endpoint). |

Consequence for the design: capability-as-identity works as sketched. The Fuchsia LoLa client must treat
`PEER_CLOSED` on first use as "not available / not allowed", and the realm test should check that every configured
consumer is actually routed.

### A.2 Service-instance filtering (not available)

cmc on the pinned SDK rejects `source_instance_filter`, `renamed_instances` and `filter` in `offer` and `expose`
(`unknown field ...`). The same SDK's FIDL declarations contain these fields, so they might be reachable through
dynamic offers via `fuchsia.component.Realm.CreateChild`; **not tested**. With a plain `service` offer, a consumer
sees **all** instances. So "consumer X may only see instance Y" cannot be expressed with a static service offer.
Workarounds: one protocol capability per consumer (A.1, our design) or one provider component per instance.

### A.3 Scale (N apps × M per-consumer capabilities)

All connections verified by identity (no errors, no identity mismatches). Times in guest monotonic time.

| config | offers | first connect+ping, mean / max over apps (µs) | repeated connect+ping, mean / p99 (µs) | compiled CML root / provider |
|---|---|---|---|---|
| 1×1 | 1 | 441 / 441 | 112 / 333 (1 app) | 3 KB |
| 30×20 | 600 | 2,430 / 3,476 | 151 / 365 (1 app) | 182 KB / 179 KB |
| 30×50 | 1,500 | 2,401 / 4,586 | 143 / 357 (1 app) | 417 KB / 452 KB |
| 30×100 | 3,000 | 2,091 / 4,466 | 137 / 319 (1 app) | – |
| 100×50 | 5,000 | 5,621 / 9,530 | 172 / 413 (1 app) | – |
| 100×100 | 10,000 | 8,936 / 20,465 | 172 / 460 (1 app) | 2.7 MB / 3.1 MB |

* **No limit was hit** up to 10,000 offers (no cmc or component-manager error).
* **Realm size barely affects end-to-end boot** (2.3 s → 3.8 s `ffx` wall time, of which about 2 s is fixed overhead).
* **Steady-state connect+ping stays at about 110–170 µs** regardless of the number of offers. Routing is therefore
  not on the hot path, consistent with the design (route once at `Attach`).
* The first connect grows with realm size and concurrent component starts (0.4 ms at 1×1, 5–11 ms with 100 apps
  starting concurrently).
* With 30 apps looping concurrently, the per-connect cost rises to about 1.3 ms (contention on the emulated machine),
  not because of the number of offers.
* Compiled manifests become large (MBs at 10,000 offers), which supports generating the realm and splitting it into
  subrealms.

### A.4 Fan-out benchmark: FIDL messages vs shared VMO

Publisher plus 8 subscriber components (separate processes), FEMU with 12 vCPUs, 2,000 samples at 1 ms pacing.
FIDL: one one-way message per subscriber with the payload in a `vector<uint8>`. VMO: one VMO mapped by all, the
publisher writes the payload once and then does an atomic release-store of the sequence number; subscribers spin
on it. All values in µs, mean / p99.

| payload | m | FIDL publisher cost | FIDL end-to-end latency | VMO publisher cost | VMO end-to-end latency |
|---|---|---|---|---|---|
| 64 B | 1 | 12.2 / 17.8 | 99.9 / 205.7 | 0.11 / 0.25 | 0.82 / 0.87 |
| 64 B | 8 | 80.1 / 92.4 | 66.7 / 113.5 | 0.12 / 0.21 | 0.70 / 1.3 |
| 4 KiB | 1 | 66.6 / 76.0 | 64.0 / 118.5 | 0.19 / 0.41 | 1.4 / 1.6 |
| 4 KiB | 8 | 509.2 / 570.4 | 284.4 / 543.2 | 0.21 / 1.25 | 3.0 / 3.4 |
| 60 KiB | 1 | 873.4 / 1162 | 561.6 / 735.2 | 5.5 / 6.3 | 12.7 / 14.2 |
| 60 KiB | 8 | 7502.6 / 10422 | 3912 / 9775 | 15.7 / 19.4 | 24.6 / 31.9 |

(Full table with m = 2 and 4 in the experiment report.)

* **FIDL publisher cost is linear in subscribers and payload**: about 10 µs per subscriber at 64 B, about 60 µs at
  4 KiB, about 0.9 ms at 60 KiB. At 60 KiB with two or more subscribers it exceeds the 1 ms period, so the publisher
  cannot sustain 1 kHz.
* **VMO publisher cost is independent of the subscriber count**; for 60 KiB it is just the `memcpy`. It is about
  100× lower at 64 B and about 500× lower at 60 KiB with 8 subscribers.
* No drops in either variant.
* The VMO variant used one **busy-polling thread per subscriber**. Real designs need a notification mechanism for
  non-polling subscribers, whose cost is not included. With fewer vCPUs than pollers the latency would degrade
  (not measured).
* The FIDL latency at m = 1 / 64 B is higher than at m = 2 and is considered first-use/scheduling noise (single run).
* Caveat: emulated guest, shared host, single run per cell. Trust the trends (linear for FIDL, flat for VMO), not
  the absolute numbers.

### A.5 Not tested

* Dynamic offers with instance filtering via `Realm.CreateChild`.
* Non-eager start of a component with an unrouted required `use`.
* Scale tests with all apps looping beyond 30 apps; repeated runs for variance.
* The notification path for non-polling VMO subscribers.
* Anything about the VMO-backed `SharedMemoryResource`, `Attach`, discovery, liveness and the broker (design only).

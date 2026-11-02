# LoLa on Fuchsia — Architecture Concept

| | |
|---|---|
| **Status** | Draft / proposal (RFC). Parts are implemented and verified on branches that are not yet merged or pushed, parts are design only. See [Status of this document](#13-status-of-this-document-what-is-verified-and-what-is-not) and [Appendix D](#d-branches-and-commits). |
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
16. [Appendix B: FMEA summary](#b-fmea-summary)
17. [Appendix C: Test matrix summary](#c-test-matrix-summary)
18. [Appendix D: Branches and commits](#d-branches-and-commits)

---

## 1. Purpose and summary

LoLa moves event/field samples between processes with **zero copy through shared memory** and uses a small amount of
**message passing** for control (subscription, notification registration, method calls). Today both mechanisms are
implemented on top of POSIX (Linux) and QNX primitives.

Fuchsia has neither POSIX shared-memory objects with a global namespace (`shm_open` fails with `ENOENT` in a component,
measured on FEMU, see [section 13](#13-status-of-this-document-what-is-verified-and-what-is-not)), nor working `flock` (`ENOTSUP`), nor process credentials
(pid/uid) on its IPC channels. Instead it offers:

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
 Liveness /       │ flock'ed marker files, pid, │      │ broker leases (process death),  │
 partial restart  │ /tmp flag files, inotify    │      │ data-VMO epoch, peer-closed     │
                  └─────────────────────────────┘      └─────────────────────────────────┘
```

---

## 2. LoLa today: what is OS-specific?

The binding-independent `mw::com::impl` layer talks to a binding through the interfaces `ProxyBinding`,
`SkeletonBinding`, `ProxyEventBinding`, `SkeletonEventBinding` and `IBindingRuntime`
(`score/mw/com/impl/*_binding.h`, `i_binding_runtime.h`). `BindingType` currently knows `kLoLa`, `kFake`, `kSomeIp`.
The LoLa binding is in `score/mw/com/impl/bindings/lola/`.

The following table lists every place where LoLa touches OS-specific functionality, and what that means on Fuchsia.
It is the basis for [section 10](#10-impact-on-the-current-implementation).

| Concern | How it works today | Where | Fuchsia problem |
|---|---|---|---|
| **Data/control shared memory** | Named shared-memory objects (`data`, `control` per QoS level, optional `method` channel) created via `score::memory::shared::SharedMemoryFactory` / `SharedMemoryResource`, which uses `shm_open`, `ftruncate`, `mmap`, `fchmod`, a lock file for create-or-open, and typed memory on QNX (only when tracing registers a callback). | `bindings/lola/skeleton_memory_manager.cpp`, `proxy.cpp`; `score/memory/shared/shared_memory_resource.cpp` | No global shm namespace: `shm_open` compiles but fails at runtime (`ENOENT`, [status](#13-status-of-this-document-what-is-verified-and-what-is-not)). Replaced by VMOs that are *handed over*. |
| **Shm access rights** | `os::Acl` / file mode bits: the *allowed-consumer uids* get READ on data and READ+WRITE on control (strict, or world-accessible if the list is empty and not strict); the skeleton uid is used for the method shm. | `skeleton_memory_manager.cpp`, `proxy.cpp` (~L772, `GetSkeletonShmPermissions`) | No uids, no ACLs (shim returns `ENOTSUP`). Rights are properties of VMO handles. |
| **Partial restart / usage detection** | Two flock'ed marker files per instance under `/tmp/mw_com_lola/partial_restart/`: `existence-*` and `usage-*`. The skeleton takes an **exclusive `flock`** on both; proxies hold a **shared `flock`** on the usage marker. `DetermineShmReuseStrategy` has three outcomes: lock obtained, so no proxy uses the old memory, so **recreate** (`kRecreateShm`); lock not obtained, so proxies still use it, so **reuse** the existing memory and run `CleanupSharedMemoryAfterCrash` (`kReuseExistingShm`, the real partial-restart path); and `kOpenGatewayForwardedShm`. The skeleton pid is kept in `ServiceDataStorage` (data segment, read-only for proxies); transaction logs are rolled back by the *restarting proxy* per application id. | `skeleton.cpp`, `proxy.cpp`, `partial_restart_path_builder.cpp`, `service_data_storage.cpp`, `transaction_log_rollback_executor.cpp` | No shared filesystem; `flock` fails with `ENOTSUP`. The kernel drops a flock when the *process* dies, a Fuchsia handle close is weaker (see [6.5](#65-partial-restart-and-crash-recovery)). |
| **Service discovery** | *Flag files* named `<pid>_<asil-qm\|asil-b>_<disambiguator>` under `/tmp/mw_com_lola/service_discovery/<service_id>/<instance>/`, watched with **inotify** by `FlagFileCrawler` / `ServiceDiscoveryClient` (worker thread). | `bindings/lola/service_discovery/` (`flag_file.cpp`) | No shared directory, no inotify; the pid-named files also need a pid-free design. Needs a discovery service. The seam `IServiceDiscoveryClient` already exists. |
| **Control messaging** | `score/message_passing` (unix-domain sockets, or QNX dispatch). Server names derived from `CreateMessagePassingName(asil, pid)`. | `bindings/lola/messaging/` | **Done** for the transport (see [6.1](#61-control-plane-message_passing-over-fidl-implemented)); identity and pid addressing still open. |
| **Peer identity** | The pid of a message-passing peer is the routing key (`node id`), stored as connection user data, used as `sender_pid` and to derive server names. The uid is checked against the per-instance `allowed_consumer` lists (`strict_permissions`). The runtime key is the configured application id (fallback: uid); rollback uses `TransactionLogId{application id}`. | `messaging/message_passing_service_instance.cpp`, `skeleton.cpp` (`GetAllowedConsumers`), `runtime.cpp` | Fuchsia channels carry **no credentials**. The backend reports `{0,0,0}` unless the opaque identity of [6.4](#64-identity-and-access-control) is used. |
| **Build** | Only `score/message_passing/BUILD` has a Fuchsia `select`. `score/memory/shared/BUILD` selects only linux/qnx (the spike compiles it for Fuchsia with patches, see status), LoLa's own BUILD has no `@platforms//os:fuchsia` handling. Component tests are Linux-only. | `bindings/lola/BUILD`, `bindings/lola/test/BUILD` | New Fuchsia targets and packages. |

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
 │   (new)       │ + epoch, lease │   discovery (new)│   (implemented)      │
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
 │  ┌────────────┐  FIDL (registry)    ┌──────────────────┐                                │
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

In the baseline the control VMO is mapped read/write by consumers; see [6.2.1](#621-trust-model-and-freedom-from-interference).
Each proxy reaches the skeleton through its own routed endpoint; discovery only reports availability ([6.3](#63-attach-and-service-discovery)).

---

## 6. Detailed design

### 6.1 Control plane: `message_passing` over FIDL (implemented)

`score/message_passing` now has a Fuchsia backend, **separate from `unix_domain`**:

* `score/message_passing/fuchsia/` — `FuchsiaEngine`, `FuchsiaServer`, client/server factories.
* Selected via `#elif defined(__Fuchsia__)` in `engine.h`, `client_factory.h`, `server_factory.h` and via a
  `select()` on `@platforms//os:fuchsia` in `score/message_passing/BUILD` (`unix_domain` is the default branch; Fuchsia
  and QNX override it).
* There is no usable `AF_UNIX` for this backend (not relied on; the SDK headers declare `sys/un.h`, runtime behaviour
  was not probed). A client creates a **Zircon stream socket pair** and sends one end to the server through the
  FIDL protocol `score.messagepassing.Connector.Connect(identifier, socket)`
  (`fuchsia/connector.fidl`). After that, framing (code + 16-bit size + payload) runs over the socket and the
  descriptors are polled exactly like the other backends.
* A `FuchsiaConnectionRegistry` (leaked singleton running an `async::Loop` thread) hosts the `Connector` service and
  keeps the in-process shortcut when client and server live in the same process.
* Verified on FEMU: in-process echo and cross-component echo (separate components, routed capability).

Message passing remains the transport for LoLa's *control* traffic only: subscription, notification registration,
method calls. It is **not** used for samples.

Hardening follow-ups (committed on separate branches, not yet merged into this document's base): `fix/fuchsia-registry-start`
(`ea8396f9`, a failing registry start is reported, not swallowed); `fix/fuchsia-engine-writes` (`645b2a88`, non-blocking
serialized writes through a bounded queue; when full the message is dropped and the sender sees `EAGAIN`, which callers must
treat as "not delivered", risk 10); `feat/opaque-client-identity` (`400bc46b`, known issue: `FromEndpoint` conflates node
id and principal id, 6.5 / PR 1).

Without the opaque identity the server cannot learn the peer, and `GetClientIdentity()` returns `{0,0,0}`. See 6.4.

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

1. **One data VMO and one control VMO per service instance** (and ASIL level), mirroring today's objects. Rights are
   part of the design, not an implementation detail. The skeleton (or broker) keeps the original handles with full
   rights and sends only duplicates with *reduced* rights ([6.2.2](#622-handle-rights-normative-and-the-tests-that-prove-them)):
   * **Data VMO**: skeleton writes; consumers hold `READ | MAP | GET_PROPERTY` (no `WRITE`, `EXECUTE`, `DUPLICATE`,
     `TRANSFER`). It also holds the skeleton **epoch** (rule 5), because proxies map it read-only and cannot forge it.
   * **Control VMO**: proxies **must write** (slot reference counts, subscription state, transaction logs), so in the
     baseline they hold `READ | WRITE | MAP | GET_PROPERTY` (no `EXECUTE`, `DUPLICATE`, `TRANSFER`). This is the
     *per-ASIL-level* trust model of Linux/QNX (per-uid file ACLs on a per-ASIL control object); it protects the ASIL-B
     control VMO from QM consumers but **not one consumer of a level from another** ([6.2.1](#621-trust-model-and-freedom-from-interference)).
   * All VMOs are **non-resizable**; consumers map with `ZX_VM_REQUIRE_NON_RESIZABLE` and request `WRITE` permission only
     on a region they may write.
   * **TRANSFER:** a handle that is *sent* over a channel must carry `TRANSFER` (the kernel requires it), but the handle
     a consumer *keeps* must not, otherwise it could re-export the VMO to an unrouted component. The receiving side
     therefore **strips `TRANSFER` on open** (implemented in `VmoSharedMemoryResource::Open`, branch
     `feat/vmo-shared-memory-resource`, `313b0f73`) and consumers must hold none afterwards. The rights are also
     declared in the FIDL handle types (6.3); **whether `fidlc`/the bindings reduce rather than reject on a mismatch is
     untested.**
2. The skeleton hands out **one duplicate per proxy per VMO** during `Attach` (see [6.3](#63-attach-and-service-discovery)).
   After that, FIDL messages carry **no handles and no payload**, only small indices/sequence numbers where a
   notification is needed.
3. Consumers **map once** at attach and keep the mapping; hot-path code uses the existing *local views*
   (`ConsumerEventDataControlLocalView`, …), which already resolve `OffsetPtr`s once.
4. Sending a handle **moves** it. To keep using a VMO the skeleton always sends **duplicates** and keeps its original.
   A new VMO per sample would be possible (`zx_vmo_create_child`) but costs allocation and mapping per message and is
   **not** the design.
5. **The skeleton epoch lives in the read-only data VMO, never in the writable control VMO** (a consumer could forge
   it there). It replaces `ServiceDataStorage::skeleton_pid_` (which is also in the data segment today).

```text
 skeleton                                  kernel                          proxy
 --------                                  ------                          -----
 zx_vmo_create(data) ──────────────────────► VMO object (pages)
 zx_vmo_create(control)
 zx_vmar_map(rw)                                                          Attach() on routed endpoint
 write epoch, slots, atomics ...                                               │
 dup(data, R|M|GP|T)  ──► FIDL reply carries handle(s) ───────────────────────►
 dup(control, R|W|M|GP|T)                                                  strip T; zx_vmar_map(ro / rw)
                                                                           read slots in place
                                                                           CAS on control atomics
```

**Control-block details:** the control VMO today also contains the `application_id_pid_mapping_`; it becomes an
`application_id → incarnation` mapping (6.5). `skeleton_pid_` becomes an atomic **epoch** in the data VMO.

#### 6.2.1 Trust model and freedom from interference

Routing decides *who may attach*, not what an *admitted* component does with a writable mapping. A faulty admitted
consumer with the writable control VMO can corrupt slot timestamps and reference counts (the skeleton then reuses a
slot another consumer still reads), hold slots forever, or overwrite the configuration and `OffsetPtr`s that the
skeleton and the other proxies dereference. The same holds on Linux/QNX today (per-uid ACLs, never per consumer; the
skeleton's `ignore_qm_control_` detection only covers QM-versus-ASIL-B). A consumer really needs to write only (1) "I
hold slot S", (2) "I am subscribed", (3) a recovery record of both.

| Level | Control state | Consumer access | Claim |
|---|---|---|---|
| **Option A (baseline, now)** | one control VMO per ASIL level, shared by all admitted consumers of that level | read/write | **No freedom from interference between consumers of one level.** Valid only if all are developed to the VMO's ASIL and covered by the DFA; AoU below. |
| **Option B (target)** | skeleton-owned control VMO (read-only for consumers) + one **per-consumer hold region** (fixed layout, no pointers, atomics only) written only by its owner; the skeleton aggregates and validates (range, count limit, epoch) and zeroes the region on peer-closed | read-only control, read/write own region | A faulty consumer can only starve slots up to its limit. Also removes transaction logs and the pid mapping on Fuchsia. |
| Option C (not for events) | control updated by the skeleton on request messages | read-only | Per-sample syscalls; used only for `Subscribe`/`Unsubscribe`/methods. |

**Option B verdict (prototype `exp/option-b-hold-regions`, `efdd3725`).** A header-only, OS-agnostic prototype
(slot-generation words owned by the skeleton, one hold VMO per consumer, scan-on-Allocate, Dekker-style
store/revalidate on acquire, audit and eviction) passes 10 host tests (stress, TSan, fault injection; two mutations make
the tests fail), and a 3-process FEMU test: a consumer that keeps writing garbage into its own hold region is evicted,
the healthy consumer is unaffected (0 payload violations), and no consumer can reach another's hold VMO. Host benchmark
against a replica of LoLa's CAS scheme (x86, 48 threads): consumer acquire+release 2.5x faster uncontended at 16
consumers (37 vs 84 ns) and 2.5-8x under contention; skeleton Allocate+Send **+36 % at 16 consumers uncontended** (65 vs
48 ns) but about 4x faster under contention. **Not measured:** ARM64 (the seq_cst/Dekker pair must be reviewed and
measured there), the real LoLa classes (the baseline is a replica), FEMU timing (functional only). Verdict: hot-path
cost neutral to better on x86, safety-side complexity lower (no consumer-writable refcounts, no transaction log), but
LoLa integration (about ten work items, e.g. `ProviderEventDataControlLocalView`, `EventSlotStatus`, per-consumer hold
VMO lifecycle) is not done. **Still no FFI claim between consumers** (risk 2). Assumptions of use for the baseline:

* **AoU-FUCHSIA-SHM-1:** consumers of an ASIL level are developed to at least that ASIL; no QM consumer gets the ASIL-B control VMO.
* **AoU-FUCHSIA-SHM-2:** consumers touch the control VMO only through the LoLa library.
* **AoU-FUCHSIA-SHM-3:** consumers do not duplicate, transfer or forward LoLa VMO handles or their per-consumer endpoint; the integrator does not weaken 6.2.2.
* **AoU-FUCHSIA-SHM-4:** a consumer process holding LoLa handles hosts no code of lower integrity (e.g. untrusted plug-ins).
* **AoU-FUCHSIA-SHM-5:** the realm CML is generated from the mw::com configuration and checked against it (safety-relevant artefact); `strict_permissions` is on for ASIL-B instances.
* **AoU-FUCHSIA-SHM-6:** the system DFA treats the shared control VMO as a dependent-failure initiator between consumers of one level.

The failure modes are summarised in [Appendix B](#b-fmea-summary).

#### 6.2.2 Handle rights (normative) and the tests that prove them

| Handle | Holder | Rights | Never |
|---|---|---|---|
| Data VMO (original) | skeleton/broker | `READ WRITE MAP DUPLICATE TRANSFER GET_PROPERTY` | `EXECUTE`, resizable |
| Data VMO | consumer | `READ MAP GET_PROPERTY` | `WRITE EXECUTE DUPLICATE TRANSFER SET_PROPERTY` |
| Control VMO (original) | skeleton/broker | `READ WRITE MAP DUPLICATE TRANSFER` | `EXECUTE`, resizable |
| Control VMO, Option A | consumer | `READ WRITE MAP GET_PROPERTY` | `EXECUTE DUPLICATE TRANSFER` |
| Control VMO, Option B | consumer | `READ MAP GET_PROPERTY` | `WRITE EXECUTE DUPLICATE TRANSFER` |
| Hold region, Option B | owning consumer | `READ WRITE MAP` | `EXECUTE DUPLICATE TRANSFER` |
| Liveness eventpair | consumer | `WAIT` | `SIGNAL SIGNAL_PEER DUPLICATE TRANSFER` |

The consumer's endpoint channel cannot be restricted when it comes from the component namespace (FM-04 residual).
The notification eventpair (6.6) is held by the consumer with `WAIT | SIGNAL` (no `SIGNAL_PEER`: it can clear its own
side but cannot signal the skeleton).

Status: **verified on FEMU** (branch `feat/fuchsia-attach`, `99bac535`; `w07` E7/E9; Option B prototype `w08`) are the
rights received after `Attach`, rights reduction by the bindings, `TRANSFER` absent after receive, and that a read-only
data handle cannot be widened (map RW, duplicate with WRITE, `vmo_write`, `set_size`, slice and snapshot give
`ACCESS_DENIED`). A CAS on a read-only mapping crashes the reader (`FATAL_PAGE_FAULT`), so every word a consumer must
modify has to live in a separate RW VMO. **Still design-only:** the unrouted-component re-export test with a negative
control and the CI lint for bare `zx.Handle:VMO`/`EVENTPAIR` (risk 3). Rights through FIDL: declared rights with *more*
rights sent are silently reduced to the declared set; with *fewer* rights sent the receiving binding is closed
(`ZX_ERR_INVALID_ARGS`), i.e. the whole channel, not only the call; untyped `zx.Handle` is passed through unchanged.

### 6.3 Attach and service discovery

Today a proxy finds a service instance by watching flag files and then opens shared memory *by name*. On Fuchsia
neither exists, so discovery (information) and attach (access) are separated:

* **Discovery carries information only** (what is offered, in which epoch). It never hands out channel ends, because a
  handle returned in a reply is moved, cannot be given to several consumers, and is not subject to capability routing
  (it would bypass the access policy of 6.4).
* **Access is capability routing.** Every allowed consumer reaches the skeleton through its **own routed `Endpoint`
  capability** (6.4); `Attach` runs on that channel.

**Discovery** replaces flag files + inotify and plugs in behind `IServiceDiscoveryClient`. It is a component with two
protocols, each exposed as **one capability per caller**, renamed with `as` in the realm (7.1), so the discovery
component knows who is calling:

* `ServiceRegistry` (skeleton → discovery): `Offer(id, quality, epoch)` / `StopOffer(id, quality)`. Closing the
  channel (also on crash) withdraws all offers of that skeleton, which is `StopOffer` for 6.5. A generated table checks
  that a skeleton offers only ids it is configured for.
* `ServiceDiscovery` (proxy → discovery): `Find(filter)` returns a snapshot; `StartFind(filter, Search)` creates a
  per-find `Search` channel whose `Watch()` is a hanging get returning full snapshots. Results are **filtered by a
  generated allow-table** (derived from the same configuration as the realm offers), so a consumer sees only the
  instances it may use. `StopFindService` = close `Search`.

Mapping: `OfferService` → `Registry.Offer`; `StopOfferService(id, quality)` → `Registry.StopOffer`; `FindService` →
`Find`; `StartFindService` → `StartFind` + `Watch` loop (handler runs on the LoLa thread); `StopFindService` →
close `Search`.

Semantics: the first `Watch` returns the current snapshot at once, later ones return when the set or any epoch
changes; one outstanding `Watch` per `Search` (a second one closes the channel). The skeleton calls `Offer` only
after its VMOs exist and its endpoints are served. Discovery is a **hint**; `Attach` is authoritative: it returns the
skeleton `epoch`, and a mismatch (or `PEER_CLOSED`) makes the proxy re-`Watch`.

**Attach** replaces "open shm by name": the proxy connects to its routed endpoint `score.mw.com.ep.<service>.<instance>`
(declared with `availability: "optional"`; if not routed, the first call fails with `PEER_CLOSED`, see A.1) and calls `Attach()`. **There is no `application_id` parameter:** the skeleton derives the consumer identity (and
the application id used as transaction-log key) **only from the endpoint the connection arrived on** and returns it,
so a proxy cannot claim another identity. The reply carries the data/control VMO duplicates, the layout metadata, the
`epoch`, a liveness eventpair (6.5) and a per-subscriber notification eventpair (6.6). There is **no event channel**:
an earlier version of this document had the proxy pass an `EventChannel` in `Attach` over which the skeleton would send
events. The implementation showed that direction was wrong (nothing ever served it); events are data in the shared VMO
plus an eventpair wake-up, and FIDL stays control-only.

**Implemented** (`feat/fuchsia-attach`, `99bac535`): `score_mw_com.fidl` with `Endpoint`, `ServiceRegistry`,
`ServiceDiscovery` and `Search` (hanging-get `Watch`); per-caller discovery capabilities; `Attach` without application
id. Deviation: one data+control VMO pair per provider endpoint, so an ASIL-B instance needs its own endpoint per quality
level (consistent with one control VMO per ASIL level, 6.2.1). The sketch below is the *design* sketch; the compiled
protocol on that branch differs in details (e.g. the notification handle, no `EventChannel`).

FIDL sketch (design). The previous sketch did not compile (a `closed` protocol needs `strict` two-way methods, fi-0115);
with `strict` the earlier version compiled with the pinned `fidlc` (audit t14). **This sketch itself has not been
compiled**; the implemented `score_mw_com.fidl` is the reference. Rights on handle types follow 6.2.2 (reduction on
receive verified).

```fidl
library score.mw.com;

using zx;

type ServiceInstanceId = struct {
    service_id uint64;
    instance_id uint32;
    major_version uint8;
    minor_version uint8;
};

type Quality = flexible enum : uint8 { QM = 1; ASIL_B = 2; };

type InstanceFilter = table {
    1: service_id uint64;
    2: instance_id uint32;          // absent = wildcard
    3: major_version uint8;
    4: minor_version uint8;
    5: quality Quality;
};

type OfferedInstance = struct {
    id ServiceInstanceId;
    quality Quality;
    epoch uint64;
};

const MAX_INSTANCES uint32 = 256;

type OfferError = flexible enum : uint32 { NOT_CONFIGURED = 1; ALREADY_OFFERED = 2; };
type AttachError = flexible enum : uint32 { NOT_FOUND = 1; NOT_ALLOWED = 2; INCOMPATIBLE_VERSION = 3; };

@discoverable
closed protocol ServiceRegistry {   // one renamed capability per skeleton
    strict Offer(struct { id ServiceInstanceId; quality Quality; epoch uint64; }) -> () error OfferError;
    strict StopOffer(struct { id ServiceInstanceId; quality Quality; }) -> ();
};

@discoverable
closed protocol ServiceDiscovery {  // one renamed capability per consumer
    strict Find(struct { filter InstanceFilter; })
        -> (struct { instances vector<OfferedInstance>:MAX_INSTANCES; });
    strict StartFind(resource struct { filter InstanceFilter; search server_end:Search; });
};

closed protocol Search {
    strict Watch() -> (struct { instances vector<OfferedInstance>:MAX_INSTANCES; });
};

closed protocol Endpoint {          // served at one path per allowed consumer, routed with `as`
    strict Attach() -> (resource struct {
        data_vmo zx.Handle:<VMO, zx.Rights.READ | zx.Rights.MAP | zx.Rights.GET_PROPERTY>;
        // Option A; Option B: READ|MAP|GET_PROPERTY plus a per-consumer hold region (6.2.1)
        control_vmo zx.Handle:<VMO, zx.Rights.READ | zx.Rights.WRITE | zx.Rights.MAP | zx.Rights.GET_PROPERTY>;
        layout vector<uint8>:4096;
        epoch uint64;                 // copy of the data-VMO header epoch, for the mismatch check
        application_id uint64;        // derived by the skeleton from the endpoint, not supplied by the caller
        liveness zx.Handle:<EVENTPAIR, zx.Rights.WAIT>;   // peer-closed == skeleton gone
        notification zx.Handle:<EVENTPAIR, zx.Rights.WAIT | zx.Rights.SIGNAL>;   // USER_0 = new sample (6.6)
    }) error AttachError;
};
```

Subscribe/unsubscribe, notification registration and method calls continue to run through `message_passing`.

Open points, now measured on FEMU (`exp/fuchsia-open-questions`, `5262d114`, single runs, SMP=4, indicative):
connecting to a *routed* provider that is not yet running **waits** (no error; a 3 s late provider gave a connect that
returned after 3.0 s), so the client needs its own timeout; an unrouted capability or an exited provider gives
`PEER_CLOSED`. After a skeleton kill `PEER_CLOSED` arrives within about 1 ms and reconnect to the restarted process takes
1-16 ms (up to 2 attempts). A strong skeleton/proxy cycle is rejected by `cmc`; one `dependency: "weak"` edge suffices.
1500 parked `Watch`es under churn: 1500/1500 completed, 0 errors, reply p99 <= 47 ms. After a discovery restart all
`Watch`es see `PEER_CLOSED` and a re-`Watch` returns a full snapshot in about 10-15 ms (generation restarts at 0).
1 provider x 50 services x 30 consumers: 0 errors. `Realm.CreateChild` with dynamic protocol/service offers works
(about 2 ms) and `source_instance_filter=[inst_a]` restricts correctly, but combined with `renamed_instances` it neither
restricted nor renamed (unreliable, **do not rely on it**).
Rejected: *service* capabilities with instances (no per-consumer filtering or identity, A.2) and dynamic offers via
`Realm.CreateChild` (moves the access policy out of the static, reviewed CML).

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
3. **`ClientIdentity` becomes an opaque type** (implemented, `400bc46b`). It supports equality, hashing, a log string and
   `IsAllowed(AllowedConsumers)`. On Linux/QNX it wraps pid/uid. On Fuchsia it wraps the *application id* derived
   from the endpoint. Call sites stop looking at `.pid`/`.uid` directly. This also removes the "fake zeros" problem.
   `Attach` takes **no** application id from the caller (6.3): identity is derived from the endpoint only.
4. **The pid still needs a replacement as peer key and name seed.** Two different things must not be conflated: the
   **principal** (which capability, i.e. which consumer; stable) and the **node** (which incarnation of that
   consumer; volatile, a broker lease, 6.5). `FromEndpoint` in `400bc46b` currently sets both to a per-connection id,
   which is a known issue to fix in the incarnation PRs ([10.3](#103-incarnation-aware-recovery-code-impact-and-pr-list)).
   **Status:** `feat/identity-split` (`9a86db64`) separates node and principal; `feat/identity-opaque` (`c1e6187c`)
   makes `NodeId`/`PrincipalId` distinct, non-convertible opaque types (`OpaqueId<Tag>`) throughout the LoLa messaging
   layer; Linux/Fuchsia pid/uid conversions happen only at the platform boundary. Verified: 227 host tests pass (3
   skipped), FEMU `message_passing_itf_test` passes. **Not done:** the wire format is still a 4-byte pid
   (`kOutdatedNodeId`), message-passing names still use the pid, and skeleton internals and configuration still use
   `pid_t`/`uid_t`; `IncarnationId` and the resolver are later PRs.

Example: see [7.1](#71-capability-routing-for-one-service); app_c gets no offer, so its connection fails (`PEER_CLOSED` on first use).

**Semantics to define:** `strict_permissions` on Fuchsia. Proposal: *strict* means "the realm must contain an offer per
configured allowed consumer and the skeleton rejects connections arriving on endpoints that are not in its configured
list" (defence in depth); *non-strict* behaves as today (all connected consumers allowed). A **build-time check**
compares the generated realm offers with the mw::com configuration.

**Safety implication.** The realm CML becomes a safety-relevant artifact (review, traceability, change control), just
like the mw::com configuration is today. It should be **generated** from the configuration (one source of truth).

### 6.5 Partial restart and crash recovery

**What must be preserved** (behaviour of today's LoLa): a skeleton restart must not corrupt proxies that still hold
slots; a proxy restart must not leak slot references of its previous incarnation; proxies must learn about a skeleton
restart and re-subscribe.

**Why the Linux mechanism does not carry over.** Today the skeleton decides between *recreate* and *reuse* of shared
memory by trying an exclusive `flock` on the usage marker (section 2; reuse is the real partial-restart path: the
memory survives, proxies keep mapping it, the restarted skeleton re-adopts it and runs `CleanupSharedMemoryAfterCrash`).
The kernel drops a flock when the *process* dies, so "lock obtained" means "no proxy process is left". A Fuchsia
eventpair is released when a *handle* is closed. A proxy can close it and still hold a mapping (or the VMO handle and
re-map later) and write the control VMO. Neither a handle nor a mapping can be revoked from outside. Therefore **a
closed eventpair proves nothing about memory use; only the termination of the consumer's process does.** The VMO
refcount does keep the memory alive across a skeleton death, which is exactly why re-adoption is unsafe while proxies live.

**Design decisions.**

1. *Rebirth is the default.* Every skeleton incarnation creates a **new** data VMO and a **new** control VMO. The old
   VMOs are fenced and abandoned; a new skeleton never writes memory a former proxy may still hold. Proxies re-attach,
   map the new VMOs and re-subscribe (they must anyway).
2. *Adoption is an optimisation.* Re-using the old VMOs is allowed only if every lease that received a handle to that
   incarnation is proven dead via `ZX_PROCESS_TERMINATED`. In the common case "skeleton crashes, proxies keep running"
   adoption is therefore **not** possible.
3. *Epoch.* A 64-bit epoch, strictly increasing per service instance (`max(prev+1, boot-clock ns)`, issued by the
   broker), lives in a header cache line **of the read-only data VMO** (together with layout hash and scrub state) and
   replaces `skeleton_pid_`. **It is not in the writable control VMO**, where a consumer could forge it. `0` = not
   published, `UINT64_MAX` = fenced. It is stored with release semantics **last**, after initialisation (and after the
   scrub in the adoption case). The broker keeps the full-rights data handle and is the only party besides the skeleton
   that writes the header (the fence value).
4. *Leases and the broker.* The **broker watches process termination**: each skeleton and each attaching proxy passes a
   process-handle duplicate (`WAIT | INSPECT`), the broker issues a monotonic `lease_id` and marks the lease `DEAD`
   when the process terminates. `lease_id` replaces the pid in the application-id mapping. The broker is not on the
   data path; it issues epochs and leases, holds VMO handles (for adoption), **fences first** on skeleton loss (stores the
   fence value in the data-VMO header and asserts a `fenced` signal on each proxy's instance eventpair), tracks
   quarantined VMOs and decides adoptability. Without a broker (phase 1) the fallback is rebirth only.
5. *Liveness signals.* The skeleton-liveness eventpair (peer held by the skeleton, so broker death never looks like
   skeleton death) and the broker's `fenced` eventpair give proxies prompt notice; the header epoch is the hot-path
   accelerator (one acquire load per `GetNewSamples`; `ReferenceNextEvent` and `SamplePtr` release unchanged because a
   stale decrement under rebirth only touches abandoned memory).

**Skeleton restart, rebirth.** `BeginIncarnation` → create and map new VMOs → initialise → store epoch → `Deposit` →
`Publish` → `Offer`. The old VMOs are not rolled back; their content is discarded and they stay *quarantined* in the
broker (bounded by a budget; a faulty proxy that pins memory is a supervisor event) until all holders are dead.

**Skeleton restart, adoption (only if all consumer leases are dead).** `TryAdopt` → resumable, idempotent scrub
(provider cleanup, rollback of all logs, zero refcounts and subscriptions) → store the new epoch → `Publish` → `Offer`.
*Scrub, then epoch, then offer, then attach.* A crash leaves `scrub_state != DONE`; the next skeleton repeats the scrub
or falls back to rebirth. Linux/QNX behave like this today, except that the epoch is currently written *before* the
cleanup (to be reordered, PR 5).

**Proxy crash and restart.** The broker sees the process terminate (`DEAD`) and tells the skeleton. The logs of that
lease are rolled back **either** by the skeleton (new: a proxy that never returns no longer leaks slots) **or** by the
restarted proxy of the same application id (which receives `rollback_for` in the broker `Attach`). A claim word in each
`TransactionLog` (`ACTIVE(L)` → `NEEDS_ROLLBACK(L)` → `ROLLING_BACK(L, executor)` → `CLEAN`) gives a single executor and
lets another take over a dead one. A log is **never** rolled back while its owner is not `DEAD`; a live twin with the same
application id is rejected (`ALREADY_ATTACHED`). Re-runnability of `RollbackProxyTransactions` after a crash mid-rollback
is **assumed and unproven**; characterization tests come first (PR 0).

**Code level.** `IncarnationId` (pid on Linux/QNX, lease on Fuchsia), `SkeletonEpoch`, `NodeId` and an injectable
`IIncarnationAuthority` (takeover policy `kAlwaysTakeover` on Linux/QNX = today's behaviour, `kRequireDead` on Fuchsia);
stale notifications are handled by rules (receiver identity is the connection's `NodeId`, an outdated-incarnation message
only removes state). Impact and PR sequence: [10.3](#103-incarnation-aware-recovery-code-impact-and-pr-list).

**Broker death.** The data plane keeps running (fail-static). Proxies tell *broker lost* (fenced eventpair closed
without the `fenced` signal) from *skeleton lost*; if the skeleton dies meanwhile, proxies fence themselves from the
liveness eventpair and nobody writes the header fence value. A restarted broker is empty: the running skeleton
re-deposits, proxies re-attach, adoption is refused for a reconcile period. If broker and skeleton are both gone the next
skeleton does rebirth.

**Faulty proxies.** Memory pinning (bounded by quarantine) and writes into an old control VMO (harmless under rebirth)
are contained; corruption of the *current* control VMO is **not solved** in the baseline (Option B, 6.2.1). Remote
revocation and `set_size`/decommit do not stop a mapped writer and are rejected. Test matrix: [Appendix C](#c-test-matrix-summary).
**Implementation status (FEMU-verified parts only; everything else above remains design).**

* *Locks* (`feat/fuchsia-lock`, `fed9fe49`): `ILockProvider` with `PosixLockProvider` (flock) and a Fuchsia
  `RegistryLockProvider` whose locks are bound to an eventpair, so a holder's death releases the lock. FEMU test passes
  including holder death. It is a registry, **not yet a real broker**, and does not give the "process terminated"
  guarantee argued above (handle closure vs process death).
* *Lease broker* (`feat/fuchsia-broker`, `5ad56a4b`; hardening `feat/fuchsia-broker-hardening`, `1571950c`): epoch in an
  `IncarnationHeader` at the start of both VMOs (**deviation:** the broker writes the tombstone `UINT64_MAX` into the
  *control* VMO, not the read-only data VMO), epoch = `max(last+1, boot clock)`, state machine PROVISIONING -> ACTIVE ->
  FENCED -> released, at most 2 draining incarnations per instance, a second live incarnation refused. Death is detected
  by a liveness eventpair (`PEER_CLOSED`) and an optional process-handle watch (`PROCESS_TERMINATED`); the broker
  tombstones and signals each proxy's `fenced` eventpair. The broker does **not** hand VMOs to proxies (the routed
  skeleton `Endpoint` does, preserving capability identity); `application_id` in `RegisterProxy` is caller-asserted.
  Verified on FEMU (414 checks, about 24 s): skeleton death by process-only, liveness-only and both; proxy death by
  release, liveness close and process kill (leases 4 -> 1); broker kill and restart (proxy distinguishes *broker lost*
  from *skeleton lost*; `Reannounce` adopts a live skeleton's epoch, wrong epoch -> `INVALID_REGION`, duplicate ->
  `ALREADY_PROVISIONED`); 100 kill cycles with 0 monotonicity violations, 0 missed fences and no leaked handles
  (the hardening also fixed an unbounded per-lease eventpair leak found while writing the test). Also `broker_core_test`
  (12 host cases) and a skeleton `zx_process_exit`/`__builtin_trap` kill with proxy reattach to a new epoch.
  **Limitations:** adoption trusts the caller (any holder of valid VMOs with a correct header can adopt an unused
  instance; no scrub state, no trusted-ack/ASIL profile); proxies that held leases on a fenced incarnation at broker
  restart cannot re-register; `Reannounce` is not wired into `skeleton_main`; `last_epoch` is lost on a broker restart, so
  monotonicity relies on the boot clock (monotonic across a broker restart, not across a reboot); the holders in the
  hardening test are impersonated by the test; the process-handle path is only asserted in the hardening test (liveness
  fires first in the plain test); single instance/proxy in `fuchsia_broker_itf_test`.
* *Why a broker cannot find processes itself* (`exp/fuchsia-open-questions`, E10): `ZX_PROCESS_TERMINATED` needs
  `ZX_RIGHT_WAIT` on the process handle (0.8 ms after kill; `ZX_INFO_PROCESS_MAPS` needs `INSPECT`), and a component in
  another job cannot obtain it (`zx_object_get_child` -> `NOT_FOUND`, no `RootJob` routed): peers must send the handle
  themselves. A liveness eventpair (`PEER_CLOSED` 0.2 ms, no extra rights) is the cheaper signal, but only a process handle
  distinguishes crash from orderly exit (`return_code`).
* *Rollback re-run question* (`feat/incarnation-pr0-2`, `3aca6e1b`; `feat/incarnation-pr3-7`): re-running an interrupted
  rollback is safe only at slot granularity; a stuck log whose begin/end bits cannot tell "before" from "after" the
  decrement may underflow if repeated. A `kQuarantine` policy (clear the record, worst case one leaked reference) exists
  next to the default `kFail`; production (`proxy.cpp`) still uses `kFail`.

The remaining mechanism (rebirth wiring in LoLa, adoption with scrub, provider-side rollback, claim word) is **design-only**.

### 6.6 Notification path

Event *data* is in the VMO. Event *wake-ups* (receive handlers) need a notification mechanism. Rules: one-way, no ack
(the publisher never blocks on a slow subscriber); coalescing is acceptable ("something new in slot >= n"); pure polling
consumers (`GetNewSamples` without a handler) never use FIDL after `Attach`.

Measured on FEMU (Appendix A.4, 10 runs per cell, single slot, shared and loaded host, **emulation inflates notification
costs**), notification costs about 5-7 us **per subscriber**, i.e. O(m), not O(1):

* **Shared VMO + per-subscriber eventpair** (peer signalled by the publisher) is the choice for payloads **>= 4 KiB or
  >= 8 subscribers**: publisher cost 7.9 us (4 KiB, m=1) vs 96 us for FIDL, and 90 us vs 1520 us at m=16.
* **FIDL messages are competitive for small payloads and <= 4 subscribers** (64 B, m=1: 13.3 us vs 6.5 us; m=4: 50 us vs 24.7 us).
* A shared event object is no better than eventpairs and worse from m >= 8; futexes cannot be used across processes.
* **Polling is opt-in only** for dedicated cores: lowest latency (0.65 us for 64 B), but with oversubscribed CPUs p99
  reached 7 ms and delivery fell to 25-60 % (FIDL stayed at 100 %).

Not covered: port-based subscribers, multi-slot rings (a late subscriber loses samples with a single slot), real hardware.

**Implemented** (`feat/fuchsia-events`, `2fba0b66`): the `EventChannel` of the earlier sketch is **removed** (its direction
was wrong, see 6.3). `Attach` returns a per-subscriber notification eventpair (`WAIT | SIGNAL`); the skeleton stores the
sample, release-stores the index and calls `zx_object_signal_peer(USER_0)` on each session (never blocks, coalescing).
The subscriber waits for `USER_0 | PEER_CLOSED`, **clears `USER_0`, then** re-reads the index (no lost wake-up);
`PEER_CLOSED` = skeleton gone. Polling is opt-in: closing the handle makes the skeleton drop that session. Dead
subscribers are pruned lazily (next notify/Attach). FEMU (single run, SMP=8, one slot, 500 samples at 1 kHz, indicative
only): publish cost p50 9.9 us (m=1) to 57.8 us (m=8), about 7 us per extra subscriber; wake latency p50 about 40-80 us;
a 20 ms/event slow subscriber saw 26 of 500 events and ended at the last sequence while the publisher stayed unblocked
(p50 28 us, max 1.4 ms); killing a subscriber did not disturb the publisher; every surviving subscriber reached the final
sequence (delivered < 100 % is coalescing). Publisher p99/max are ms-scale even at m=1 (host scheduling noise). **Not
done:** ring/slot index, port-based multiplexing, async peer-closed pruning, `NotifySubscribers` is O(m) syscalls under a
mutex, and it is **not hooked into LoLa's event/skeleton code**, only the transport.

### 6.7 Typed interfaces and IDL options

Three levels of typing for the *FIDL side*. The shared-memory payload types remain C++ types in all of them.

| Option | Description | Pro | Con |
|---|---|---|---|
| **A. Generic protocol** (recommended start) | One protocol (`Endpoint`) for all services; the service is data (`ServiceInstanceId`, layout). | One FIDL library; no generator. | CML capabilities are untyped w.r.t. service type unless a naming convention is used. |
| **B. Generic protocol + typed marker capability** | As A, plus an empty protocol per service interface used only for CML routing. | Typed routing and review at low cost. | Needs a small generator or hand-written markers. |
| **C. Fully typed per-service FIDL (FIDL-first)** (alternative to D) | `.fidl` per service interface is the source of truth; a generator over the `fidlc` JSON IR emits mw::com C++ types, FIDL, CML and layout constants. Payloads restricted to fixed-layout, trivially copyable types. | One source of truth; strongest checking. | Needs a generator; FIDL events (messages) vs mw::com events (shared slots) is a semantic mismatch carried by attributes; Linux/QNX need a non-FIDL C++ backend; migration of existing interfaces. |
| **D. ECU-model-first** (recommended target, see 6.9/6.10) | `inc_ecu_model` (Franca/protobuf + FDEPL) is the source of truth; generators emit C++ types, mw::com interfaces, config, the typed FIDL marker (as in B) and the realm CML. | Single source of truth on all OSes; reuses existing parsers and the `DatatypeGenerator`; the model already holds the provider/consumer graph. | Incubator maturity; new generators needed. |

FIDL-generated *wire* C++ types for primitive-only structs may be directly usable as payloads on Fuchsia, but
generated plain structs are the safer default for multi-OS portability (the FIDL C++ bindings exist only in the
Fuchsia SDK and FIDL does not guarantee trivially-copyable layouts of its generated C++).

### 6.8 Realm composition and scaling

* The realm CML is **generated** from the mw::com configuration (one capability and one offer per allowed
  consumer and instance, plus discovery routes). Output must be stable, sorted and diffable; a build test compares it
  with the configuration.
* The number of offers is a **consumers × endpoints** matrix (30 apps × 50 services ≈ 1,500). Routing happens at `Attach`
  time; the hot path never touches FIDL. Reduction: subrealms, dictionaries. **Service-instance filtering is not
  available** (A.2). No limit was hit up to 10,000 offers and connect cost does not depend on their number (A.3).

### 6.9 The example service: how `WheelSpeed` is defined

The examples in this document use one service, `WheelSpeed`. This section defines it completely, from the
binding-independent design down to what each layer sees. Everything here is a **design sketch** (not built), except
that the C++ shape and the JSON configuration shape follow what exists today
(`score/mw/com/example/com-api-example/`, `etc/mw_com_config.json`).

**Step 1: design-time interface (binding independent).** Owned by the interface author; no OS, no shared memory.

| Element | Kind | Content |
|---|---|---|
| `score.vehicle.WheelSpeed` v1.0 | interface | namespace `score.vehicle` |
| `WheelSpeedSample` | struct | `front_left`, `front_right`, `rear_left`, `rear_right`: `float32` (rad/s); `timestamp_ns`: `uint64`; `valid`: `bool` |
| `Quality` | enum | `kOk`, `kDegraded`, `kInvalid` |
| `speed` | broadcast (event) | outputs: `sample: WheelSpeedSample` |
| `calibration` | attribute (field) | `float32`, getter + setter, subscribable |
| `Reset` | method | no inputs, output `Quality`, error codes enum |

Constraint carried over from the data plane (section 6.2): everything that travels through an *event* must be
fixed-layout and trivially copyable (no strings, vectors or maps), because it is `memcpy`-ed into a shared slot.

**Step 2: deployment.** A deployment binds the interface to the `MW::COM` binding and provides the per-member
numbers that LoLa needs today in `mw_com_config.json`.

| Deployment property | Value | Today's JSON key |
|---|---|---|
| `service_id` | `6433` | `serviceTypes[].bindings[].serviceId` |
| event id `speed` | `1` | `events[].eventId` |
| instance id | `1` | `instances[].instanceId` |
| `asil-level` | `QM` | `asil-level` |
| `numberOfSampleSlots` (speed) | `30` | `events[].numberOfSampleSlots` |
| `maxSubscribers` (speed) | `3` | `events[].maxSubscribers` |
| provider app / consumer apps | `4001` / `[4002, 4003]` | `allowedProvider`, `allowedConsumer` |

Fuchsia-specific, optional deployment properties (ignored by Linux/QNX): `fuchsia.capability_prefix`,
`fuchsia.provider_component` (offer source) and `fuchsia.consumer_components` (application id → component, offer targets).

Note that on Fuchsia `allowedConsumer` is **compiled into the realm** (section 6.4) rather than checked against a
runtime uid; the same data therefore drives both the runtime check on Linux/QNX and the generated CML on Fuchsia.

**Step 3: what each layer receives.** C++ payload types (existing datatype generator, 6.10); the mw::com interface
(`WheelSpeedInterface<Trait>` with `Event<WheelSpeedSample> speed`, `Field<float> calibration`, `Method<Quality()> Reset`);
`mw_com_config.json` (same schema as today); a typed FIDL marker `score.mw.com.ep.WheelSpeed` extending `Endpoint` (option B,
6.7; the payload is **not** in FIDL); a realm fragment with capability `score.mw.com.ep.WheelSpeed.for_app_4002`, offered
to `app_4002` with `as` (7.1); and layout constants (slot size, count, VMO sizes).

Generated FIDL for the typed marker (sketch):

```fidl
library score.mw.com.wheelspeed;

using score.mw.com;

// Routing marker only: gives CML a typed capability per interface. Behaviour comes from Endpoint.
// `Endpoint` takes no application id: the skeleton derives the identity from the endpoint (6.3).
@discoverable(name = "score.mw.com.ep.WheelSpeed")
closed protocol WheelSpeed {
    compose score.mw.com.Endpoint;
};
```

The generated sketch needs `strict` on two-way methods and `@discoverable(name=...)` (found by the realm generator
experiment below). The mw::com C++ interface and configuration are *unchanged for applications* (section 7.4); the new artifacts are
the FIDL marker, the realm fragment and the layout constants.

### 6.10 Generators and `inc_ecu_model`

The `eclipse-score` organisation already has an incubator, **`inc_ecu_model`** ("an IDL independent data model
representing the interfaces between the different components inside the stack of an automotive ECU"), plus parsers
and a code-generation framework. It is the natural first tier for the IDL question of section 6.7: instead of making
FIDL the source of truth (option C), the source of truth is the **ECU model**, and FIDL becomes one *output*.
(`inc_score_codegen` is, at the time of writing, an empty repository with a one-line description.)

**What exists today in `inc_ecu_model`** (shallow clone of `main`): a pydantic model (data types, `InterfaceDefinition`,
`ServiceInterface` with deployment properties, provided/required service ports, `Ecu`/`Application`), parsers (Franca +
FDEPL, protobuf, `load_and_merge`) and a generator runner with a single generator, `DatatypeGenerator` (one C++ header
per data type; Bazel `score_ecu_model_artifacts`/`cc_score_ecu_model_library`). Not there yet: mw::com interface classes,
`mw_com_config.json`, anything platform-specific; README/Architecture are "TBD".

**The `WheelSpeed` model** would be authored in Franca (`.fidl` + `.fdepl`, existing parser) or protobuf for the data
types, and the architecture in the model's `Ecu`/`Application` objects. Instances and the provider/consumer relation come
from `ProvidedServicePort`/`RequiredServicePort`: **the model already contains exactly the graph that the realm generator
needs** (who offers what to whom).

**Proposed generators** (registered in `GENERATOR_FACTORIES`, one model load):

| # | Generator | Output | Fuchsia-only? |
|---|---|---|---|
| G1 | `DatatypeGenerator` (exists) | C++ payload headers (add a fixed-layout validation for event types) | no |
| G2 | `MwComInterfaceGenerator` | `*_gen.h` proxy/skeleton aliases | no |
| G3 | `MwComConfigGenerator` | `mw_com_config.json` per application | no |
| G4 | `FidlMarkerGenerator` | typed routing marker `.fidl` | yes |
| G5 | `RealmGenerator` | sorted, stable `*.realm.cml` (needs the application-to-component mapping) | yes |
| G6 | `LayoutGenerator` | `*_layout.h` (should call a LoLa constexpr size function) | yes (initially) |

Validators from day one: every required port has a provider (else `PEER_CLOSED`, A.1); generated `allowedConsumer`
equals the set of requiring applications (config and realm cannot diverge); event payloads are fixed-layout;
application ids are unique (risk 17).

**Bazel wiring.** One model target feeds all generators; platform outputs (G4-G6) live in a `fuchsia/` sub-directory
consumed through filegroups, so that Linux and QNX builds never depend on them.

**Can the ECU model read Fuchsia FIDL files?** Not today, but it can be added. Caution: the existing
`franca_parser` reads *Franca* IDL, which also uses the `.fidl` extension; it cannot read Fuchsia FIDL. A new
parser would follow the `protobuf_parser` pattern (Bazel rule producing a registry pickle, merged by `load_and_merge`):

A new parser would follow the `protobuf_parser` pattern: run `fidlc` and transform its JSON IR (new
`DataTypeSource.FUCHSIA_FIDL`); struct/enum/union/array/alias map to the existing data types, FIDL events to `Broadcast`
and methods to `Method`; deployment (slots, ids) from `@attribute`s or a sidecar file. Types unsuitable for events are
rejected by the fixed-layout validator.

Effort is moderate (mostly type and protocol mapping). It is optional if Franca remains the project IDL, since the
Fuchsia FIDL marker is generated as an *output* (G4).

**Relationship to option C of section 6.7.** The ECU-model route achieves the goals of the FIDL-first idea (one
source of truth, generated C++/FIDL/CML/config) without making FIDL the IDL for all OSes, and it reuses the Franca/FDEPL
tooling that the project already maintains. It also answers the earlier question "why not use the types generated by
`fidlc`?": the C++ payload types come from the ECU model on all platforms, so there is a single type system.
The cost is that the ECU model is an incubator (several parts are "TBD" or open), so G2/G3 depend on its progress;
a pragmatic start is to keep the hand-written `vehicle_gen.h`-style interface and begin with G4/G5 only.

**Spikes (status).**

* **Realm + typed marker generator** (G4/G5 prototype): branch `exp/realm-generator`, `dba39624`; 20 unit tests pass,
  and `cmc` and `fidlc` were run on its output. Findings: FIDL sketches need `strict` on two-way methods and
  `@discoverable(name=...)`. It is a standalone prototype, not yet integrated into `inc_ecu_model`.
* **FIDL parser** (`spike/fuchsia-fidl-parser`, `fe72722`, in an `inc_ecu_model` clone): parsing `fidlc` JSON IR works.
* **Datatype lowering** (`feat/datatype-generator-lowering`, `8b6f41c`, in the `inc_ecu_model` clone): struct, enum, union
  and array lowering plus a `FixedLayoutValidator`. Unions are lowered to tagged fixed-layout structs; **maps are
  unsupported**. This replaces the "about one week" estimate for lowering (done); integration upstream is not.
* **Realm on FEMU** (`exp/realm-generator-femu`, `6bf59dec`): the generated realm runs. Small model (3 skeletons + 5
  consumers + discovery): pass; scale model (5 skeletons + 15 consumers = 20 apps): `cmc` builds all 22 manifests and the
  realm passes. Per-caller discovery is verified (each consumer sees and attaches only to its routed services, with
  rights checked); an unrouted consumer cannot see or attach; a provider offering a foreign service id is refused. Fixes
  found: one shared discovery for all apps (now per-caller), missing `LogSink`, providers `use`ing discovery, strong
  cycles (now SCC detection with weak edges). Limitations: single run per model, 20 apps only, negative checks have no
  mutation test, nodes use the raw `Endpoint` (typed marker compiled but unused), harness-mode arguments are test-specific.

---

## 7. End-to-end examples

### 7.1 Capability routing for one service

One skeleton (`wheelspeed_skeleton`), apps A and B allowed, app C not allowed. *(The `cmc` compilation of the pattern is
covered by experiment A; the full sketch below was **not run** and omits `LogSink` offers. Runtime behaviour is only
partly covered: routing and identity, A.1.)* Discovery is per caller (6.3): one `ServiceRegistry` capability per
skeleton and one `ServiceDiscovery` capability per consumer, each renamed with `as`.

```json5
// wheelspeed_skeleton.cml (excerpt)
{
    program: { runner: "elf", binary: "bin/skeleton" },
    capabilities: [
        { protocol: "score.mw.com.ep.WheelSpeed.for_app_a" },
        { protocol: "score.mw.com.ep.WheelSpeed.for_app_b" },
    ],
    use: [ { protocol: "score.mw.com.ServiceRegistry" } ],
    expose: [
        { protocol: "score.mw.com.ep.WheelSpeed.for_app_a", from: "self" },
        { protocol: "score.mw.com.ep.WheelSpeed.for_app_b", from: "self" },
    ],
}
// app_a.cml (app_b.cml identical; app_c has no `ep` use that is routed)
{
    program: { runner: "elf", binary: "bin/app" },
    use: [
        { protocol: "score.mw.com.ep.WheelSpeed", availability: "optional" },
        { protocol: "score.mw.com.ServiceDiscovery" },
    ],
}
// realm.cml (excerpt; one offer entry per consumer and capability)
{
    offer: [
        { protocol: "score.mw.com.ep.WheelSpeed.for_app_a", from: "#wheelspeed_skeleton", to: "#app_a",
          as: "score.mw.com.ep.WheelSpeed" },
        { protocol: "score.mw.com.ep.WheelSpeed.for_app_b", from: "#wheelspeed_skeleton", to: "#app_b",
          as: "score.mw.com.ep.WheelSpeed" },
        // nothing is offered to app_c
        { protocol: "score.mw.com.ServiceRegistry.for_wheelspeed", from: "#discovery", to: "#wheelspeed_skeleton",
          as: "score.mw.com.ServiceRegistry" },
        { protocol: "score.mw.com.ServiceDiscovery.for_app_a", from: "#discovery", to: "#app_a",
          as: "score.mw.com.ServiceDiscovery" },
        // ... one ServiceDiscovery.for_<app> per consumer, plus the discovery expose
    ],
}
```

Result: A and B open `score.mw.com.ep.WheelSpeed` and arrive on *different* skeleton endpoints; C cannot reach the
skeleton (its first call fails with `PEER_CLOSED`, A.1). The `offer` list *is* the `allowed_consumer` list; `as` needs
exactly **one** protocol per `offer` entry (A.1).

### 7.2 Offer → find → attach → read a sample

```text
 skeleton              discovery               proxy                    kernel / VMO
    │ create VMOs, write epoch into data header, serve ep.* ──────────────────│
    │ Registry.Offer(id, q, epoch) ►│                                         │
    │                               │◄── StartFind(filter, Search); Watch() ──│ proxy A
    │                               │──── [{id, q, epoch}] (filtered for A) ─►│
    │◄──────────────────────────────── Attach() on routed ep for A ──────────│
    │ identity := A (from the endpoint; no id parameter); check A allowed
    │ dup(data, R|M|GP|T), dup(control, R|W|M|GP|T), epoch, application_id, liveness, notification
    │────────────────────────── reply: handles + layout ─────────────────────►│
    │                                                  strip T; map data (ro), control (rw)
    │ AllocateNextSlot; write sample; EventReady (atomics)                    │
    │──────── eventpair signal USER_0 (only if notification kept) ───────────►│
    │                                                  read slot IN PLACE; CAS refcount
```

Transport parts verified on FEMU (`feat/fuchsia-attach`, `feat/fuchsia-events`); the LoLa slot protocol is not hooked up.

### 7.3 Skeleton crash and restart (with broker)

Design-only; details in 6.5. Rebirth is the default; adoption is conditional.

```text
 skeleton#1 dies ──► broker sees ZX_PROCESS_TERMINATED, fences: tombstone epoch in the data header + `fenced` signal
                     proxies: liveness peer-closed / epoch check ──► state "service unavailable"
 skeleton#2 starts ──► broker: BeginIncarnation (new epoch, leases)
   rebirth (default): new data/control VMOs, init, store epoch, Publish, Offer; old VMOs quarantined
   adoption: only if all consumer leases are DEAD (TryAdopt) ──► scrub + roll back logs ──► store epoch ──► Offer
 discovery Watch ──► proxies re-Attach (epoch mismatch / OnSkeletonRestart) ──► re-subscribe
 (without a broker: rebirth only; slot references in the old memory are simply discarded)
```

### 7.4 What an application sees

Nothing changes in application code: `Skeleton::Create` / `OfferService`, `Proxy::FindService` / `Create`,
`Subscribe`, `GetNewSamples` (zero-copy read from the VMO). The instance specifier → deployment mapping
(configuration) additionally selects the Fuchsia binding parameters.

---

## 8. Alternatives considered

| # | Alternative | Summary | Verdict |
|---|---|---|---|
| **R** | **Recommended**: LoLa data plane on VMOs, FIDL control plane, capability-based access control | See above. | ✔ |
| **P** | **Pure FIDL transport** (samples sent as FIDL messages, no shared memory) | Idiomatic Fuchsia. Subscribers receive copies. | Competitive for small payloads and <= 4 subscribers (A.4); ✘ for >= 4 KiB payloads or >= 8 subscribers: O(m) copies and syscalls per sample (96 µs vs 7.9 µs publisher cost at 4 KiB, m=1), 64 KiB message limit, no zero-copy. |
| **H** | **Hybrid with VMO-per-sample** | A new/child VMO per message. | ✘ allocation/mapping per sample; only for rare, large one-shot transfers. |
| **E** | **POSIX emulation** (keep `shm_open` + flock + files, supply a POSIX layer) | Minimal code change if the platform libc offers it. | Not viable to rely on: no global shm namespace/shared filesystem across components; breaks the isolation model; also keeps pid/uid assumptions. (Unless using a Linux-compat layer such as Starnix — a different product decision, not evaluated here.) |
| **B** | **Broker-owned memory** (a broker creates and owns all VMOs; skeleton asks it) | Strong lifetime model, easy partial restart. | Optional add-on, not a replacement. Extra component in the safety path. |

---

## 9. Pros and cons of the recommended concept

### Pros

* **Performance preserved.** Hot path is zero-copy and syscall-free; FIDL cost is only at attach and for
  notifications. Data fan-out is O(1), notification is O(m) at about 5-7 µs per subscriber. Measured on FEMU (A.4,
  10 runs, emulation inflates costs): at m=16 the publisher cost with per-subscriber eventpairs is 75-102 µs for
  64 B-60 KiB versus 183 µs-14.5 ms for FIDL; for small payloads and <= 4 subscribers FIDL is competitive.
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
* **Partial restart needs new design** (broker with process-termination leases, rebirth by default, epoch). The broker
  adds a component and a new failure mode; without it only rebirth is available.
* **Control VMO is writable by all admitted consumers of a level** (Option A baseline). A faulty consumer can corrupt
  control state, so **no freedom-from-interference claim between consumers** is made until Option B (6.2.1); AoU apply.
* **Two OS-specific mechanisms to maintain** for memory (POSIX shm + VMO) behind one interface; more test matrix.
* **Routing size.** consumers × endpoints offers; needs a generator and subrealm structure.
* **New tooling dependency** (Fuchsia SDK, FEMU, `cmc`/`fidlc`, product bundle) in CI; no Fuchsia CI job exists yet.
* **Unverified assumptions** (see sections 12 and 13).

### Trade-off summary against the main alternative (pure FIDL)

| | Recommended (VMO + FIDL) | Pure FIDL |
|---|---|---|
| Publisher cost per sample | data once, plus about 5-7 µs notification per subscriber | one channel write and payload copy per subscriber |
| Subscriber read | in place, no syscall | kernel copy + decode |
| Max payload | slot size | 64 KiB per message |
| Complexity | higher (VMO lifecycle, restart) | lower |
| Isolation | consumers write control VMO (Option A; B is the target) | no shared writable memory |
| Reuse of LoLa | high | low (new binding) |

---

## 10. Impact on the current implementation

Legend: **U** unchanged · **A** needs an abstraction/refactoring (benefits Linux/QNX too) · **N** new Fuchsia code ·
**C** configuration/documentation.

### 10.1 By component

| Area (path) | Impact | What changes |
|---|---|---|
| `score/message_passing/fuchsia/` | **N (done)** | Backend, `Connector` FIDL, factories, echo tests. Hardening: `fix/fuchsia-registry-start` (`ea8396f9`), `fix/fuchsia-engine-writes` (`645b2a88`). Remaining: backend-neutral gtests, CI. |
| `score/message_passing/{engine,client_factory,server_factory}.h`, `BUILD` | **U/A (done)** | Backend selection. |
| `score/message_passing/unix_domain/` | **U** | Stays Linux-only. |
| `score/message_passing` identity API (`GetClientIdentity`, `ClientIdentity{pid,uid,gid}`) | **A (done, `400bc46b`)** | `ClientIdentity` is opaque (equality, hash, log string, `IsAllowed`). Known issue: `FromEndpoint` conflates node and principal ids (10.3, PR 1). |
| `score/memory/shared/` (`SharedMemoryResource`, `SharedMemoryFactory`, `LockFile`, `FlockMutex`) | **A + N** | Introduce/confirm an OS-neutral creation/open interface (`ISharedMemoryFactory` exists). `VmoSharedMemoryResource` (handle-based, with FEMU test) exists on `feat/vmo-shared-memory-resource` (`313b0f73`); the Linux-side seams (`MmanImpl::shm_open`, `Fcntl::flock`, ACL no-op) are proposed by the `spike/shm-on-fuchsia` spike (`d239a34b`, compile patches only). Create-or-open and lock-file logic must not be required on the VMO path. `UserPermissions` maps to handle rights. |
| `bindings/lola/skeleton_memory_manager.*`, `skeleton.cpp`, `proxy.cpp` (shm open/create) | **A** | Stop assuming "open by name"; go through an attach-style acquisition API that the POSIX implementation satisfies by name-opening and the Fuchsia implementation by handle exchange. |
| `bindings/lola/{partial_restart_path_builder,i_partial_restart_path_builder}.*`, usage marker + `flock` in `skeleton.cpp`/`proxy.cpp`, `service_data_storage.*`, `application_id_pid_mapping*`, `transaction_log_rollback_executor.*` | **A + N** | Introduce an incarnation authority and an *instance usage* interface (file+flock on Linux, broker leases on Fuchsia). `skeleton_pid_` becomes the epoch in the data VMO, `application_id_pid_mapping` becomes `application_id → incarnation`, and the rollback executor is refactored (provider-side rollback, claim word). Details and PR list: [10.3](#103-incarnation-aware-recovery-code-impact-and-pr-list). |
| `bindings/lola/service_discovery/` (`FlagFileCrawler`, inotify) | **A + N** | Reuse `IServiceDiscoveryClient`; add a FIDL-based client. Flag-file code stays for Linux/QNX. |
| `bindings/lola/messaging/` (`CreateMessagePassingName(asil, pid)`, `sender_pid`, allowed-uid checks) | **A** | Replace pid with the opaque node/peer id; allowed-consumer check goes through `ClientIdentity::IsAllowed`. Naming seeded by application id instead of pid on Fuchsia. |
| `bindings/lola/runtime.cpp` (runtime key = pid/uid) | **A** | Runtime key from application id / incarnation. |
| `bindings/lola/{event_data_control,event_slot_status,slot_collector,…}`, subscription state machine, local views, transaction log data structures | **U** | Atomics in shared memory; unaffected. |
| `impl/*_binding.h`, `impl/plumbing/*`, `BindingType` | **U (options A/B) / A (option C)** | With the generic-protocol recommendation no new `BindingType` is needed: Fuchsia is a *platform* variant of the LoLa binding. A separate binding type would only be needed if the data plane stopped being LoLa. |
| `impl/configuration/` (`allowed_consumer`, `allowed_provider`, `strict_permissions`, shm sizes) | **C + A** | Redefine meaning of consumer entries as *application ids*; define `strict_permissions` on Fuchsia; add deployment fields for realm generation. Parsing largely unchanged. |
| Realm/CML generation (new tool) | **N** | Generate capabilities, offers and discovery routes from the ECU model (`inc_ecu_model`, G5 in 6.10) or, until it is usable, from `mw_com_config.json`; build-time consistency test. |
| Interface/config generation (G2/G3, optional) | **N** | Generate `*_gen.h` and `mw_com_config.json` from the ECU model; independent of Fuchsia. |
| Broker component | **N (phase 6)** | Process-termination leases, epochs, VMO retention/fencing (6.5). |
| `bindings/lola/BUILD`, `score/memory/shared/BUILD` | **A** | `select()` on `@platforms//os:fuchsia` for the Fuchsia seam implementations; Fuchsia packages (`fuchsia_package`). |
| Tests | **A + N** | Many LoLa component tests are Linux-only; need Fuchsia ITF variants or backend-neutral tests. Add liveness/identity/shm-provider unit tests with mocks. New FEMU integration tests per seam. |
| CI | **N** | Fuchsia job modelled on `build_and_test_qnx.yml` (FEMU instead of QEMU+QNX). None exists today. |
| Docs, safety analysis, requirements | **C** | New platform description; update FMEA for the new failure modes (realm mismatch, broker, VMO handle leaks); trace identity changes. |

### 10.2 What stays untouched (important for risk)

The slot reservation/reference-counting algorithms, the subscription state machine, local views and the generic
proxy/skeleton framework remain as they are. **Rollback is not unchanged:** the transaction-log *rollback algorithm*
stays, but its executor, its trigger and who may run it change (10.3), and its re-runnability after a crash mid-rollback
is unproven (PR 0 characterizes it first). Other changes are at the edges where LoLa meets the OS.

### 10.3 Incarnation-aware recovery: code impact and PR list

Design-only (t05) except where noted. Status: PR 0-2 are on `feat/incarnation-pr0-2` (`3aca6e1b`); `feat/incarnation-pr3-7`
adds `pid_` made atomic (`6121c192`), the `kQuarantine` rollback policy (`67df8e4c`) and
`ApplicationIdPidMapping::Takeover`/`ReleaseIfOwner` with tests (`684915c2`). **PR 3** (about 60 mechanical files) is not done (the opaque-id part is in `feat/identity-opaque`,
6.4), **PR 4** depends on it, **PR 5** is blocked (no shm layout version; widening the 4-byte owner word breaks mixed-version
shm, `Takeover` currently asserts the incarnation fits in int32), **PR 7** (executor switch to `Takeover`) is not wired;
PRs 6, 8-13 are not started. Known race: two simultaneous *fresh* claimants of one app id both see `kFresh`. The pid is used as peer address, name seed, mapping key and liveness proof; each use gets its own type.

| Area | Change |
|---|---|
| New value types | `IncarnationId`, `SkeletonEpoch`, `NodeId`; `ClientIdentity` split into principal and node |
| `IIncarnationAuthority` | `Self()`, `Query()`, policy `kAlwaysTakeover` (Linux/QNX) / `kRequireDead` (Fuchsia), `SupportsProviderSideRollback()` (false on Linux); `IRuntime::GetIncarnation` |
| `messaging/` | pid → `NodeId` everywhere (mechanical, the largest change); `INodeAddressResolver`; `NotifyOutdatedNodeId` → `NotifyOutdatedIncarnation` (`kOutdatedIncarnation = 5`); stale-notification rules S1-S7 and a `recently_outdated_` ring |
| `ServiceDataStorage` | `skeleton_pid_` → epoch (published after `CleanupSharedMemoryAfterCrash`); layout version bump |
| `application_id_pid_mapping` | `ApplicationIdIncarnationMapping`: atomic 64-bit word, CAS `Takeover` (`kFresh/kSame/kTookOver/kRefusedLive/kContended`), `ReleaseIfOwner` |
| Rollback executor | refactored into steps R1-R5; claim word; provider-side rollback (off on Linux); `IConsumerRecovery` seam (`LogRollbackRecovery`, later `HoldRegionRecovery`) |
| Fuchsia platform | broker authority and resolver, rebirth/adoption wiring, fake-broker tests |

PR sequence (each keeps Linux/QNX behaviour): **0** characterization tests (incl. re-runnability of the rollback);
**1** value types + principal/node split (fixes the `FromEndpoint` conflation); **2** authority + `IRuntime::GetIncarnation`;
**3** messaging pid → `NodeId`; **4** resolver + outdated message; **5** epoch + layout bump; **6** incarnation mapping
(adjacent to 5: both change the shm layout); **7** executor refactor; **8** skeleton keying; **9** claim word +
provider-side rollback (off on Linux); **10** `IConsumerRecovery` seam; **11** optional hot-path epoch check (budget < 2 ns,
microbenchmark T30); **12** Fuchsia platform; **13** Option B (6.2.1).

Open points: re-runnability of `RollbackProxyTransactions`; a mixed-version decision and layout version; provider-side
rollback changes Linux liveness semantics; pid reuse on Linux is accepted; the `FromEndpoint` conflation.

### 10.4 Behavioural differences visible to integrators

* "Allowed consumer" is configured as an **application identifier** (and a realm route), not a uid.
* Shared memory is **not visible** in a filesystem; debugging tools that list `/dev/shm` do not apply.
* A consumer without a route is **not** rejected at startup: the component starts and the first FIDL call fails with
  `ZX_ERR_PEER_CLOSED` (measured, A.1). The LoLa client must map this to "service not available / not allowed".
* Without a broker, a skeleton restart is always a rebirth: proxies re-attach and re-subscribe.

---

## 11. Migration plan

| Phase | Deliverable | Depends on | Status / verification |
|---|---|---|---|
| **0** | Fuchsia SDK/FEMU in ITF; OS-abstraction parts of baselibs compile; message_passing FIDL backend; echo tests | – | Done; FEMU ITF tests reported passing (not re-run for this document) |
| **1** | Identity: opaque `ClientIdentity` (`400bc46b`, done); endpoint-derived identity, principal/node split (PR 1); `strict_permissions` definition | 0 | Unit tests; FEMU test with 2 consumers + 1 unrouted (A.1 covers routing) |
| **2** | Shared-memory seam: `VmoSharedMemoryResource` (`313b0f73`, done); `Attach`; attach-style acquisition on all platforms; replace `flock`/`shm_open` use (t08 seams) | 1 | Existing Linux shm tests; FEMU test: skeleton writes, proxy reads |
| **3** | Discovery: `ServiceRegistry`/`ServiceDiscovery` behind `IServiceDiscoveryClient` (6.3); E1-E10 experiments done (`5262d114`), `feat/fuchsia-attach` prototype | 2 | Discovery FEMU tests; Linux crawler tests unchanged |
| **4** | Incarnation PRs 0-11 (10.3) and Fuchsia rebirth-only restart without broker (PR 12 subset) | 2, 3 | Characterization tests first; crash/restart FEMU tests |
| **5** | Realm generator (G5) and typed marker (G4): prototype `exp/realm-generator` (`dba39624`); integration in `inc_ecu_model`; generator lowering for struct/enum/union (done on a clone, `8b6f41c`; upstreaming open); realm runs on FEMU (`6bf59dec`) | 1 | Build test; scale test |
| **6** | Broker: leases, epochs, fencing, adoption (PR 12) | 4 | Skeleton restart test with slot rollback |
| **7** | Option B per-consumer hold regions (PR 13) before any FFI claim between consumers | 6 | Benchmark, FEMU fault-injection |
| **8** | CI job (`ci/fuchsia-femu`, `0009b5db`, written; `/dev/kvm` on hosted runners **unverified**); backend-neutral gtests; safety documentation | all | CI green on FEMU |

Phases 1-4 are the critical path for a working "hello LoLa" on Fuchsia.

### 11.1 Follow-up work packages

Done since the first draft (Appendix D): Attach/discovery, rights, locks, broker + hardening, events transport, identity
split, datatype lowering, realm on FEMU, Option B prototype, E1-E10. Remaining:

1. **Incarnation PRs 3-13** (10.3): PR 3 (pid -> `NodeId`/`IncarnationId` across messaging, about 60 files), PR 4 (resolver,
   `kOutdatedIncarnation`), PR 5 (needs a shm layout version and a mixed-version decision), PR 7 (wire `Takeover` into the
   rollback executor), PRs 6, 8-13. Open decisions: enable `kQuarantine` in production, owner-word widening, dual-read.
2. **Broker ASIL profile:** trusted-ack for adoption (today the caller is trusted), scrub state, restart reconcile period,
   persistent or re-derived `last_epoch`, automatic `Reannounce` in `skeleton_main`, re-registration of proxies on a
   fenced incarnation, quarantine budget, proxy `application_id` from the routed endpoint rather than caller-asserted.
3. **Option B** on ARM64 (seq_cst/Dekker review and benchmark) and against the real LoLa classes, then LoLa integration
   (6.2.1); AoU into requirements and safety docs.
4. **Hook events into LoLa:** connect the notification transport (6.6) to the real skeleton/proxy event code; multi-slot
   ring, port-based subscribers, async pruning, real hardware.
5. **QNX `flock` path** and Linux regression of the patched baselibs (the Fuchsia seams `MmanImpl::shm_open`,
   `Fcntl::flock` and ACL no-op are in t08; `ILockProvider` exists on `feat/fuchsia-lock`).
6. **Generators:** upstream the lowering and the realm generator into `inc_ecu_model`; maps; typed marker use at runtime;
   realm scale beyond 20 apps with repetition; mutation tests for the negative routing checks.
7. **ASIL-B per quality:** one endpoint (and data+control pair) per quality level per provider (w02 deviation).
8. **Remaining experiments/tests:** re-export denial test with negative control and the FIDL rights lint (6.2.2); retest
   `source_instance_filter` + `renamed_instances`; proxy-death and multi-proxy tests with the real LoLa path; full 7.1 run.
9. **CI hardening:** KVM on hosted runners, SDK caching.
10. **Validation:** `validation_cli` reports 2 errors for `message_passing_architectural_design` (`static_design.puml` lines
    16/17); verified identical on the base commit, so pre-existing (warning only).

---

## 12. Risks and open questions

| # | Item | Risk / question | Mitigation |
|---|---|---|---|
| 1 | **`shm_open`/`flock` on Fuchsia libc** | Measured (t08): `shm_open` fails with ENOENT, `flock` with ENOTSUP; `memfd_create`/`mmap` work. `memory/shared` compiles only with compile patches (not on mainline). | Seams for `shm_open`/`flock`/ACLs; VMO resource (done); Linux regression of patches not run. |
| 2 | **Writable control VMO; no FFI between consumers** | A faulty admitted consumer can corrupt control state, starve slots or feed wild offsets; routing does not prevent it. | Baseline: per-ASIL control VMOs, rights, AoU-FUCHSIA-SHM-1…6, FMEA (Appendix B). Target: Option B (PR 13). No FFI claim until then. |
| 3 | **Rights reduction through FIDL** | Verified (`w07` E9, `99bac535`): more rights are reduced, fewer close the binding (whole channel). Remaining: re-export denial test and lint. | Rights-exactness and re-export tests (6.2.2); CI lint for bare `zx.Handle:VMO`; declare rights exactly. |
| 4 | **Broker design** | Implemented and FEMU-tested (6.5), but adoption trusts the caller, no ASIL profile, `last_epoch` lost on restart (boot clock), quarantine pressure not addressed. | Phase it; rebirth-only fallback; trusted-ack; quarantine budget; supervisor events. |
| 5 | **Discovery/endpoint behaviours (E1-E10)** | Measured (6.3). A routed but not-running provider makes the client *wait*; `source_instance_filter` + `renamed_instances` unreliable; single runs. | Client timeouts; per-consumer protocol capabilities; rerun per release. |
| 6 | **Rollback re-runnability and layout change** | Re-running an interrupted rollback is safe only at slot granularity; stuck logs otherwise (`kQuarantine` leaks at most one reference, `kFail` stays default). Epoch/mapping change the shm layout and wire format (mixed-version decision needed, PR 5 blocked). | `kQuarantine` decision; layout version; adjacent PRs 5/6. |
| 7 | **Provider-side rollback and pid reuse on Linux** | Provider-side rollback changes Linux liveness semantics (kept off); pid reuse on Linux is accepted. | Policy `kAlwaysTakeover`; documented. |
| 8 | **`ClientIdentity::FromEndpoint` conflates node and principal ids** | Split done on `feat/identity-split`/`feat/identity-opaque`; the wire format is still a pid. | Incarnation PRs 3-5. |
| 9 | **Routing cost at scale** | Fine up to 10,000 offers (A.3, single runs); compiled CML grows to MBs. | Subrealms; generator; rerun per release. |
| 10 | **Single FIDL async loop thread in the registry** | Throughput and isolation; writes now non-blocking, serialized, bounded queue, drop-with-EAGAIN (`645b2a88`): drops under load. | Measure; keep control traffic small. |
| 11 | **Notification-path cost** | About 5-7 µs per subscriber, O(m) (A.4; `feat/fuchsia-events` about 7 µs per extra subscriber); single slot, emulated, not hooked into LoLa. | Eventpairs for >= 4 KiB or >= 8 subscribers; ring/port experiments. |
| 12 | **Hot-path epoch check** | One acquire load per `GetNewSamples`; budget < 2 ns unmeasured. | PR 11, microbenchmark T30. |
| 13 | **Unrouted `use` detected late** | Connect succeeds, first call fails with `PEER_CLOSED` (A.1). | Treat as "not offered"; realm self-check. |
| 14 | **No instance filtering in CML** | A service capability exposes all instances (A.2). | Per-consumer protocol capabilities (this design). |
| 15 | **`inc_ecu_model` maturity** | Incubator; lowering done in a clone (`8b6f41c`, no maps), not upstream; no Fuchsia `NetworkKind`. | Start with G4/G5; upstream. |
| 16 | **`strict_permissions` meaning** | Needs a decision with safety/requirements owners. | Proposal in 6.4. |
| 17 | **Stable application ids** | Transaction-log rollback relies on them. | Configuration requirement; validate at attach (live twin rejected). |
| 18 | **ASIL aspects** | Component framework and FIDL stack not qualified by us. | Out of scope here. |
| 19 | **CI infrastructure** | `ci/fuchsia-femu` (`0009b5db`) written; `/dev/kvm` on hosted runners unverified; SDK size, caching (one FEMU run also hit "No space left on device" on `/mnt/data` during an SDK fetch). | Dedicated job; cache the SDK. |
| 20 | **Option B unmeasured where it matters** | Only x86 and a replica baseline; skeleton Allocate +36 % at 16 consumers; ARM64 ordering unreviewed. | ARM64 measurement; real-LoLa benchmark before any decision. |
| 21 | **One endpoint per quality** | One data+control pair per provider endpoint, so ASIL-B needs its own endpoint (`99bac535`). | Generator emits one endpoint per quality. |
| 22 | **Weak-test caveats** | Many FEMU results are single runs with passing output not captured (broker, events); negative routing checks lack mutation tests; flaky `reserving_skeleton_slots` seen once under load. | Repeat runs; add mutation tests. |

---

## 13. Status of this document: what is verified and what is not

**Implemented and verified on FEMU** (evidence = commits; echo tests are reported passing and were **not re-run** for
this document)

* Hermetic Fuchsia SDK and product bundle, FEMU ITF plugin and hello-world (`feat/fuchsia-femu-integration-test`).
* Only the OS abstraction, futurecpp and errno parts of `score_baselibs` compile for Fuchsia
  (`third_party/score_baselibs/fuchsia_support.patch`). `memory/shared` compiles only with the compile patches of
  `spike/shm-on-fuchsia` (`d239a34b`, `fuchsia_shm_support.patch`, `-frtti`); Linux regression of these not run; **lola is not built for Fuchsia**.
* `message_passing` Fuchsia backend over FIDL; in-process and cross-component echo tests; Linux unaffected.
* Hardening: `fix/fuchsia-registry-start` (`ea8396f9`), `fix/fuchsia-engine-writes` (`645b2a88`).
* `feat/vmo-shared-memory-resource` (`313b0f73`): `VmoSharedMemoryResource` with FEMU test; TRANSFER is stripped on open.
* `feat/opaque-client-identity` (`400bc46b`); the node/principal conflation is addressed by the later identity branches (below).
* t08 spike: `shm_open` ENOENT and `flock` ENOTSUP at runtime; `memfd_create`/`ftruncate`/`mmap(MAP_SHARED)`/`fchmod` work.
* `exp/realm-generator` (`dba39624`): 20 tests, `cmc`/`fidlc` verified on its output.
* `spike/fuchsia-fidl-parser` (`fe72722`): parser works; lowering is on `feat/datatype-generator-lowering` (`8b6f41c`, below).
* Experiments on `exp/fuchsia-cml-routing` (`023b6c77`, `42a73b23`, `45b8246e`): routing with `as`, no instance filtering, scale, fan-out benchmark (Appendix A; limited runs).

**Implemented and verified on FEMU, branches local and not pushed** (full table: [Appendix D](#d-branches-and-commits); all
single runs unless noted; "verified" means the stated test passed, not that the feature is complete)

* Locks: `feat/fuchsia-lock` (`fed9fe49`): `ILockProvider`, eventpair-bound registry locks incl. holder death; not a broker.
* Attach/discovery/rights: `feat/fuchsia-attach` (`99bac535`): `score_mw_com.fidl`, per-caller discovery, `Attach` without application id, rights reduction on receive, `TRANSFER` absent.
* Broker: `feat/fuchsia-broker` (`5ad56a4b`), `feat/fuchsia-broker-hardening` (`1571950c`): leases, epoch, tombstone, fencing, reannounce, 100-cycle stress, no leaked handles; adoption trusts the caller, no ASIL profile.
* Events: `feat/fuchsia-events` (`2fba0b66`): VMO data + per-subscriber eventpair wake-up; transport only, not in LoLa.
* Identity: `feat/identity-split` (`9a86db64`), `feat/identity-opaque` (`c1e6187c`): host + FEMU `message_passing` tests; wire format still pid.
* Realm: `exp/realm-generator-femu` (`6bf59dec`): generated realm runs (8 and 20 apps), per-caller discovery, unrouted consumer blocked.
* Option B: `exp/option-b-hold-regions` (`efdd3725`): host + FEMU fault injection; x86 benchmark only.
* Experiments E1-E10: `exp/fuchsia-open-questions` (`5262d114`).
* Regression proof: the two hardening fixes fail their new tests on the old code (`connection_registry_test` FAILED, `stalled_reader_test` TIMEOUT) and pass on `ea8396f9`/`645b2a88`.

**Verified on host only:** `feat/datatype-generator-lowering` (`8b6f41c`, struct/enum/union/array lowering, fixed-layout
validator, no maps); `feat/incarnation-pr0-2` (`3aca6e1b`) and `feat/incarnation-pr3-7` (`684915c2`: atomic `pid_`,
`kQuarantine`, `Takeover`; `//score/...` suites pass except one `reserving_skeleton_slots` flake that passed in isolation).

**Written but unverified:** `ci/fuchsia-femu` (`0009b5db`): `/dev/kvm` on hosted runners not verified.

**Design only (not built, not measured):** the re-export denial test and FIDL rights lint, Appendix C tests not listed above,
Option B inside LoLa, rebirth/adoption wiring in LoLa (the broker only reannounces), scrub state, ASIL profile of the
broker, incarnation PRs 3-13, hooking events into LoLa, QNX `flock` path, realm integration in `inc_ecu_model`.
**lola is still not built for Fuchsia.** CML snippets are illustrative unless Appendix A says otherwise.
**Not caused by these changes:** the 2 `validation_cli` errors for `message_passing_architectural_design` also occur on the base commit.

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
| **Epoch** | Strictly increasing per-instance skeleton incarnation number in the read-only data VMO header. |
| **Lease** | Broker-issued incarnation id of a process; `DEAD` on `ZX_PROCESS_TERMINATED`. |
| **Rebirth** | New VMOs per skeleton incarnation (default); adoption of old VMOs only if all leases are dead. |
| **Broker** | Optional component: leases, epochs, fencing, VMO retention. |
| **FEMU** | Fuchsia emulator (QEMU-based) used for integration tests. |
| **ITF** | The repo's integration test framework. |

---

## A. Experiment results

Measured on FEMU (SDK 33.20260919, KVM, `ffx test run`). Code and raw logs are in the experiment branch
`exp/fuchsia-cml-routing` under `experiments/fuchsia_cml/` (commits `023b6c77`, `42a73b23`; benchmark redone in
`45b8246e`). Absolute times come from an emulated guest and are indicative only. A.1-A.3 are single runs; A.4 has 10
runs per cell.

### A.1 Per-consumer capabilities renamed with `as` (works)

Verified CML: the provider declares and exposes `svc.for_app_a` and `svc.for_app_b`; the realm offers each to one
consumer with `as: "svc.consumer"` (nothing to app_c/app_d); each consumer has `use: [ { protocol: "svc.consumer" } ]`.

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

cmc on the pinned SDK rejects `source_instance_filter`, `renamed_instances` and `filter` in `offer`/`expose`; with a
plain `service` offer a consumer sees **all** instances. Workarounds: one protocol capability per consumer (A.1, our
design) or one provider per instance. Dynamic offers via `Realm.CreateChild`: **not tested**.

### A.3 Scale (N apps × M per-consumer capabilities)

Single runs, all connections verified by identity, guest monotonic time.

| config | offers | first connect+ping mean / max (µs) | repeated connect+ping mean / p99 (µs) | compiled CML root / provider |
|---|---|---|---|---|
| 1×1 | 1 | 441 / 441 | 112 / 333 | 3 KB |
| 30×50 | 1,500 | 2,401 / 4,586 | 143 / 357 | 417 KB / 452 KB |
| 100×100 | 10,000 | 8,936 / 20,465 | 172 / 460 | 2.7 MB / 3.1 MB |

No cmc or component-manager limit was hit up to 10,000 offers; steady-state connect+ping stays at about 110-170 µs
regardless of the number of offers (routing is not on the hot path); the first connect grows with realm size and
concurrent starts; compiled manifests reach MBs, which supports generating the realm and using subrealms.

### A.4 Notification-path benchmark: FIDL vs shared VMO

Publisher plus m subscriber components (separate processes), FEMU with 24 vCPUs (plus an oversubscription run on 4
vCPUs), 10 runs per cell, single slot. Variants: `fidl` (one one-way message per subscriber, payload in a vector),
`vmo_poll` (subscribers spin on an atomic sequence), `vmo_eventpair` (one eventpair per subscriber signalled by the
publisher), `vmo_event` (one shared event; futexes cannot be used across processes). Mean values in µs.

Publisher cost:

| payload | m | FIDL | poll | eventpair | event |
|---|---|---|---|---|---|
| 64 B | 1 | 13.3 | 0.13 | 6.5 | 7.5 |
| 64 B | 4 | 50 | – | 24.7 | 21 |
| 64 B | 16 | 183 | – | 75 | 110 |
| 4 KiB | 1 | 96 | – | 7.9 | 6.8 |
| 4 KiB | 4 | 375 | – | 26.7 | 24.8 |
| 4 KiB | 16 | 1520 | – | 90 | 114 |
| 60 KiB | 1 | 1257 | 11.6 | 14.3 | 16 |
| 60 KiB | 4 | 4627 | – | 30 | 26.8 |
| 60 KiB | 16 | 14539 | 14.4 | 102 | 119 |

End-to-end latency:

| payload | m | FIDL | poll | eventpair | event |
|---|---|---|---|---|---|
| 64 B | 1 | 28 | 0.65 | 20 | 23 |
| 64 B | 16 | 124 | – | 55 | 130 |
| 4 KiB | 16 | 819 | 3.0 | 64 | 132 |
| 60 KiB | 16 | 8244 | 22 | 78 | 137 |

* Notification costs about 5-7 µs per subscriber (O(m), not O(1)); eventpair beats the shared event from m >= 8.
* **Conclusion:** shared VMO + per-subscriber eventpair for payloads >= 4 KiB or >= 8 subscribers; FIDL is competitive for
  small payloads and <= 4 subscribers; **polling is opt-in** for dedicated cores (4 vCPUs: p99 7 ms, delivery as low as
  25-60 %, FIDL 100 %).
* Caveats: emulation inflates notification costs, the host was shared and loaded, single slot (a late subscriber loses
  samples; a real design needs a ring). Not covered: port-based subscribers, multi-slot rings, real hardware.
* These numbers supersede the earlier single-run benchmark (polling only, no notification cost).

### A.5 Not tested

* Non-eager start with an unrouted required `use` is covered (immediate `PEER_CLOSED`); `renamed_instances` with instance filters is unreliable (E8, see 6.3).
* Scale tests with all apps looping beyond 30 apps; realm generator beyond 20 apps.
* Port-based subscribers and multi-slot rings in the notification path.
* Option B on ARM64 and against the real LoLa classes.

Later FEMU experiments (E1-E10, `exp/fuchsia-open-questions`, `5262d114`) are summarised in 6.3 and 6.5. Further results of E6:
`socket(AF_UNIX)` fails (`EPROTONOSUPPORT`) but `socketpair(AF_UNIX)` works (no `SO_PEERCRED`); `AF_INET` needs
`fuchsia.posix.socket.Provider`; `flock`, `fcntl` locks, inotify, `mkfifo`, `shm_open` and named semaphores are unavailable;
there is no writable `/tmp`. So "no named `AF_UNIX`" is the correct statement, not "no `AF_UNIX`".

---

## B. FMEA summary

Condensed from the trust analysis (design-only, not yet in the project FMEA). Option A = writable control VMO.

| ID | Failure mode | Main mitigation |
|---|---|---|
| FM-01 | Admitted consumer writes arbitrary data into the control region (corrupt refcounts, `OffsetPtr`) | Skeleton checks, `ignore_qm_control_`-style detection; Option B; AoU-1/2 under Option A |
| FM-02 | Consumer holds slots beyond its limit | Popcount limit, evict on watchdog |
| FM-03 | Consumer re-exports a VMO handle to an unrouted component | Strip TRANSFER/DUPLICATE/WRITE, rights tests, AoU-4 |
| FM-04 | Consumer forwards its per-consumer endpoint | Single attach per endpoint, AoU-4; residual: channel rights not restrictable |
| FM-05 | Realm CML differs from the mw::com configuration | Generate CML from the configuration, CI diff gate, strict mode |
| FM-06 | Broker failure or compromise | Minimal broker, fail-static data plane, rebirth fallback |
| FM-07 | Stale epoch: VMOs of a dead incarnation still used | Epoch in read-only data VMO, liveness eventpair, re-attach |
| FM-08 | Consumer resizes a writable VMO | Non-resizable VMOs, `REQUIRE_NON_RESIZABLE` |
| FM-09 | Mixed-integrity consumers share one control VMO | One control VMO per ASIL level; `Attach` rejects mismatch |

## C. Test matrix summary

Design matrix. Parts have since been run (see 6.2.2, 6.3, 6.5, Appendix D): rights, process/liveness death, epoch
monotonicity and fencing, broker death and restart, identity types, discovery and Option B store+validate vs CAS (x86
only). Not run: characterization of rollback re-runnability beyond slot granularity, rebirth/adoption with scrub, claim
word, hot-path epoch check. Layers: **U** unit (fake broker/authority), **F** FEMU integration, **B**
benchmark, **E** experiment.

| Group | Tests | Layer |
|---|---|---|
| Characterization (PR 0) | existing rollback/partial-restart behaviour, rollback re-runnability | U |
| Rights (6.2.2) | rights equality, unrouted re-export denied (with negative control), write attempts fail, FIDL rights lint | U/F |
| Leases and epoch | process death marks lease dead, epoch monotonic, fencing order, header publish order | U/F |
| Rebirth and adoption | rebirth default, adoption only if all leases dead, scrub crash resumes, quarantine budget | U/F |
| Rollback | claim word transitions, single executor, takeover of dead executor, live twin rejected | U |
| Broker death | fail-static, restart reconcile, both-gone rebirth | F |
| Identity | endpoint-derived identity, no application id parameter, principal/node split | U/F |
| Discovery | E1-E10 (6.3), allow-table filtering | F/E |
| Performance | hot-path epoch check < 2 ns (T30), Option B store+validate vs CAS | B |

---

## D. Branches and commits

All branches below are **local and unpushed** except this document's branch. Short hashes as reported by the work items.
"Host" = Bazel unit tests; "FEMU" = ITF test on the emulator (single runs).

| Branch | Commit | Content | Evidence |
|---|---|---|---|
| `feat/fuchsia-lock` | `fed9fe49` | `ILockProvider`, `PosixLockProvider`, Fuchsia `RegistryLockProvider` (eventpair-bound) | FEMU lock test incl. holder death |
| `feat/fuchsia-attach` | `99bac535` | `score_mw_com.fidl`, `Attach` without application id, per-caller discovery, rights checks | FEMU `fuchsia_attach_itf_test` |
| `feat/fuchsia-broker` | `5ad56a4b` | lease broker, epoch, tombstone, liveness + process watch | host 10 cases; FEMU `fuchsia_broker_itf_test` |
| `feat/fuchsia-broker-hardening` | `1571950c` | `Reannounce`/adoption, lease leak fix, 100-cycle stress | host 12 cases; FEMU 414 checks |
| `feat/fuchsia-events` | `2fba0b66` | `EventChannel` removed; notification eventpair | FEMU attach suite 4/4 (SMP=8) |
| `feat/identity-split` | `9a86db64` | node vs principal split | host/FEMU |
| `feat/identity-opaque` | `c1e6187c` | opaque `NodeId`/`PrincipalId` | host 227 pass (3 skipped); FEMU `message_passing_itf_test` |
| `feat/incarnation-pr0-2` | `3aca6e1b` | incarnation PR 0-2 | host; `validation_cli` 2 errors pre-existing |
| `feat/incarnation-pr3-7` | `684915c2` (`6121c192`, `67df8e4c`) | atomic `pid_`, `kQuarantine`, `Takeover`/`ReleaseIfOwner`; PR 3/4/5/7 not done | host 226/233 (1 flaky, passes isolated) |
| `feat/datatype-generator-lowering` | `8b6f41c` | struct/enum/union/array lowering, `FixedLayoutValidator` (`inc_ecu_model` clone) | host; no maps |
| `exp/realm-generator-femu` | `6bf59dec` | generated realm on FEMU (8 and 20 apps), generator fixes | FEMU; 29 Python tests |
| `exp/fuchsia-open-questions` | `5262d114` | E1-E10 experiments | FEMU, single runs |
| `exp/option-b-hold-regions` | `efdd3725` | Option B prototype and benchmark | host 10/10, TSan; FEMU; x86 benchmark |
| fix branches | `ea8396f9`, `645b2a88` | registry start, engine writes | new tests fail on old code, pass on fix (FEMU) |
| earlier | `400bc46b`, `313b0f73`, `dba39624`, `fe72722`, `0009b5db`, `023b6c77`, `42a73b23`, `45b8246e`, `d239a34b` | see section 13 | see section 13 |

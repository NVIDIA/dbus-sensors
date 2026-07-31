# nvidia-fw-write-protect — Design

---

## 1. Overview

The `nvidia-fw-write-protect` service manages firmware write-protection state
on OpenBMC systems. It reads write-protection signals (GPIO lines, D-Bus
properties), computes effective protection state through a reactive graph, and
publishes the results on D-Bus objects consumed by bmcweb for Redfish.

The service supports two Redfish representations:

| Redfish resource | D-Bus interface | Use case |
|---|---|---|
| `NvidiaWriteProtectDomain` | `com.nvidia.Software.WriteProtection` | Per-domain write protection with `SetWriteProtected` |
| `NvidiaChassis.HardwareWriteProtectedControl` | `com.nvidia.State.HardwareWriteProtectedControl` | Chassis-level hardware write-protect signal |

Both representations share the same underlying mechanism: sources feed a
reactive graph whose outputs drive D-Bus properties on firmware inventory
objects.

---

## 2. Architecture

### 2.1 Why a Reactive Graph

Write-protection state is a dataflow problem: input signals flow through a
boolean computation and produce outputs that drive D-Bus properties. A reactive
graph models this declaratively — the topology is wired once from
configuration, and propagation is automatic.

This replaces the previous imperative model which had several structural
issues:

- **Split ownership** — GPIO protectors created and updated their own D-Bus
  objects, while domains maintained separate software component maps with a
  different update path. There was no single source of truth for which objects
  were authoritative.
- **Hardcoded single global override** — The global write-protect GPIO was a
  special case baked into the manager. Adding a second chassis-level signal or
  sharing a signal across a subset of domains required new code paths.
- **No support for chassis-only platforms** — The domain class inherited a
  fixed D-Bus interface (`WriteProtection`), so platforms needing only
  `HardwareWriteProtectedControl` without discrete domains could not reuse the
  same mechanism.
- **Blocking source initialization** — Protector creation (GPIO retries, D-Bus
  proxy detection) ran inline during group setup, delaying all D-Bus object
  creation by up to 25 seconds per unavailable source.

The reactive graph addresses these by separating concerns:

- **Sources** are pure readers — they set a graph node value and nothing else.
- **Compute nodes** (OR/AND) combine inputs declaratively.
- **Observers** are callbacks that update D-Bus properties when a node's output
  changes. The graph has no knowledge of D-Bus; observers handle that.

Adding a new Redfish representation is just a new observer. Sharing a source
across groups is just connecting one node to multiple compute nodes.
Chassis-only platforms and domain-based platforms use the same graph engine
with different D-Bus facades wired as observers.

### 2.2 Graph Structure

All state propagation flows through a directed acyclic graph of boolean nodes.

```
Source nodes (inputs)         Compute nodes           Observers (outputs)
┌──────────────────┐
│ GPIO / D-Bus     │──┐
│ proxy value      │  │    ┌──────────┐
└──────────────────┘  ├───→│ OR node  │───→ Domain.WriteProtected
┌──────────────────┐  │    │          │───→ SoftwareComponent.WriteProtected
│ GPIO / D-Bus     │──┘    └──────────┘
│ proxy value      │
└──────────────────┘
```

- **Source nodes** hold boolean values set by protector monitors.
- **Compute nodes** combine inputs with a boolean operator (OR or AND).
- **Observers** are callbacks subscribed to a node; they fire only when the
  node's output actually changes after propagation.

Propagation is synchronous and runs on the single-threaded async context.
Sources are deduplicated by identifier — the same GPIO line or D-Bus path
appearing in multiple groups creates one source node connected to multiple
compute nodes.

### 2.3 Non-Blocking Source Initialization

Source graph nodes are created immediately when a group is configured.
Protector construction (GPIO retries, D-Bus proxy detection) runs in a
background coroutine. Groups and their D-Bus objects appear on the bus without
waiting for sources to come online.

When a source's protector becomes available, the background coroutine reads its
initial value, updates the graph, and calls `propagate()` to push the value to
all connected observers. If the protector fails to initialize (e.g., GPIO line
does not exist), the source node remains at its default value (`false`) and a
warning is logged.

### 2.4 Component Diagram

```
┌─────────────────────────────────────────────────────────────────────┐
│                        DomainManager                                │
│                                                                     │
│  ┌───────────┐    ┌───────────────────────────────────────────────┐ │
│  │ Entity    │    │              Graph                            │ │
│  │ Manager   │───→│  S0 ──┐                                      │ │
│  │ Interface │    │       ├─ OR ──→ [Domain WP] [SW_0 WP] ...    │ │
│  └───────────┘    │  S1 ──┘                                      │ │
│                   │  S0 ────── OR ──→ [HW WP Ctrl]               │ │
│  ┌───────────┐    └───────────────────────────────────────────────┘ │
│  │ Source    │                                                      │
│  │ monitors  │    Protector::changed() → graph.set() → propagate() │
│  └───────────┘                                                      │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.5 Key Classes

| Class | Responsibility |
|---|---|
| `DomainManager` | Reads Entity Manager config, builds the graph, creates D-Bus facades, spawns source monitors |
| `Graph` | Reactive boolean DAG with source nodes, compute nodes, observers, and cycle detection |
| `Domain` | D-Bus facade for `com.nvidia.Software.WriteProtection` and `xyz.openbmc_project.Association.Definitions`; delegates `SetWriteProtected` to `DomainManager`; optionally creates chassis association for `WriteProtectDomain` configs |
| `HardwareWriteProtectedControl` | D-Bus facade for `com.nvidia.State.HardwareWriteProtectedControl` and `xyz.openbmc_project.Inventory.Item.Chassis` |
| `SoftwareComponent` | D-Bus object at `/xyz/openbmc_project/software/<name>` exposing `xyz.openbmc_project.Software.Settings` |
| `Protector` | Abstract interface for reading and writing protection state (`get`, `set`, `changed`) |
| `Gpio` | Protector backed by a gpiod line with edge-event monitoring |
| `DbusProxy` | Protector backed by a remote D-Bus object with PropertiesChanged monitoring |

---

## 3. Configuration

### 3.1 Entity Manager Config Types

The service watches for four Entity Manager configuration interfaces:

- `xyz.openbmc_project.Configuration.WriteProtectGroup`
- `xyz.openbmc_project.Configuration.HardwareWriteProtectedControl`
- `xyz.openbmc_project.Configuration.WriteProtectDomain`
- `xyz.openbmc_project.Configuration.WriteProtectInput`

The first three are "group" types that share the same properties and differ
in the D-Bus facade they create.  `WriteProtectInput` is a separate config
type for GPIO-backed sources.

#### WriteProtectGroup

Defines a write-protection group that combines sources with a boolean
operator.  No D-Bus facade is created for the group itself; it exists as a
reusable building block that other groups can reference by name.

| Property | Type | Required | Description |
|---|---|---|---|
| `Name` | string | yes | Unique group identifier |
| `Sources` | string[] | yes | Source references (see §3.2) |
| `FlashProtectedComponents` | string[] | no | Firmware inventory names; the service creates `xyz.openbmc_project.Software.Settings` objects at `/xyz/openbmc_project/software/<name>` |
| `SourceMode` | string | no | Boolean operator for combining sources: `"OR"` (default) or `"AND"` |

#### HardwareWriteProtectedControl

Defines a write-protection group that creates a
`HardwareWriteProtectedControl` D-Bus object at the parent path (chassis)
exposing `com.nvidia.State.HardwareWriteProtectedControl` and
`xyz.openbmc_project.Inventory.Item.Chassis`.

| Property | Type | Required | Description |
|---|---|---|---|
| `Name` | string | yes | Unique group identifier |
| `Sources` | string[] | yes | Source references (see §3.2) |
| `FlashProtectedComponents` | string[] | no | Firmware inventory names |
| `SourceMode` | string | no | Boolean operator: `"OR"` (default) or `"AND"` |

#### WriteProtectDomain

Creates a `Domain` D-Bus object at
`/xyz/openbmc_project/state/<Name>` exposing
`com.nvidia.Software.WriteProtection` and
`xyz.openbmc_project.Association.Definitions` with a
`write_protect_domains` / `chassis` association linking the chassis (parent
path) to the domain.  The first source becomes settable via
`SetWriteProtected`.

| Property | Type | Required | Description |
|---|---|---|---|
| `Name` | string | yes | Unique group identifier |
| `Sources` | string[] | yes | Source references (see §3.2) |
| `FlashProtectedComponents` | string[] | no | Firmware inventory names |
| `SourceMode` | string | no | Boolean operator: `"OR"` (default) or `"AND"` |

The association is defined on the domain object as:
- Forward: `chassis` (domain → chassis)
- Reverse: `write_protect_domains` (chassis → domain)

#### WriteProtectInput

Defines a GPIO-backed write-protection source.  Creates both a graph source
node and a group entry so that other groups can reference it by name.

| Property | Type | Required | Description |
|---|---|---|---|
| `Name` | string | yes | Unique identifier; other groups reference this name in their `Sources` |
| `LineName` | string | yes | GPIO line name passed to gpiod |
| `ActiveLow` | bool | no | If `true`, invert the GPIO value (default `false`) |
| `FlashProtectedComponents` | string[] | no | Firmware inventory names |
| `PollInterval` | uint64 | no | Poll interval in milliseconds; when set, the GPIO is polled instead of using edge interrupts |

### 3.2 Source Resolution

Each entry in a group's `Sources` array is a plain name.  The service resolves
each name at group creation time using the following rules:

1. **Existing group** — If a group with that name already exists, its output
   node is connected directly into the new group's compute node in the graph.
   No D-Bus round-trip occurs.
2. **Pending entity** — If no group exists yet, the name is recorded as a
   pending entity link.  Two things may happen:
   - A group with that name appears later (via `WriteProtectInput` or another
     group config).  The group's output node is wired in and any in-flight
     D-Bus proxy initialization for this entity is cancelled.
   - The D-Bus object at `/xyz/openbmc_project/software/<name>` appears.  A
     `DbusProxy` protector is created to monitor its `WriteProtected` property.

GPIO sources are not specified inline in `Sources`.  Instead, define a
separate `WriteProtectInput` Entity Manager config (see §3.1) and reference
its `Name` from the group's `Sources` array.

#### Deduplication

Sources are deduplicated by name.  If two groups reference the same source
name, one graph source node and one monitor coroutine are shared.

### 3.3 Associations

- **Chassis ↔ domains** (`write_protect_domains` / `chassis`): Managed by
  this service for `WriteProtectDomain` configs. The domain object publishes
  `xyz.openbmc_project.Association.Definitions` with the association
  automatically. For `WriteProtectGroup` configs, chassis associations are
  not created (define them externally in Entity Manager if needed).
- **Affected components → domains**: Define `write_protected_by` /
  `write_protects` associations on each affected entity. For Entity Manager
  top-level entities, use `xyz.openbmc_project.Association.Definitions`. For
  nsmd Expose records, use the Expose-level `Associations` array.

The ObjectMapper creates bidirectional endpoints automatically.

### 3.4 Example: Platform with Domains and Hardware Write Protect

Entity Manager chassis config (`hgxr_chassis_base.json`):

```json
{
    "Name": "HGX_Chassis_0",
    "Type": "Chassis",
    "Exposes": [
        {
            "Name": "GLOBAL_WP",
            "Type": "WriteProtectInput",
            "LineName": "GLOBAL_WP"
        },
        {
            "Name": "HGX_HW_WriteProtect",
            "Type": "HardwareWriteProtectedControl",
            "Sources": ["GLOBAL_WP"]
        },
        {
            "Name": "Domain_0_GPU_SPI_WriteProtect",
            "Type": "WriteProtectDomain",
            "Sources": [
                "Domain_0_GPU_SPI_WriteProtect",
                "HGX_HW_WriteProtect"
            ],
            "FlashProtectedComponents": [
                "HGX_FW_GPU_0",
                "HGX_FW_GPU_1"
            ]
        },
        {
            "Name": "HMC_SPI_WriteProtect",
            "Type": "WriteProtectDomain",
            "Sources": ["HGX_HW_WriteProtect"],
            "FlashProtectedComponents": ["HGX_FW_ERoT_BMC_0"]
        }
    ]
}
```

The `GLOBAL_WP` input reads the GPIO once.  The
`HGX_HW_WriteProtect` group references it by name, and domain groups
reference the HW group by name — connecting output nodes directly in the
graph.  The GPIO is not read multiple times, and a single `propagate()` call
updates the chassis property and all domain facades atomically.

The `WriteProtectDomain` type automatically creates `write_protect_domains` /
`chassis` associations between the chassis and each domain, so no separate
`Association.Definitions` config is needed on the chassis for these.

Affected entity config (e.g., `hgxr_gpu_chassis.json` Expose record):

```json
{
    "Name": "GPU_$INSTANCE_NUMBER Processor",
    "Type": "NSM_Processor",
    "Associations": [
        {
            "Forward": "write_protected_by",
            "Backward": "write_protects",
            "AbsolutePath": "/xyz/openbmc_project/state/Domain_0_GPU_SPI_WriteProtect"
        }
    ]
}
```

### 3.5 Example: Chassis-Only Hardware Write Protect (No Domains)

For platforms that expose only `HardwareWriteProtectedControl` without
discrete domains:

```json
{
    "Name": "HGX_ProcessorModule_0",
    "Type": "Chassis",
    "Exposes": [
        {
            "Name": "WP_HW_CTRL_N",
            "Type": "WriteProtectInput",
            "LineName": "WP_HW_CTRL_N",
            "FlashProtectedComponents": ["HGX_FW_GPU_0", "HGX_FW_GPU_1"]
        },
        {
            "Name": "HW_WriteProtect",
            "Type": "HardwareWriteProtectedControl",
            "Sources": ["WP_HW_CTRL_N"]
        }
    ]
}
```

No domain D-Bus objects are created.  The GPIO input drives both the chassis
`HardwareWriteProtectedControl` property and the firmware inventory
`WriteProtected` properties.

---

## 4. Control Flow

### 4.1 Startup

1. `main()` creates the `sdbusplus::async::context`, D-Bus managers, and
   `DomainManager`.
2. `DomainManager::start()` creates an `EntityManagerInterface` watching for
   `WriteProtectGroup`, `HardwareWriteProtectedControl`,
   `WriteProtectDomain`, and `WriteProtectInput` configs.
3. `handleInventoryGet()` queries existing Entity Manager inventory.
4. For each matching config, `processInventoryAdded` dispatches to
   `addGpioGroup` (for `WriteProtectInput`) or `addGroup` (for the other
   three types).

### 4.2 addGroup

1. Read config properties from Entity Manager (`GroupConfig::tryFrom`).
2. Create a compute node with the configured boolean operator (OR or AND).
3. For each source name:
   - If a group with that name already exists, connect its output node
     directly.
   - Otherwise, call `getOrCreateSource` (creates a graph source node if new)
     and record the name as a pending entity link.  If the source is new,
     spawn background D-Bus proxy initialization (`spawnEntityInit`).
4. Based on the EM config type, create the D-Bus facade:
   - `WriteProtectDomain` → `Domain` at `/xyz/openbmc_project/state/<Name>`
     with chassis association.
   - `HardwareWriteProtectedControl` → `HardwareWriteProtectedControl` at
     the chassis (parent) path.
   - `WriteProtectGroup` → no facade; exists only as a reusable graph
     building block.
   Subscribe the facade (if any) as an observer on the compute node.
5. For each `FlashProtectedComponents` entry, create a `SoftwareComponent`
   D-Bus object and subscribe it as an observer.
6. Resolve any pending entity links from other groups waiting for this one.

### 4.2.1 addGpioGroup

1. Read config properties from Entity Manager (`GpioInputConfig::tryFrom`).
2. Create a graph source node — or, if a group processed earlier already
   reserved a placeholder node under this name while resolving its
   `Sources` list, adopt that node (it is what every referencing group is
   wired to).  Group and input configs are therefore correct in either
   dispatch order.  If a D-Bus proxy protector already claimed the
   adopted source, an error is logged: the proxy cannot be safely
   replaced while its monitor coroutine may be suspended inside it, so
   the GPIO line will not be opened.
3. Register the source as a group (using the source node directly as the
   output node) so other groups can reference it by name.
4. Create `SoftwareComponent` objects for any `FlashProtectedComponents`.
5. Resolve pending entity links, cancelling any in-flight D-Bus proxy
   initialization for this name.  Adopted placeholder links are kept
   live rather than rewired.
6. Spawn GPIO protector initialization.  Initialization is skipped with
   a warning if the source already has a protector.

### 4.3 Source Initialization (Background)

Each GPIO or entity source is initialized asynchronously:

- **GPIO** (`initializeGpioSource`): Calls `Gpio::create` with the
  configured line name, polarity, and optional poll interval.
- **Entity** (`initializeEntitySource`): Calls `DbusProxy::create` at
  `/xyz/openbmc_project/software/<name>`.  Runs inside a cancellable
  `async_scope` so that `resolveGroupLinks` can abort it if the entity
  resolves as a local group instead.

Common steps after protector creation (`activateSource`):

1. If creation fails, log a warning and return.  The source node stays at its
   default value.
2. Read the initial value via `protector->get()`.
3. Update the graph: `graph.set(sourceNode, value)`, `graph.propagate()`.
4. Spawn the monitor coroutine.

### 4.4 Source Monitoring

Each source has a dedicated coroutine that loops:

1. `co_await protector->changed()` (blocks until the underlying value changes).
2. `graph.set(sourceNode, newValue)`.
3. `graph.propagate()` — recomputes downstream nodes and notifies observers
   whose output changed.

### 4.5 SetWriteProtected

1. Client calls `SetWriteProtected(value)` on a domain D-Bus object.
2. `Domain::method_call` delegates to `DomainManager::setWriteProtected`.
3. The manager looks up the source protector by name and calls
   `protector->set(value)`.
4. On success, `graph.set(sourceNode, value)` and `graph.propagate()` update
   all observers (including the domain's own `WriteProtected` property and all
   associated `SoftwareComponent` objects).

---

## 5. Error Handling

| Scenario | Behavior |
|---|---|
| Config property read fails | `tryFrom` catches the exception, logs the error, returns `nullopt`; group is skipped |
| Source build fails (GPIO not found, D-Bus object unavailable) | Logged as warning; source node stays at default (`false`); group is still created with remaining sources |
| All sources fail for a group | OR/AND node output defaults to the operator's identity (`false` for OR, `true` for AND); D-Bus objects are still created |
| `protector->changed()` returns error | Logged; monitor continues to next iteration |
| `protector->set()` fails during SetWriteProtected | Logged; graph is not updated; domain state unchanged |
| GPIO line not found | Retried 5 times at 5 s intervals; if still missing, source initialization fails |
| D-Bus proxy target not present | Waits for `InterfacesAdded` signal indefinitely until the object appears |
| Entity resolves as group during D-Bus wait | In-flight `DbusProxy::create` is cancelled via `async_scope::request_stop()`; group output node is wired in instead |
| `DomainManager::start()` called twice | Throws `std::runtime_error` |

All async operations use `upon_error` handlers to catch and log exceptions
without terminating the process.

---

## 6. D-Bus Objects

The service runs under the well-known name `com.nvidia.fwwriteprotect`.

### 6.1 Object Summary

| Object path | Interfaces | Created when |
|---|---|---|
| `/xyz/openbmc_project/state/<Name>` | `com.nvidia.Software.WriteProtection`, `xyz.openbmc_project.Association.Definitions` | Type = `WriteProtectDomain` |
| Chassis parent path (e.g., `.../HGX_Chassis_0`) | `com.nvidia.State.HardwareWriteProtectedControl`, `xyz.openbmc_project.Inventory.Item.Chassis` | Type = `HardwareWriteProtectedControl` |
| `/xyz/openbmc_project/software/<name>` | `xyz.openbmc_project.Software.Settings` | For each entry in `FlashProtectedComponents` (any config type) |

### 6.2 Interface Details

**`com.nvidia.Software.WriteProtection`** (on Domain objects)

| Member | Type | Description |
|---|---|---|
| `WriteProtected` | property (bool) | Effective write-protection state (combined via the group's boolean operator) |
| `SetWriteProtected(bool)` | method | Sets the first settable source; returns `NotAllowed` if no settable source exists |

**`com.nvidia.State.HardwareWriteProtectedControl`** (on chassis objects)

| Member | Type | Description |
|---|---|---|
| `WriteProtectedControl` | property (bool) | Current state of the hardware write-protection signal |

**`xyz.openbmc_project.Inventory.Item.Chassis`** (on chassis objects, same path
as `HardwareWriteProtectedControl`)

| Member | Type | Description |
|---|---|---|
| `Type` | property (enum) | Chassis type classification |

This interface is co-hosted so that the chassis object is visible to the
ObjectMapper as an inventory item.

**`xyz.openbmc_project.Software.Settings`** (on software inventory objects)

| Member | Type | Description |
|---|---|---|
| `WriteProtected` | property (bool) | Effective write-protection state, driven by the group's compute node |

### 6.3 Relationships

There are three layers: sources provide values, the reactive graph computes
effective state, and D-Bus objects expose the results.

**Layer 1 — Sources** read hardware or D-Bus signals:

```
┌─────────────────────────────────┐  ┌─────────────────────────────────┐
│ WriteProtectInput: GLOBAL_WP    │  │ Entity: Domain_0_GPU_SPI_...    │
│ (gpiod edge events)             │  │ (DbusProxy PropertiesChanged)   │
└───────────────┬─────────────────┘  └───────────────┬─────────────────┘
                │                                    │
           source S0                            source S1
```

**Layer 2 — Graph** combines sources per group:

```
         S0 ──── OR (HGX_HW_WriteProtect)
                  │
         S1 ──┬── OR (Domain_0_GPU_SPI)
     ref:    ─┘
```

The named group link connects the HW WP group's OR node directly into the
domain group's OR node.  A single `propagate()` updates both in one pass.

**Layer 3 — D-Bus objects** are updated by graph observers:

```
Group: Domain_0_GPU_SPI_WriteProtect (Type: WriteProtectDomain)
  ┌────────────────────────────────────────────────────────────────┐
  │ D-Bus path: /xyz/openbmc_project/state/                        │
  │               Domain_0_GPU_SPI_WriteProtect                    │
  │ Interfaces: com.nvidia.Software.WriteProtection                │
  │             xyz.openbmc_project.Association.Definitions         │
  │ Property:   WriteProtected ← OR node output                   │
  │ Method:     SetWriteProtected(bool)                            │
  │ Assoc:      chassis → .../HGX_Chassis_0                        │
  │             (reverse: write_protect_domains)                    │
  ├────────────────────────────────────────────────────────────────┤
  │ Redfish:    /redfish/v1/Chassis/HGX_Chassis_0/                │
  │               Oem/Nvidia/WriteProtectDomains/0                 │
  └────────────────────────────────────────────────────────────────┘
        │
        │ OR node also drives:
        ▼
  ┌────────────────────────────────────────────────────────────────┐
  │ D-Bus path: /xyz/openbmc_project/software/HGX_FW_GPU_0        │
  │ Interface:  xyz.openbmc_project.Software.Settings              │
  │ Property:   WriteProtected ← OR node output                   │
  ├────────────────────────────────────────────────────────────────┤
  │ Redfish:    /redfish/v1/UpdateService/                         │
  │               FirmwareInventory/HGX_FW_GPU_0                   │
  └────────────────────────────────────────────────────────────────┘

Group: HGX_HW_WriteProtect (Type: HardwareWriteProtectedControl)
  ┌────────────────────────────────────────────────────────────────┐
  │ D-Bus path: .../HGX_Chassis_0                                  │
  │ Interfaces: com.nvidia.State.HardwareWriteProtectedControl     │
  │             xyz.openbmc_project.Inventory.Item.Chassis          │
  │ Property:   WriteProtectedControl ← OR node output             │
  ├────────────────────────────────────────────────────────────────┤
  │ Redfish:    /redfish/v1/Chassis/HGX_Chassis_0                  │
  │               → Oem.Nvidia.HardwareWriteProtectedControl       │
  └────────────────────────────────────────────────────────────────┘
```

**Associations**:

```
Chassis ↔ Domain (managed by this service for WriteProtectDomain type):
  Domain object defines:
    chassis ──────────────────→ .../HGX_Chassis_0
  ObjectMapper creates reverse:
    write_protect_domains ────→ /xyz/openbmc_project/state/Domain_0_...

Affected entity ↔ Domain (managed by Entity Manager / nsmd):
  write_protected_by ─────────→ /xyz/openbmc_project/state/Domain_0_...
                     ←───────── write_protects
```

bmcweb populates `AffectedComponents` by querying the `write_protects`
association endpoints on each domain object. These endpoints are created by the
ObjectMapper from `Association.Definitions` published by Entity Manager and
nsmd.

---

## 7. Logging

The service uses phosphor-logging lg2. Key log messages at each level:

- **info**: Config parsing results, source creation and initialization, group
  lifecycle, source value changes, graph wiring and link resolution.
- **warning**: Source unavailable after retries, unknown interface type,
  initial value read failure.
- **error**: Config parse failure, protector set/get errors, monitor errors.

Filter logs with:

```
journalctl -u com.nvidia.fwwriteprotect -p info
```

---

*Document version: 5.1 — WriteProtectInput replaces inline source specifiers;
WriteProtectGroup no longer creates a Domain facade; Domain objects live at
`/xyz/openbmc_project/state/<Name>`; SourceMode support (OR/AND); cancellable
entity source initialization; order-independent input/group config
processing (a WriteProtectInput adopts a placeholder source node reserved
by an earlier-dispatched referencing group).*

# Class Diagram

> **Hand-maintained.** This document is derived by reading the source, not auto-generated.
> GitHub renders the Mermaid diagram below natively. Ownership semantics (owns vs.
> references, cardinality) reflect *intent* read out of the headers — update this file when
> the structure changes.

The composition root is `main()` (`apps/motion_master/main.cc`), which owns every
subsystem on the stack and wires references between them. There is no `App` class.

## Diagram

```mermaid
classDiagram
    class CyclicTask {
        <<interface>>
        +execute()*
    }
    class ProcessDataTask {
        +execute()
        -DeviceManager& deviceManager_
    }
    class GameLoop {
        +addTask(CyclicTask*)
        +run()
        +stop()
        -vector~CyclicTask*~ tasks_
        -atomic~bool~ running_
        -CyclicTimer timer_
    }
    class CyclicTimer {
        +waitForNextCycle()
    }
    class HttpServer {
        +start() / stop()
        -DeviceManager& deviceManager_
        -MonitoringManager& monitoringManager_
        -thread thread_ «port 61447»
        -atomic~uWS::Loop*~ loop_
    }
    class WebSocketServer {
        +start() / stop()
        +broadcast(json)
        +publish(topic, json)
        -thread thread_ «port 62281»
        -atomic~uWS::Loop*~ loop_
    }
    class MonitoringManager {
        +create(Monitoring)
        +setPublish(cb)
        +start() / stop()
        -DeviceManager& deviceManager_
        -ParameterRefresher refresher_
        -thread thread_
    }
    class ParameterRefresher {
        +acquire() / release()
        +start() / stop()
        -DeviceManager& deviceManager_
        -thread thread_
    }
    class DeviceManager {
        +init(unique_ptr~FieldbusDriver~)
        +scan() / reset()
        +configureProcessData()
        +transitionToState()
        +exchangeProcessData() «RT, lock-free»
        -unique_ptr~FieldbusDriver~ driver_
        -vector~Device~ devices_
        -unique_ptr~ProcessData~ pd_
        -shared_mutex busMutex_
    }
    class Device {
        +upload() / download()
        +readFile() / writeFile()
        +readRegister() / writeRegister()
        +readParameter() / writeParameter()
        -FieldbusDriver& driver_
        -uint16 slavePosition_
        -map~uint32,DeviceParameter~ parameters_
        -PdoMappings pdoMappings_
    }
    class FieldbusDriver {
        <<interface>>
        +init()* / scan()*
        +readSdo()* / writeSdo()*
        +exchangeProcessData()* «lock-free»
        +transitionToState()*
        #mutex socketMutex_
    }
    class SoemFieldbusDriver {
        -string ifname_
        -unique_ptr~ecx_context~ ctx_
    }
    class DeviceParameter {
        +DeviceParameterValue value
        +uint16 index, subindex
    }
    class PdoMappings
    class ProcessData {
        +readPdo() / writePdo() «lock-free»
        +healthy()
        -atomic~ProcessImage*~ image
        -vector~atomic_u64~ outputSlots
        -ProcessDataRing ring
        -atomic~int~ lastWkc, expectedWkc
        -atomic~bool~ exchanging
    }
    class ProcessDataRing {
        +write() «RT, wait-free»
        +head() / oldestValidSeq()
        +readRecord(seq, Record&) «lock-free»
        +allocate() / clear()
        -vector~uint8~ buffer_
        -vector~atomic_u64~ seqWords_
        -atomic~u64~ head_
    }
    class ProcessImage
    class ProfileDevice {
        +device() Device&
        -Device& device_
    }
    class Cia402Drive {
        +state() / enable() / quickStop()
        +setControlword() / setOperationMode()
        +setTargetPosition() / ...
    }
    class SomanetDrive {
        +vendor-specific OD access
    }

    CyclicTask <|.. ProcessDataTask
    FieldbusDriver <|.. SoemFieldbusDriver
    ProfileDevice <|-- Cia402Drive
    Cia402Drive <|-- SomanetDrive

    GameLoop o-- "0..*" CyclicTask : non-owning
    GameLoop *-- CyclicTimer
    ProcessDataTask ..> DeviceManager : ref
    HttpServer ..> DeviceManager : ref
    HttpServer ..> MonitoringManager : ref
    MonitoringManager ..> DeviceManager : ref
    MonitoringManager ..> WebSocketServer : publish cb (setPublish)
    MonitoringManager *-- ParameterRefresher
    ParameterRefresher ..> DeviceManager : ref
    DeviceManager *-- "1" FieldbusDriver : owns
    DeviceManager *-- "0..*" Device : owns
    DeviceManager *-- "1" ProcessData : owns (pd_)
    ProcessData *-- "1" ProcessDataRing : owns (recorder)
    ProcessData o-- "0..*" ProcessImage : generations
    Device ..> FieldbusDriver : ref (shared)
    Device ..> ProcessData : ref (live IO image)
    Device *-- "0..*" DeviceParameter
    Device *-- PdoMappings
    ProfileDevice ..> Device : borrows (non-owning)
```

## Ownership and references

`*--` = owns (by value / `unique_ptr` / `vector`). `..>` = references (raw `&`, non-owning).
`<|..` = implements an interface.

| Holder | Member | Type | Owns? | File |
|---|---|---|---|---|
| `GameLoop` | `tasks_` | `vector<CyclicTask*>` | No — caller owns | `apps/motion_master/game_loop.h` |
| `GameLoop` | `timer_` | `CyclicTimer` | Yes | `apps/motion_master/game_loop.h` |
| `ProcessDataTask` | `deviceManager_` | `DeviceManager&` | No | `apps/motion_master/process_data_task.h` |
| `HttpServer` | `deviceManager_`, `monitoringManager_` | `&` | No | `apps/motion_master/http_server.h` |
| `WebSocketServer` | — (publish target for `MonitoringManager::setPublish`) | — | No | `apps/motion_master/ws_server.h` |
| `MonitoringManager` | `refresher_` | `ParameterRefresher` | **Yes** | `libs/node/monitoring_manager.h` |
| `MonitoringManager` | `deviceManager_` | `DeviceManager&` | No | `libs/node/monitoring_manager.h` |
| `ParameterRefresher` | `deviceManager_` | `DeviceManager&` | No | `libs/node/parameter_refresher.h` |
| `DeviceManager` | `driver_` | `unique_ptr<FieldbusDriver>` | **Yes (exclusive)** | `libs/node/device_manager.h` |
| `DeviceManager` | `devices_` | `vector<Device>` | **Yes** | `libs/node/device_manager.h` |
| `DeviceManager` | `pd_` | `unique_ptr<ProcessData>` | **Yes** | `libs/node/device_manager.h` |
| `ProcessData` | `ring` | `ProcessDataRing` | **Yes** | `libs/node/process_data.h` |
| `ProcessData` | `outputSlots` | `vector<atomic<uint64_t>>` | **Yes** | `libs/node/process_data.h` |
| `ProcessData` | `generations` | `vector<shared_ptr<const ProcessImage>>` | **Yes (retained)** | `libs/node/process_data.h` |
| `Device` | `driver_` | `FieldbusDriver&` | No — same instance `DeviceManager` owns | `libs/node/device.h` |
| `Device` | `processData_` | `ProcessData*` | No — points at `DeviceManager::pd_` | `libs/node/device.h` |
| `Device` | `parameters_` | `unordered_map<uint32_t, DeviceParameter>` | **Yes** | `libs/node/device.h` |
| `Device` | `pdoMappings_` | `PdoMappings` | **Yes** | `libs/node/device.h` |

## Inheritance

The design favours composition. Two of the three hierarchies are single-interface dispatch
points; the third is a stateless view chain:

| Derived | Base | File |
|---|---|---|
| `ProcessDataTask` | `CyclicTask` | `apps/motion_master/process_data_task.h` |
| `SoemFieldbusDriver` | `FieldbusDriver` | `libs/comm/soem_fieldbus_driver.h` |
| `Cia402Drive` | `ProfileDevice` | `libs/node/cia402_drive.h` |
| `SomanetDrive` | `Cia402Drive` | `libs/node/somanet_drive.h` |

`SpoeDriver` (SPoE) is a further `FieldbusDriver` implementation noted in the design docs.

**The drive-profile chain is *not* `Device` inheritance.** `ProfileDevice` and its subclasses
do **not** derive from `Device` and are not owned by `DeviceManager`; each *borrows* a `Device&`
and carries no state beyond that reference (the drive's state lives in its statusword on the
wire). A profile view is a thin, here-and-now view over a device's object dictionary —
constructed for a single operation (a stack local in an HTTP handler, or a member scoped to a
`CyclicTask`) and dropped. Because they are never stored base-typed (no `vector<ProfileDevice>`,
no polymorphic container), `SomanetDrive → Cia402Drive → ProfileDevice` is a slicing-free is-a
chain. Validate-then-bind via `createCia402Drive` / `createSomanetDrive`. See `NEXTGEN.md` for
the rationale and the Somanet free-function design.

## Key value types

```cpp
using DeviceParameterValue = std::variant<
  int8_t, int16_t, int32_t, int64_t,
  uint8_t, uint16_t, uint32_t, uint64_t,
  float, double, std::string, std::vector<uint8_t>>;
```

`DeviceParameter` holds an `index`, `subindex`, and a `DeviceParameterValue`
(`libs/node/device_parameter.h`). Dispatch on the value with `std::visit`.

See also the [threading model](THREADS.md) for how these objects are accessed across threads.

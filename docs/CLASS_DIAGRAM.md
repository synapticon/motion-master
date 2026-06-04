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
    class Server {
        +start()
        +stop()
        +broadcast(json)
        +publish(topic, json)
        -DeviceManager& deviceManager_
        -MonitoringManager& monitoringManager_
        -thread thread_
    }
    class MonitoringManager {
        +create(Monitoring)
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
    class ProcessImage

    CyclicTask <|.. ProcessDataTask
    FieldbusDriver <|.. SoemFieldbusDriver

    GameLoop o-- "0..*" CyclicTask : non-owning
    GameLoop *-- CyclicTimer
    ProcessDataTask ..> DeviceManager : ref
    Server ..> DeviceManager : ref
    Server ..> MonitoringManager : ref
    MonitoringManager ..> DeviceManager : ref
    MonitoringManager *-- ParameterRefresher
    ParameterRefresher ..> DeviceManager : ref
    DeviceManager *-- "1" FieldbusDriver : owns
    DeviceManager *-- "0..*" Device : owns
    DeviceManager *-- ProcessImage
    Device ..> FieldbusDriver : ref (shared)
    Device *-- "0..*" DeviceParameter
    Device *-- PdoMappings
```

## Ownership and references

`*--` = owns (by value / `unique_ptr` / `vector`). `..>` = references (raw `&`, non-owning).
`<|..` = implements an interface.

| Holder | Member | Type | Owns? | File |
|---|---|---|---|---|
| `GameLoop` | `tasks_` | `vector<CyclicTask*>` | No — caller owns | `apps/motion_master/game_loop.h` |
| `GameLoop` | `timer_` | `CyclicTimer` | Yes | `apps/motion_master/game_loop.h` |
| `ProcessDataTask` | `deviceManager_` | `DeviceManager&` | No | `apps/motion_master/process_data_task.h` |
| `Server` | `deviceManager_`, `monitoringManager_` | `&` | No | `apps/motion_master/server.h` |
| `MonitoringManager` | `refresher_` | `ParameterRefresher` | **Yes** | `libs/node/monitoring_manager.h` |
| `MonitoringManager` | `deviceManager_` | `DeviceManager&` | No | `libs/node/monitoring_manager.h` |
| `ParameterRefresher` | `deviceManager_` | `DeviceManager&` | No | `libs/node/parameter_refresher.h` |
| `DeviceManager` | `driver_` | `unique_ptr<FieldbusDriver>` | **Yes (exclusive)** | `libs/node/device_manager.h` |
| `DeviceManager` | `devices_` | `vector<Device>` | **Yes** | `libs/node/device_manager.h` |
| `DeviceManager` | `pd_` | `unique_ptr<ProcessData>` | **Yes** | `libs/node/device_manager.h` |
| `Device` | `driver_` | `FieldbusDriver&` | No — same instance `DeviceManager` owns | `libs/node/device.h` |
| `Device` | `parameters_` | `unordered_map<uint32_t, DeviceParameter>` | **Yes** | `libs/node/device.h` |
| `Device` | `pdoMappings_` | `PdoMappings` | **Yes** | `libs/node/device.h` |

## Inheritance

The design favours composition; there are only two polymorphic hierarchies:

| Derived | Base | File |
|---|---|---|
| `ProcessDataTask` | `CyclicTask` | `apps/motion_master/process_data_task.h` |
| `SoemFieldbusDriver` | `FieldbusDriver` | `libs/comm/soem_fieldbus_driver.h` |

`SpoeDriver` (SPoE) is a further `FieldbusDriver` implementation noted in the design docs.
The CiA402 layer is modelled as `Device → Cia402Drive` (shallow is-a inheritance); see
`NEXTGEN.md` for that and the Somanet free-function design.

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

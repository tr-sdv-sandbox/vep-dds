# Vehicle Data Readout (VDR) Ecosystem Specification

## Overview

This document specifies a proof-of-concept implementation of an onboard data collection system using DDS (Cyclone DDS) as the internal transport. The system consists of:

1. **Probes** — Components that collect data from various sources and publish to DDS
2. **VDR (Vehicle Data Readout)** — Central component that subscribes to DDS topics and prepares data for offboarding

## Design Principles

1. **DDS as the single onboard transport** — All data flows through DDS, keeping VDR simple
2. **C++17 with RAII wrappers** — All C resources (DDS, allocations) wrapped in C++ for safety
3. **Probes are independent** — Each probe is a separate process, unaware of VDR
4. **Configuration-driven** — Topic subscriptions and behaviors configurable at runtime
5. **Minimal dependencies** — Only what's necessary for the PoC

## Architecture

```
┌─────────────────────────────────────────────────────────────────────────┐
│                              DDS Domain 0                               │
│                                                                         │
│   Topics:                                                               │
│   ├── rt/vss/signals           (VSS signal samples)                    │
│   ├── rt/events/vehicle        (Vehicle events)                        │
│   ├── rt/diagnostics/measurements (Diagnostic data)                    │
│   ├── rt/telemetry/metrics     (Prometheus-style metrics)              │
│   └── rt/logs/entries          (Log entries)                           │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
        ▲               ▲               ▲               │
        │               │               │               │
        │               │               │               ▼
┌───────────────┐ ┌───────────────┐ ┌───────────────┐ ┌───────────────┐
│ VSS Probe     │ │ Metrics Probe │ │ Event Probe   │ │     VDR       │
│ (sample)      │ │ (sample)      │ │ (sample)      │ │               │
└───────────────┘ └───────────────┘ └───────────────┘ └───────────────┘
                                                              │
                                                              ▼
                                                      [glog output]
                                                      (simulates MQTT)
```

## Directory Structure

```
/home/saka/BALI/DDS/
├── SPECIFICATION.md          # This document
├── install_deps.sh           # Dependency installation script
├── CMakeLists.txt            # Root CMake configuration
├── idl/                      # DDS IDL definitions
│   └── telemetry.idl         # Message type definitions
├── src/
│   ├── common/               # Shared utilities and wrappers
│   │   ├── dds_wrapper.hpp   # RAII wrappers for Cyclone DDS
│   │   ├── dds_wrapper.cpp
│   │   ├── types.hpp         # Common type definitions
│   │   └── config.hpp        # Configuration utilities
│   ├── vdr/                  # Vehicle Data Readout
│   │   ├── vdr.cpp           # Main VDR application
│   │   ├── subscriber.hpp    # DDS subscription management
│   │   ├── subscriber.cpp
│   │   ├── encoder.hpp       # Encodes messages for offboard (simulated)
│   │   └── encoder.cpp
│   └── probes/               # Sample probes
│       ├── vss_probe/        # VSS signal probe
│       │   └── main.cpp
│       ├── metrics_probe/    # Prometheus-style metrics probe
│       │   └── main.cpp
│       └── event_probe/      # Vehicle event probe
│           └── main.cpp
├── config/                   # Configuration files
│   └── vdr_config.yaml       # VDR configuration
└── tests/                    # Unit tests
    └── ...
```

## IDL Definitions

### Common Header

All messages share a common header for tracing and ordering:

```idl
module telemetry {

    struct Header {
        string source_id;           // ECU/component identifier (e.g., "ecu_front_left")
        long long timestamp_ns;     // Nanoseconds since epoch (vehicle clock)
        unsigned long sequence;     // Per-source monotonic sequence number
        string correlation_id;      // Optional UUID for event correlation
    };

};
```

### VSS Signals

```idl
module telemetry {
module vss {

    enum Quality {
        NOT_AVAILABLE,
        VALID,
        INVALID
    };

    union Value switch (short) {
        case 0: boolean bool_val;
        case 1: long int32_val;
        case 2: long long int64_val;
        case 3: float float_val;
        case 4: double double_val;
        case 5: string string_val;
    };

    struct Signal {
        Header header;
        string path;                // VSS path (e.g., "Vehicle.Speed")
        Value value;
        Quality quality;
    };

};
};
```

### Vehicle Events

```idl
module telemetry {
module events {

    enum Severity {
        INFO,
        WARNING,
        ERROR,
        CRITICAL
    };

    struct Event {
        Header header;
        string category;            // e.g., "ADAS", "POWERTRAIN", "CAB"
        string event_type;          // e.g., "harsh_brake", "dtc_set"
        Severity severity;
        sequence<octet> payload;    // Opaque event-specific data (JSON, protobuf, etc.)
    };

};
};
```

### Diagnostic Measurements

```idl
module telemetry {
module diagnostics {

    enum MeasurementType {
        ACCUMULATED,
        MOMENTARY
    };

    struct ScalarMeasurement {
        Header header;
        string variable_id;         // Measurement identifier
        string unit;                // Unit of measurement
        MeasurementType mtype;
        double value;
    };

    struct VectorMeasurement {
        Header header;
        string variable_id;
        string unit;
        MeasurementType mtype;
        sequence<double> values;
    };

};
};
```

### Telemetry Metrics (Prometheus-style)

```idl
module telemetry {
module metrics {

    struct Label {
        string key;
        string value;
    };

    struct Counter {
        Header header;
        string name;
        sequence<Label> labels;
        double value;               // Cumulative value
    };

    struct Gauge {
        Header header;
        string name;
        sequence<Label> labels;
        double value;               // Current value
    };

    struct HistogramBucket {
        double upper_bound;
        unsigned long long count;
    };

    struct Histogram {
        Header header;
        string name;
        sequence<Label> labels;
        unsigned long long sample_count;
        double sample_sum;
        sequence<HistogramBucket> buckets;
    };

};
};
```

### Log Entries

```idl
module telemetry {
module logs {

    enum Level {
        DEBUG,
        INFO,
        WARN,
        ERROR
    };

    struct LogEntry {
        Header header;
        Level level;
        string component;           // Software component name
        string message;
        sequence<Label> fields;     // Structured fields (reuse Label from metrics)
    };

};
};
```

## Topic Configuration

| Topic Name | IDL Type | QoS Profile | Description |
|------------|----------|-------------|-------------|
| `rt/vss/signals` | `telemetry::vss::Signal` | Reliable, Keep Last 100 | VSS signal updates |
| `rt/events/vehicle` | `telemetry::events::Event` | Reliable, Keep All, Transient Local | Vehicle events (must not lose) |
| `rt/diagnostics/scalar` | `telemetry::diagnostics::ScalarMeasurement` | Reliable, Keep Last 10 | Scalar diagnostics |
| `rt/diagnostics/vector` | `telemetry::diagnostics::VectorMeasurement` | Reliable, Keep Last 10 | Vector diagnostics |
| `rt/telemetry/counters` | `telemetry::metrics::Counter` | Best Effort, Keep Last 1 | Prometheus counters |
| `rt/telemetry/gauges` | `telemetry::metrics::Gauge` | Best Effort, Keep Last 1 | Prometheus gauges |
| `rt/telemetry/histograms` | `telemetry::metrics::Histogram` | Best Effort, Keep Last 1 | Prometheus histograms |
| `rt/logs/entries` | `telemetry::logs::LogEntry` | Best Effort, Keep Last 100 | Log entries |

## QoS Profiles

### Reliable Critical (Events)

```xml
<qos>
    <reliability kind="RELIABLE"/>
    <durability kind="TRANSIENT_LOCAL"/>
    <history kind="KEEP_ALL"/>
</qos>
```

### Reliable Standard (VSS, Diagnostics)

```xml
<qos>
    <reliability kind="RELIABLE"/>
    <durability kind="VOLATILE"/>
    <history kind="KEEP_LAST" depth="100"/>
</qos>
```

### Best Effort (Telemetry, Logs)

```xml
<qos>
    <reliability kind="BEST_EFFORT"/>
    <durability kind="VOLATILE"/>
    <history kind="KEEP_LAST" depth="1"/>
</qos>
```

## C++ Wrapper Design

### RAII Principles

All Cyclone DDS C handles wrapped using RAII:

```cpp
namespace dds {

// Unique ownership wrapper for DDS entities
template<typename Deleter>
class Entity {
public:
    explicit Entity(dds_entity_t handle);
    ~Entity();

    Entity(Entity&& other) noexcept;
    Entity& operator=(Entity&& other) noexcept;

    // No copying
    Entity(const Entity&) = delete;
    Entity& operator=(const Entity&) = delete;

    dds_entity_t get() const noexcept;
    explicit operator bool() const noexcept;

private:
    dds_entity_t handle_;
};

// Convenience types
using Participant = Entity<ParticipantDeleter>;
using Topic = Entity<TopicDeleter>;
using Publisher = Entity<PublisherDeleter>;
using Subscriber = Entity<SubscriberDeleter>;
using Writer = Entity<WriterDeleter>;
using Reader = Entity<ReaderDeleter>;

}  // namespace dds
```

### Factory Functions

```cpp
namespace dds {

// Create participant with default QoS
Participant create_participant(dds_domainid_t domain = 0);

// Create topic with type support
template<typename T>
Topic create_topic(const Participant& participant,
                   const std::string& name,
                   const dds_qos_t* qos = nullptr);

// Create writer
template<typename T>
Writer create_writer(const Participant& participant,
                     const Topic& topic,
                     const dds_qos_t* qos = nullptr);

// Create reader
template<typename T>
Reader create_reader(const Participant& participant,
                     const Topic& topic,
                     const dds_qos_t* qos = nullptr);

}  // namespace dds
```

## VDR Configuration

VDR reads configuration from YAML:

```yaml
# vdr_config.yaml
domain_id: 0

subscriptions:
  - topic: "rt/vss/signals"
    enabled: true
    buffer_size: 1000

  - topic: "rt/events/vehicle"
    enabled: true
    buffer_size: 100
    priority: critical

  - topic: "rt/telemetry/gauges"
    enabled: true
    buffer_size: 50

  - topic: "rt/logs/entries"
    enabled: false  # disabled in this config

offboard:
  # Simulated - just logs what would be sent
  log_format: json  # or "compact"
  batch_size: 10
  flush_interval_ms: 1000
```

## Sample Probes

### VSS Probe

Simulates VSS signal sampling:

- Publishes to `rt/vss/signals`
- Generates fake Vehicle.Speed, Vehicle.Battery.SOC, etc.
- Configurable publish rate

### Metrics Probe

Simulates Prometheus-style metrics:

- Publishes to `rt/telemetry/gauges`, `rt/telemetry/counters`
- Generates CPU usage, memory, request counts
- Configurable scrape interval

### Event Probe

Simulates vehicle events:

- Publishes to `rt/events/vehicle`
- Generates random events (harsh_brake, dtc_set, etc.)
- Random intervals

## Build System

CMake-based build with:

- C++17 standard
- Cyclone DDS via apt packages
- IDL compilation via `idlc` (Cyclone IDL compiler)
- Google logging (glog)
- yaml-cpp for configuration
- GoogleTest for unit tests

## Dependencies

| Package | Purpose | Ubuntu Package |
|---------|---------|----------------|
| Cyclone DDS | DDS implementation | `cyclonedds-dev`, `cyclonedds-tools` |
| glog | Logging | `libgoogle-glog-dev` |
| yaml-cpp | Configuration parsing | `libyaml-cpp-dev` |
| GoogleTest | Unit testing | `libgtest-dev` |
| CMake | Build system | `cmake` |
| pkg-config | Dependency detection | `pkg-config` |

## Success Criteria for PoC

1. **Probes publish independently** — Each probe runs as separate process
2. **VDR receives all data** — Subscribes to configured topics
3. **VDR logs simulated output** — glog shows what would be sent over MQTT
4. **QoS works** — Events are reliable, telemetry is best-effort
5. **Clean shutdown** — RAII ensures no resource leaks
6. **Configuration works** — VDR respects enable/disable per topic

## Future Work (Out of Scope for PoC)

- MQTT offboarding (replace glog simulation)
- Compact binary encoding for bandwidth optimization
- Android bridge (WebSocket/gRPC)
- OTel Collector integration
- vsomeip bridge
- Security (DDS Security plugins)
- Multi-domain support

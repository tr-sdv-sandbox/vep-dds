# VDR-Light

Proof-of-concept Vehicle Data Readout system using Cyclone DDS for onboard telemetry collection and routing.

## Purpose

This is a PoC to verify architectural concepts. MQTT encoding and transmission are intentionally out of scope—the VDR logs what would be sent rather than actually publishing.

## Architecture

```
Probes (VSS, Events, Metrics, AVTP) --> DDS Topics --> VDR --> [simulated MQTT]
```

## Building

```bash
./install_deps.sh              # Ubuntu 24.04
cmake -B build
cmake --build build -j$(nproc)
```

## Components

| Component | Description |
|-----------|-------------|
| `vdr` | Central subscriber, receives DDS data, logs simulated MQTT output |
| `probe_vss` | VSS signal publisher |
| `probe_events` | Vehicle event publisher |
| `probe_metrics` | Prometheus-style metrics publisher |

## Usage

```bash
./build/vdr                    # Central subscriber
./build/probe_vss              # VSS signals
./build/probe_events           # Vehicle events
./build/test_dds_wrapper       # Tests
```

## Configuration

`config/vdr_config.yaml` controls topic subscriptions.

## License

Apache 2.0

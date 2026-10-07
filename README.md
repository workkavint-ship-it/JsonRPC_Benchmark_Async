# ES1 JSON-RPC Async Benchmark

This repository is the asynchronous counterpart to `ES1Test-JSONRPC-Benchmark`.
It keeps the same `set*`, `get*`, and `measure*` operations and payload limits,
but exposes them through the Thunder `@async` interface model.

Each request returns an immediate acknowledgement. The completion is delivered
as an event on the same JSON-RPC channel and is correlated by the client with
the async ID assigned by Thunder.

## Dependencies

- Thunder / WPEFramework plugin headers and libraries
- The matching `entservices-apis` build containing `IES1BenchmarkAsync.h`
- C++14 and POSIX sockets for the standalone client

The interface is added under `entservices-apis/apis/ES1Benchmark` and is
generated with the normal `ProxyStubGenerator` flow. Build the API repository
before configuring this repository.

## Build

```sh
cmake -S . -B build -DPLUGIN_ES1BENCHMARKASYNC=ON -DPLUGIN_ES1BENCHMARKASYNC_CLIENT=ON
cmake --build build
```

The plugin is installed as `ES1BenchmarkAsync`; the client reads
`/opt/es1-async.config` by default and writes JSON Lines to stdout.
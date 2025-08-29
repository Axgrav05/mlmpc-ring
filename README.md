# Lock-Free MPMC Ring Buffer (Ticketed Slots)

Bounded, cache-friendly, **lock-free MPMC** queue for high-throughput ML pipelines.

## Results (Intel/Windows, MSVC19)
- 2P/2C, cap=65,536, batch=32 → **~XX.X Mops/s**
- 4P/4C, cap=131,072, batch=32 → **~201.8 Mops/s**

## Why it’s fast
- Per-slot **sequence tickets** (no ABA).
- **ACQUIRE/RELEASE** publishes/consumes payloads correctly.
- **CAS** on head/tail; indices padded to avoid false sharing.
- **Batch enqueue** (block reserve) + **non-blocking batched dequeue** (claim only ready run).
- Tiny backoff with `_mm_pause()` and **thread affinity** on Windows.

## Build
```powershell
cmake -S . -B build-vs2019 -G "Visual Studio 16 2019" -A x64 -DENABLE_CUDA=OFF
cmake --build build-vs2019 --config Release -j

.\build-vs2019\Release\bench_throughput.exe 2000000 2 2 65536 32
Benchmark config:
  items_per_producer = 2000000
  producers          = 2
  consumers          = 2
  queue_capacity     = 65536
  batch              = 32
Results:
  elapsed (s): 0.01
  total ops :  4000000.00
  throughput:  334.23 Mops/s

.\build-vs2019\Release\bench_throughput.exe 2000000 4 4 131072 16
Benchmark config:
  items_per_producer = 2000000
  producers          = 4
  consumers          = 4
  queue_capacity     = 131072
  batch              = 16
Results:
  elapsed (s): 0.05
  total ops :  8000000.00
  throughput:  147.31 Mops/s

.\build-vs2019\Release\bench_throughput.exe 2000000 4 4 131072 32
Benchmark config:
  items_per_producer = 2000000
  producers          = 4
  consumers          = 4
  queue_capacity     = 131072
  batch              = 32
Results:
  elapsed (s): 0.04
  total ops :  8000000.00
  throughput:  178.86 Mops/s

.\build-vs2019\Release\bench_throughput.exe 2000000 8 8 131072 32
Benchmark config:
  items_per_producer = 2000000
  producers          = 8
  consumers          = 8
  queue_capacity     = 131072
  batch              = 32
Results:
  elapsed (s): 0.15
  total ops :  16000000.00
  throughput:  105.16 Mops/s

## Run
.\build-vs2019\Release\test_correctness.exe
.\build-vs2019\Release\bench_throughput.exe [items_per_producer] [producers] [consumers] [capacity] [batch]



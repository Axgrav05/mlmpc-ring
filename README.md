# Lock-Free MPMC Ring Buffer

A high-performance lock-free multi-producer, multi-consumer (MPMC) queue implemented in C++17, designed for low-latency concurrent workloads.
Built with per-slot sequence numbers, batched enqueue/dequeue, cache-line padding, and thread affinity pinning to eliminate lock contention and minimize false sharing.

## Features

- Lock-free design using per-slot sequence numbers to ensure correctness without locks

- Batched enqueue/dequeue to amortize atomic overhead and maximize throughput

- Cache-line padding to prevent false sharing across cores

- Thread affinity pinning to reduce scheduling overhead and ensure repeatability

- Correctness validation with exactly-once guarantees under concurrent load

- Throughput & latency benchmarking with tunable parameters (producers, consumers, capacity, batch size)

## Results & Analysis

We evaluated the lock-free RingMPMC queue under varying workloads on Windows 11 (Visual Studio 2019, Release build). Benchmarks were run with different producer/consumer configurations, queue capacities, and batch sizes.

### 1. Baseline (Balanced 4P / 4C, Capacity 131072, Batch 32)

Throughput: ~241 Mops/s

Latency (p50/p95/p99): 100 / 200 / 300 ns

#### Observation: This configuration provides a stable baseline with predictable latency and high throughput.

### 2. Effect of Queue Capacity
Capacity	Producers/Consumers	Batch	Throughput (Mops/s)	Notes
65536	4 / 4	32	~241	Matches baseline
131072	4 / 4	32	~241	Sweet spot
262144	4 / 4	32	~106	Cache pressure hurts performance

#### Conclusion: Oversized queues degrade cache locality. A mid-size (64k–128k) capacity is optimal.

### 3. Effect of Batch Size
Batch Size	Producers/Consumers	Capacity	Throughput (Mops/s)	Notes
16	4 / 4	131072	~167	Smaller batches = higher sync overhead
32	4 / 4	131072	~241	Balanced throughput & latency
64	4 / 4	131072	~280	Best throughput observed

#### Conclusion: Larger batches reduce per-item synchronization, increasing throughput. Batch=64 yielded the highest throughput (~280 Mops/s).

### 4. Producer/Consumer Asymmetry
Producers	Consumers	Capacity	Batch	Throughput (Mops/s)	Notes
6	2	131072	32	~173	Consumer bottleneck
2	6	131072	32	~235	Overprovisioned consumers, close to baseline

#### Conclusion: Performance is sensitive to too few consumers (drain bottleneck) but remains stable with extra consumers.

## Summary
- Throughput peaks at ~280 Mops/s with balanced workloads (4P/4C, batch 64).

- Latency stays predictable across configurations (p50=100ns, p95=200ns, p99=300ns).

- Optimal settings for most use cases: capacity 131072, batch 32–64, balanced producer/consumer counts.

- Avoid oversizing the queue — larger than 131072 hurts cache locality and reduces throughput.

## Key Results

- Peak throughput: 334 Mops/s (2 producers, 2 consumers, batch=32)

- Sustained throughput: 280 Mops/s (4 producers, 4 consumers, batch=64)

- Latency distribution: p50 = 100ns, p95 = 200ns, p99 ≤ 400ns

- Scales to 8P/8C with ~200 Mops/s sustained throughput

## Future Work

- NUMA-aware scheduling for multi-socket scalability

- Cross-platform benchmarking (Linux, macOS)

- Integration into real-world messaging or pipeline workloads



# caching_dex

Single-threaded, local-memory port of the LeanStore caching service from dex.


## Build


```
git clone https://github.com/thanhha1210/caching_dex.git
cd caching_dex
mkdir build && cd build
cmake ..
make
```

This produces `newbench` — workload-driven throughput benchmark.


## Running the benchmark

```
newbench [key_space] [op_num] [read%] [insert%] [update%] [delete%] [range%]
         [zipfian] [cache_mb] [scan_len]
```

All positional, all optional. Ratios must sum to 100. Defaults:

```
key_space=1M  op_num=1M  reads=50  inserts=0  updates=50  deletes=0
range=0       zipfian=0.99  cache_mb=16  scan_len=100
```

Each run has two phases:

1. **Bulk-load** — insert `key_space` keys sequentially (1..N), each with
   `value = key + 1`. Prints `bulk-load: inserted N keys in T s (R Mops/s)`.
   
2. **Mixed workload** — `op_num` operations sampled from the ratio mix.
   Keys drawn from a Zipfian distribution over `[1, key_space]`. Prints
   throughput, per-op-type counts, and cache-miss totals.


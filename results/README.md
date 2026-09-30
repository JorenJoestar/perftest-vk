# Results

| GPU | Type | Driver | Vulkan | Files |
|---|---|---|---|---|
| NVIDIA GeForce RTX 3070 Laptop (Ampere) | discrete | 610.74 | 1.4.341 | `rtx3070-laptop_*.csv` |
| AMD Radeon Graphics, Ryzen 5000 "Cezanne" (Vega) | integrated | 2.0.279 | 1.3.260 | `cezanne_*.csv` |

Both GPUs are in the same Windows laptop. Shaders: DXC 1.9.0.5512, Slang 2026.17-25 and glslang 16.6.0 from Vulkan SDK 1.4.363.0 (glslang cannot compile the `bindless` suite).

Times are one dispatch in ms. RTX 3070 numbers come from the DXC run, Vega numbers from the Slang run (the Vega DXC run was noisy; the SPIR-V is the same except where [Compilers](#compilers) says otherwise). Differences below ~10% are noise. The two GPUs are in different classes: compare paths within one GPU, not times across GPUs.

## Compilers

- Slang `LoadAligned<T>` emits one 128-bit load; DXC `Load<T>` emits four 32-bit loads (visible with `spirv-dis`).
- Slang up to 2026.17 does not decorate `buf[NonUniformResourceIndex(i)][j].field` as non-uniform. On the Vega iGPU `--verify` fails for that test with Slang (4096/4096 threads wrong) and passes with DXC; on the RTX 3070 it passes with both. Probably [slang#10525](https://github.com/shader-slang/slang/issues/10525). The Slang time for that test is not valid.
- On the Vega iGPU, random 16-byte stores into 256 KB measured 9.3 ms (Slang run) and 16.3 ms (DXC run) with the same SPIR-V: treat that test as unstable.

To compare two files: `python scripts/compare.py results/rtx3070-laptop_dxc.csv results/rtx3070-laptop_slang.csv --metric median`.

## `original` suite

Speed relative to `Buffer<RGBA8>.Load random`; higher is faster.

| Test | 3070 uniform | 3070 linear | 3070 random | Vega uniform | Vega linear | Vega random |
|---|---|---|---|---|---|---|
| `Buffer<R32f>.Load` | 1.51x | 1.51x | 1.01x | 3.63x | 3.66x | 1.00x |
| `Buffer<RGBA32f>.Load` | 0.76x | 0.77x | 0.77x | 1.00x | 1.00x | 0.89x |
| `ByteAddressBuffer.Load` | 2.16x | 2.13x | 2.14x | 3.61x | 3.66x | 1.00x |
| `ByteAddressBuffer.Load4` | 0.73x | 0.37x | 0.76x | 1.52x | 1.00x | 0.89x |
| `StructuredBuffer<float4>.Load` | 1.38x | 1.02x | 1.13x | 3.62x | 1.00x | 0.89x |
| `cbuffer{float4} load` | 1.12x | 0.03x | 0.05x | 3.62x | 1.00x | 0.89x |
| `Texture2D<RGBA8>.Load` | 1.37x | 1.04x | 0.78x | 1.00x | 1.00x | 1.00x |
| `Texture2D<RGBA32F>.Sample(bilinear)` | 0.20x | 0.20x | 0.13x | 0.25x | 0.17x | 0.12x |

## Load paths

Random `uint4` loads; 256 per thread over 16 KB, 32 over 64 MB.

| Path | 3070 16 KB | 3070 64 MB | Vega 16 KB | Vega 64 MB |
|---|---|---|---|---|
| `StructuredBuffer<uint4>` | 0.285 | 1.578 | 2.397 | 12.57 |
| `ByteAddressBuffer.Load4` | 1.033 | 4.781 | 2.397 | 12.84 |
| DXC `Load<uint4>` | 1.033 | 4.682 | 2.521 | 12.75 |
| Slang `LoadAligned<uint4>` | 0.287 | 1.579 | 2.397 | 12.75 |
| BDA, alignment 16 | 0.284 | 1.574 | 2.386 | 12.72 |

BDA `float4`, 256 loads per thread from 16 KB:

| Declared alignment | 3070 uniform | 3070 linear | 3070 random | Vega uniform | Vega linear | Vega random |
|---|---|---|---|---|---|---|
| 4 | 0.920 | 1.835 | 0.921 | 3.997 | 3.997 | 4.496 |
| 16 | 0.480 | 0.494 | 0.486 | 3.997 | 3.997 | 4.496 |

## Access patterns

`StructuredBuffer<uint4>` (typed) and `ByteAddressBuffer.Load4`; 256 loads per thread (32 for 64 MB). Wave-uniform: one random address per wave. Wave-coherent: each wave reads a contiguous block at a random position.

| Working set | Pattern | 3070 typed | 3070 `Load4` | Vega typed | Vega `Load4` |
|---|---|---|---|---|---|
| 16 KB | linear | 0.176 | 0.434 | 1.016 | 1.014 |
| 16 KB | random | 0.285 | 1.033 | 2.397 | 2.397 |
| 16 KB | wave-uniform | 0.187 | 0.259 | 1.019 | 1.011 |
| 16 KB | wave-coherent | 0.222 | 0.434 | 1.016 | 1.010 |
| 4 MB | linear | 0.165 | 0.434 | 1.896 | 2.052 |
| 4 MB | random | 1.740 | 2.459 | 70.68 | 70.67 |
| 4 MB | wave-uniform | 0.228 | 0.271 | 1.485 | 1.494 |
| 4 MB | wave-coherent | 0.588 | 0.655 | 16.76 | 16.74 |
| 64 MB | linear | 0.309 | 0.311 | 2.777 | 2.767 |
| 64 MB | random | 1.578 | 4.781 | 12.57 | 12.84 |
| 64 MB | wave-uniform | 0.054 | 0.057 | 0.227 | 0.227 |
| 64 MB | wave-coherent | 0.293 | 0.294 | 2.751 | 2.749 |

## Non-uniform textures

Time relative to one texture with a uniform index. Scalarized = manual `WaveReadLaneFirst` loop.

| Textures per wave | 3070 driver | 3070 scalarized | Vega driver | Vega scalarized |
|---|---|---|---|---|
| 1 | 1.1x | 1.1x | 1.0x | 1.0x |
| 2 | 1.1x | 1.1x | 1.7x | 1.7x |
| 4 | 1.1x | 2.2x | 2.5x | 2.5x |
| 8 | 1.6x | 4.4x | 3.9x | 3.9x |
| 16 | 3.9x | 8.9x | 9.4x | 9.5x |
| 32 | 24.4x | 17.6x | 17.7x | 17.7x |

## Descriptor divergence

Random `Load4`. The first two rows read the same 16 KB through 8 descriptors, so only the descriptor index diverges. The last three read 8 regions of one 128 KB buffer.

| Test | 3070 | Vega |
|---|---|---|
| 8 descriptors, uniform index | 0.923 | 4.496 |
| 8 descriptors, `NonUniformResourceIndex` | 0.927 | 21.00 |
| 8 regions: 8 descriptors, `NonUniformResourceIndex` | 0.516 | 21.16 |
| 8 regions: one descriptor, divergent offset | 0.504 | 11.12 |
| 8 regions: BDA, divergent base | 0.500 | 11.15 |

## Stores

32 stores per thread into 64 MB. Sparse writes one element per 64-byte line.

| Path | 3070 linear | 3070 sparse | 3070 random | Vega linear | Vega sparse | Vega random |
|---|---|---|---|---|---|---|
| `RWStructuredBuffer<uint>` | 0.078 | 0.871 | 1.713 | 0.739 | 7.251 | 14.09 |
| `RWStructuredBuffer<uint2>` | 0.160 | 0.867 | 1.711 | 1.488 | 7.222 | 14.10 |
| `RWStructuredBuffer<uint4>` | 0.318 | 0.863 | 1.674 | 2.331 | 7.244 | 14.09 |
| `RWByteAddressBuffer.Store` | 0.081 | 0.879 | 1.676 | 0.736 | 7.244 | 14.10 |
| `RWByteAddressBuffer.Store2` | 0.162 | 1.050 | 3.283 | 1.487 | 7.234 | 14.10 |
| `RWByteAddressBuffer.Store4` | 0.574 | 1.395 | 6.188 | 2.333 | 7.211 | 14.09 |
| BDA 4 B | 0.080 | 0.719 | 1.686 | 0.737 | 7.251 | 14.11 |
| BDA 8 B | 0.160 | 0.749 | 1.677 | 1.489 | 7.230 | 14.11 |
| BDA 16 B | 0.285 | 0.751 | 1.674 | 2.339 | 7.210 | 14.11 |

16-byte stores into smaller working sets, 256 per thread:

| Working set | 3070 typed | 3070 `Store4` | Vega typed | Vega `Store4` |
|---|---|---|---|---|
| 16 KB linear | 0.991 | 4.951 | 1.998 | 2.004 |
| 16 KB random | 4.126 | 16.57 | 8.249 | 8.247 |
| 256 KB random | 3.484 | 13.84 | 9.295 | 9.292 |
| 4 MB random | 4.038 | 14.00 | 93.05 | 90.51 |

## Atomics

32 operations per thread. DXC and Slang give the same times.

Append (compaction) to one counter, then a write to a list:

| Active lanes | 3070 per lane | 3070 per wave | Vega per lane | Vega per wave |
|---|---|---|---|---|
| 100% | 0.177 | 0.178 | 0.716 | 0.717 |
| 50% | 0.165 | 0.168 | 0.355 | 0.361 |
| 10% | 0.151 | 0.153 | 0.193 | 0.288 |

256-bin histogram (skewed: all values in 16 bins):

| Data | 3070 global | 3070 groupshared | Vega global | Vega groupshared |
|---|---|---|---|---|
| uniform | 2.228 | 0.018 | 0.402 | 0.129 |
| skewed | 1.760 | 0.017 | 0.947 | 0.184 |

Random counters (the counter index comes from push constants):

| Counters | 3070 Add | 3070 Add, result used | 3070 Or | Vega Add | Vega Add, result used | Vega Or |
|---|---|---|---|---|---|---|
| 1 | 0.146 | 0.147 | 0.146 | 4.072 | 4.533 | 4.071 |
| 64 | 2.476 | 2.515 | 2.536 | 1.000 | 1.002 | 1.000 |
| 4096 | 0.385 | 0.386 | 0.385 | 1.044 | 1.838 | 1.044 |
| 1M | 0.568 | 0.552 | 0.537 | 23.79 | 23.78 | 23.77 |

`InterlockedMax` (64-bit value: `depth << 32 | id`):

| Targets | 3070 32-bit | 3070 64-bit | Vega 32-bit | Vega 64-bit |
|---|---|---|---|---|
| 4096 | 0.386 | 0.263 | 1.045 | 1.114 |
| 1M | 0.541 | 1.742 | 23.78 | 28.01 |

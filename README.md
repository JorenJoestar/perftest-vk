# perftest-vk: unofficial Vulkan port of sebbbi/perftest

A shader memory performance test tool for Vulkan, based on **[PerfTest](https://github.com/sebbbi/perftest)** by Sebastian Aaltonen. It is a synthetic suites of shaders (compute for now) to measure speed of various operations, helping (hopefully) to take informed decisions on data structures used on the GPU.

- **`original` suite (138 tests):** the original HLSL shaders, compiled unchanged to SPIR-V, with the same names, dispatch size and output format, so the numbers can be compared with the tables in the original README.
- **`bindless` suite (161 tests):** loads, stores and buffer atomics through a typical engine bindless layout (one pipeline layout, large `UPDATE_AFTER_BIND` descriptor arrays, push constants only): `NonUniformResourceIndex` on textures and buffers, working sets from 16 KB to 64 MB, raw against typed views against buffer device address, descriptor divergence, stores, append/histogram/contention/max atomics. **Every test has a CPU reference, so `--verify` checks that the compiler and driver produce correct results.**
- **Three shader compilers:** Slang, DXC and glslang, so you can also compare compilers.

Bindless will be probably renamed, as it is increasing it test size. Planning to add also storage images, image atomics, graphics pipelines, to test even more. Parts of the tests (like histograms) are inspired by real usage in modern engines.

This is a personal project, not affiliated with any GPU vendor.

## Results

| GPU | Type | Driver | Files |
|---|---|---|---|
| NVIDIA GeForce RTX 3070 Laptop (Ampere) | discrete, laptop | 610.74 | `results/rtx3070-laptop_*.csv` |
| AMD Radeon Graphics, Ryzen 5000 "Cezanne" (Vega) | integrated, laptop | 2.0.279 | `results/cezanne_*.csv` |
| **your GPU here** | | | see [Contributing results](#contributing-results) |

Tables and notes: [`results/README.md`](results/README.md). More GPUs are very welcome, see below.

## Contributing results

Results from more GPUs are the most useful contribution: desktop and mobile, any vendor and generation.

```sh
perftest-vk --verify                                   # must pass before timing means anything
perftest-vk --shaders build/shaders/dxc   --csv <gpu>_dxc.csv
perftest-vk --shaders build/shaders/slang --csv <gpu>_slang.csv
```

Open a pull request that adds the CSV files to `results/`, or attach them to an issue. Please mention desktop or laptop, and whether the machine was plugged in and set to maximum performance. If `--verify` fails on your GPU, please open an issue with its output: that is even more interesting.

## Quick start

You need CMake ≥ 3.16, a C++17 compiler, Python 3 and the [Vulkan SDK](https://vulkan.lunarg.com/) (it ships `slangc`, `dxc` and `glslangValidator`). Use **Vulkan SDK 1.4.363.0 or later**, and prefer the DXC shader set for timing: Slang up to 2026.17 mishandles some `NonUniformResourceIndex` cases, which gives wrong results on some GPUs (see [Compiler notes](#compiler-notes)).

**Windows** (Visual Studio 2022 or later):

```sh
cmake -S . -B build
cmake --build build --config Release
build\Release\perftest-vk.exe
```

**Linux:**

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
build/perftest-vk
```

The build compiles the shaders once per compiler found, into `build/shaders/slang`, `build/shaders/dxc` and `build/shaders/glslang`. Without options, the program picks the first discrete GPU and the first shader set it finds (Slang, then DXC, then glslang), and runs both suites.

## Reading the output

An example:

```
Device:   ...
Driver:   ...
Shaders:  build/shaders/dxc (dxc ...)
Dispatch: 4 x 1024 thread groups of 256 threads, 30 warm-up + 30 benchmark frames
Config:   default (subgroup size 32, default)

Performance compared to Buffer<RGBA8>.Load random

Buffer<RGBA8>.Load uniform: 1.234ms 3.210x
Buffer<RGBA8>.Load random: 3.961ms 1.000x
Texture2D<RGBA32F>.Load random: 5.021ms 0.789x  (noisy: cv 7%)
cbuffer{float4} load random: skipped (maxUniformBufferRange < 16400)
```

- **`X.XXXms`**: GPU time of the test summed over the benchmark frames (30 by default), as in the original.
- **`Y.YYYx`**: speed relative to `Buffer<RGBA8>.Load random`. Above 1 is faster, below 1 is slower.
- **`(noisy: cv N%)`**: the time changed by more than 5% between frames. Trust only differences well above that.
- **`skipped (...)`**: the GPU, the driver or the shader set does not support the test; the reason is given.

Added the header to record the device, driver, compiler and configuration.

## Common tasks

```sh
perftest-vk --list                                   # devices and test names
perftest-vk --device 1                               # second GPU (laptops often list the iGPU too)
perftest-vk --suites bindless --filter "working set" # only some tests
perftest-vk --verify                                 # check the bindless results instead of timing them
perftest-vk --csv mygpu_dxc.csv --shaders build/shaders/dxc
```

**Compare compilers** on one GPU:

```sh
perftest-vk --shaders build/shaders/dxc   --csv dxc.csv
perftest-vk --shaders build/shaders/slang --csv slang.csv
python scripts/compare.py "DXC=dxc.csv" "Slang=slang.csv"
```

**Compare GPUs:** run with `--csv` on each machine, then `python scripts/compare.py a.csv b.csv c.csv`.

**Check that the compiler and driver produce correct code:** `perftest-vk --verify` (exit code 3 if any test returns wrong values). Do this once per GPU and shader set before trusting the bindless numbers.

**Cost of bounds checking:** compare a normal run with `--robustness`.

**Wave size on RDNA:** compare `--subgroup-size 32` with `--subgroup-size 64`.

**Less noise:** laptops and iGPUs change clocks with temperature and power. Plug in, select maximum performance, and use `--frames 100 --shuffle`.

## Options

| Option | Meaning |
|---|---|
| `--device N` (or just `N`) | physical device index, as printed at start-up and by `--list` (default: first discrete GPU) |
| `--shaders DIR` | shader set to use, relative to the current directory (default: `shaders/<slang\|dxc\|glslang>` next to the executable, then `build/shaders/...`) |
| `--suites LIST` | comma-separated: `original`, `bindless` (default: both) |
| `--filter TEXT` | only tests whose name contains TEXT |
| `--warmup N` / `--frames N` | warm-up and benchmark frames (default 30 / 30); times and statistics use the benchmark frames |
| `--shuffle` | new dispatch order every frame, so clock ramps and cache state do not always favour the same tests |
| `--groups X Y` | dispatch size of the original tests (default 4×1024 groups of 256 threads = the original's 1024×1024 threads) |
| `--robustness` | enable `robustBufferAccess`, plus `robustBufferAccess2` / `robustImageAccess2` if available |
| `--subgroup-size N` | require subgroup size N for every pipeline (needs Vulkan 1.3 `subgroupSizeControl`) |
| `--verify` | check the results of the bindless suite instead of timing (exit code 3 on wrong results) |
| `--csv FILE` | also write the results as CSV |
| `--validation` | enable `VK_LAYER_KHRONOS_validation` (exit code 2 if it reports anything) |
| `--list` | list devices and tests, then exit |
| `--help` | list the options |

### CSV and `compare.py`

CSV columns: `suite, test, total_ms, ratio, median_ms, min_ms, cv_pct, status, device, vendor_id, device_id, driver, vulkan, compiler, config`. Every row carries the device, driver, compiler and configuration, so files from different machines can be merged.

`scripts/compare.py` turns several CSVs into one Markdown table. Each column after the first also shows the speed relative to the first file.

| Option | Meaning |
|---|---|
| `LABEL=file.csv` | column label (default: device / compiler / config from the CSV) |
| `--metric total\|median\|min` | total over frames (default, like the original), or median / minimum per dispatch |
| `--suite NAME`, `--filter TEXT` | select tests |
| `--markdown FILE` | write the table to a file |
| `--plot FILE.png` | also draw a bar chart of the relative speed (needs matplotlib) |

## Test suites

### `original`

The 138 tests of PerfTest, with the original shaders and names: `Buffer<format>.Load`, `ByteAddressBuffer.Load/Load2/Load3/Load4` (aligned and unaligned), `StructuredBuffer<float/float2/float4>.Load`, `cbuffer{float4}` load, `Texture2D<format>.Load` and `.Sample` (nearest and bilinear), for 9 formats from R8 to RGBA32F, each with uniform, linear and random addresses. They read at most 16 KB, so they measure the load path, not memory bandwidth.

### `bindless`

All tests share one layout (`src/bindless_layout.*`, `shaders/bindless/bindless.hlsli`): one descriptor set with runtime-sized `UPDATE_AFTER_BIND | PARTIALLY_BOUND | UPDATE_UNUSED_WHILE_PENDING` arrays (binding 0: sampled images, binding 1: every buffer view on one aliased binding), an immutable sampler, and 128 bytes of push constants. The arrays are engine-sized (16384 textures, 65536 buffers, clamped to the device limits). A test is a shader plus a row in the table in `src/bindless_suite.cpp`.

| Test | What it measures |
|---|---|
| `texture NonUniform: 1 texture, uniform index (control)` | 32 samples per thread from one texture |
| `texture NonUniform: N textures/wave, NonUniformResourceIndex` | 1 to 32 distinct textures per wave, divergence handled by the driver (the material-texture case of a visibility buffer) |
| `texture NonUniform: N textures/wave, scalarized (WaveReadLaneFirst loop)` | the same with manual scalarization |
| `working set S pattern, N loads: P` | N `uint4` loads per thread (32 for 64 MB, 256 for smaller sets) over 16 KB, 256 KB, 4 MB and 64 MB (L1, L2, last-level cache, DRAM) through `StructuredBuffer<uint4>`, `ByteAddressBuffer.Load4`, the raw aligned load (Slang `LoadAligned`, DXC `Load<uint4>`) and buffer device address. Patterns: `linear` (coalesced), `random` (a random address per lane), and for the first two paths `wave-uniform` (one random address per wave, like per-instance data) and `wave-coherent` (each wave reads a contiguous block at a random position) |
| `buffer path: ...` | 256 loads per thread from 16 KB through the raw aligned load or BDA (align 4 or 16) |
| `descriptor index: ...` | a raw buffer reached through a slot index: dynamically uniform, or each lane picks one of 8 slots with `NonUniformResourceIndex` (pure descriptor divergence) |
| `typed alias: ...` | two typed views of 32-byte structs aliased on the same binding |
| `8 buffers: ...` | divergent access to 8 regions of a 128 KB buffer: 8 slots with `NonUniformResourceIndex`, a pointer per lane (BDA), or one slot plus a divergent offset, each with a uniform control |
| `store S linear/sparse/random, N stores: P` | N stores per thread (32 for 64 MB, 256 for smaller sets) of 4, 8 or 16 bytes through `RWStructuredBuffer`, `RWByteAddressBuffer.Store/Store2/Store4` and BDA, into 64 MB (every path) or 16 KB - 4 MB (16-byte typed and raw); sparse writes one element per 64-byte line |
| `atomic append, P% of lanes: ...` | append/compaction: one `InterlockedAdd` per active lane against one per wave (`WavePrefixCountBits`) |
| `atomic histogram 256 bins, uniform/skewed: ...` | global `InterlockedAdd` against groupshared histogram + one global add per bin per group; skewed = all values in 16 bins |
| `atomic counters N: ...` | contention: random `InterlockedAdd` (result unused / used) and `InterlockedOr` over 1, 64, 4096 or 1M addresses |
| `atomic max, N targets: ...` | `InterlockedMax` 32-bit and 64-bit `(depth << 32) \| id` (visibility buffer); 64-bit needs `shaderBufferInt64Atomics` |

With `--verify` every thread writes its result and the CPU recomputes it. The data is never all zeros, so reading the wrong descriptor, address or element is caught. The texture tests use 32 RGBA8 64×64 textures with point sampling; the working-set buffer is 64 MB.

The bindless shaders need Slang (`-profile spirv_1_5`) or DXC (`-T cs_6_6`); glslang's HLSL front end cannot compile them.

## Building in more detail

Each compiler found by CMake produces its own shaders. To use a specific compiler: `-DSLANGC_EXECUTABLE=...`, `-DDXC_EXECUTABLE=...`, `-DGLSLANG_EXECUTABLE=...`; to disable one: `-DPERFTEST_SHADERS_SLANG=OFF` (also `_DXC`, `_GLSLANG`). Shaders can also be compiled by hand:

```sh
python scripts/compile_shaders.py --compiler slang --exe /path/to/slangc
```

Shaders that a compiler does not support are reported as `skip` and their tests are skipped at run time. The original shaders keep their HLSL registers, mapped with register shifts: `b0 → 0`, `t0 → 1`, `u0 → 2`, `s0 → 3`.

Requirements at run time: Vulkan 1.1 for the `original` suite; Vulkan 1.2 descriptor indexing for `bindless` (skipped otherwise); `bufferDeviceAddress` + `shaderInt64` for the BDA tests; Vulkan 1.3 for `--subgroup-size`.

## Compiler notes

For now DXC is the safest choice, Slang <= 2026.17 has NonUniform bugs, run with `--verify`.
More info on [`results/`](results/README.md).

## Differences from the DX11 original

- **Timing:** a compute→compute barrier before every dispatch, then `vkCmdWriteTimestamp(BOTTOM_OF_PIPE)` around it. This reproduces the serialization D3D11 inserts between dispatches that write the same UAV, so each time covers one dispatch.
- **Resources:** device-local, initialized on the GPU (zeros, like the original); constant buffers written with `vkCmdUpdateBuffer`.
- **Unsupported formats:** tests are marked `skipped (...)` instead of failing.
- **`cbuffer{float4}` tests** need `maxUniformBufferRange ≥ 16400`; the Vulkan minimum is 16384, so some GPUs skip them.

## Methodology and limits

First of all, I wanted a complete suite using different **compilers** so the results can be compared.

Second, **working-set sizes** from 16 KB (L1) to 64 MB (DRAM), to see how each access path behaves in L1, L2 and memory.

Third, laptops add noise because of **power management**. Each test reports median, minimum and coefficient of variation, small working sets run more iterations, and `--shuffle` changes the dispatch order every frame.

Fourth, `--verify` performs a CPU test of the results for correctness, possible because the Buffers/Textures are deterministic. It checks only 4096 threads, and can be improved.

## Status
Tested on RTX 3070 Laptop, Vega iGPU (Cezanne) with DXC; with Slang one test fails on the Vega iGPU (see [Compiler notes](#compiler-notes)). Results: [`results/`](results/README.md).

## License

MIT. PerfTest © 2016-2017 Sebastian Aaltonen (see `LICENSE-perftest.md`); the files in `shaders/` (except `shaders/bindless/`) are the original ones.

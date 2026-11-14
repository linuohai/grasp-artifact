# GRASP: Register-Chain-Aware GPU Prefetching

This repository is the artifact for the paper "GRASP: ISA-Exposed
Register-Chain Prefetching for Indirect Memory Access on GPUs" (IEEE ICCD
2026). It contains the GRASP prefetcher implementation as a set of source
files that overlay a public simulator framework, plus the scripts and
offline data needed to reproduce the paper's GRASP results.

It does **not** contain: the SOTA baseline prefetcher implementations
(Snake, CAPS, Spare Register) — Section "Baseline Configuration" below
discloses their exact effective parameters instead of shipping code;
workload sources, datasets, or pre-generated instruction traces; or the
RTL/synthesis artifacts.

## 1. What this repository is

This is **based on the Accel-Sim framework's `dev` branch (post-v1.3.0, upstream commit `d7f397a`)**,
which in turn wraps GPGPU-Sim 4.x. GRASP is implemented as a new prefetcher
module plus a small set of hooks into the upstream simulator core (cache,
core pipeline, memory-request object). `docs/grasp_hook_points.md`
enumerates every file this repository adds or modifies relative to that
upstream base, with a one-line description of what changed and why.

Layout (every path below mirrors its location inside the upstream
`accel-sim-framework` tree, so it can be overlaid directly — see §3):

```
gpu-simulator/
├── CMakeLists.txt, accel-sim.cc, accel-sim.h      # top-level driver: registers -grasp_* options
├── setup_environment.sh                            # environment/toolchain setup (unmodified in spirit; upstream URL only)
├── configs/tested-cfgs/<CONFIG>/trace.config        # per-config trace-replay parameters (15 configs)
├── trace-driven/trace_driven.{cc,h}                 # trace replay + chain-CSV loading
├── trace-parser/trace_parser.{cc,h}                 # trace format parsing (minor passthrough change)
└── gpgpu-sim/
    ├── src/abstract_hardware_model.h                # SASS mnemonic accessor used by the chain detector
    ├── src/cuda-sim/cuda-sim.cc                      # one defensive assert, unrelated to GRASP
    └── src/gpgpu-sim/
        ├── grasp_{tables,chain_detector,tracer,prefetcher}.{h,cc}   # GRASP itself
        ├── ima_prefetcher.{h,cc}                     # legacy single-hop fallback (grasp_enable=false path)
        ├── l1_tracer.*, l2_tracer.*, l2_bw_tracer.*, hbm_partition_tracer.*,
        │   icnt_tracer.*, issue_tracer.*, stall_reason_pc_stats.*    # generic tracing infra the eval scripts consume
        ├── shader.{cc,h}, gpu-sim.{cc,h}, gpu-cache.{cc,h}           # core hook points (see docs/grasp_hook_points.md)
        ├── mem_fetch.{cc,h}, dram.cc, l2cache.{cc,h}, icnt_wrapper.{cc,h},
        │   local_interconnect.{cc,h}, scoreboard.{cc,h}              # minor call-in points for the above
        └── configs/tested-cfgs/<CONFIG>/gpgpusim.config              # 15 GPU configurations (12 A100 variants + 3 SM70/Fermi-like)
data/golden_chains/strict_selected_chain_instances.csv   # offline register-chain catalog GRASP consumes at trace-load time
traceL1                        # single entry point: run one simulation (baseline or --grasp)
run_batch_experiments.sh       # convenience wrapper to launch many traceL1 runs in parallel
get-accel-sim-traces.py        # official Accel-Sim tool to download published traces
docs/grasp_hook_points.md      # file-by-file diff summary against upstream
```

## 2. Build

1. Clone the upstream framework and GPGPU-Sim, and check out the two
   commits this overlay was written against:
   ```
   git clone -b dev \
       https://github.com/accel-sim/accel-sim-framework.git upstream-accel-sim
   git -C upstream-accel-sim checkout d7f397a1766103cb9504715dfd8150da7c451607
   git clone -b dev \
       https://github.com/accel-sim/gpgpu-sim_distribution.git \
       upstream-accel-sim/gpu-simulator/gpgpu-sim
   git -C upstream-accel-sim/gpu-simulator/gpgpu-sim \
       checkout b18ee3977962921c49fabe06d26ae19694497c26
   ```
   GPGPU-Sim is not a git submodule of the framework at this commit. Clone
   it by hand as shown, before step 3: if the directory is missing,
   `setup_environment.sh` clones the current `dev` branch instead, and
   newer `dev` snapshots changed interfaces the overlay calls
   (`linear_to_raw_address_translation::init`), so the build fails.

2. Overlay this repository's `gpu-simulator/` directory on top, replacing
   files with the same relative path and adding the ones that don't yet
   exist:
   ```
   rsync -a gpu-simulator/ upstream-accel-sim/gpu-simulator/
   cp -r data upstream-accel-sim/
   cp traceL1 run_batch_experiments.sh get-accel-sim-traces.py upstream-accel-sim/
   chmod +x upstream-accel-sim/traceL1 upstream-accel-sim/run_batch_experiments.sh \
             upstream-accel-sim/get-accel-sim-traces.py
   cd upstream-accel-sim
   ```

3. Build (standard Accel-Sim flow; requires a CUDA toolkit and gcc/g++;
   tested with gcc 13.3.0 and CUDA 12.8):
   ```
   source ./gpu-simulator/setup_environment.sh
   make -j -C ./gpu-simulator/
   ```
   The simulator binary lands at `gpu-simulator/bin/release/accel-sim.out`.

## 3. Reproduce a GRASP run

`traceL1` is the single entry point for both the no-prefetch baseline and
GRASP. It self-detects the repository root, so it can be run from anywhere
inside the tree.

```
# No-prefetch baseline
./traceL1 <trace_key> my_baseline_run

# GRASP enabled (uses the chain catalog at data/golden_chains/ by default)
./traceL1 --grasp <trace_key> my_grasp_run

# Pick a different GPU config (default SM80_A100)
./traceL1 -c SM7_QV100 --grasp <trace_key> my_grasp_run_v100

# Pass raw simulator options straight through (e.g. to run the throttle
# configuration the paper's reported results were produced with — see below)
./traceL1 --grasp --sim-args "-grasp_tc_mshr_lo 40 -grasp_tc_acc_hi 70" \
    <trace_key> my_grasp_run_paper_config
```

`<trace_key>` must exist in `traceL1`'s internal `TRACE_MAP` and point at a
real Accel-Sim instruction trace directory under `hw_run/traces/...`
(a symlink or directory you provide — see §4).

**Reproducing the paper's reported numbers.** The paper's reported
results were produced with the throttle thresholds set via

```
--sim-args "-grasp_tc_mshr_lo 40 -grasp_tc_acc_hi 70"
```

as in the example above; all other options stay at their compiled
defaults. The throttle controller (`grasp_tc_mode=5`, the compiled
default) linearly interpolates the effective MSHR-occupancy threshold
between `grasp_tc_mshr_lo` (used once recent accuracy falls to
`grasp_tc_acc_lo` or below) and `grasp_tc_mshr_hi` (used once recent
accuracy reaches `grasp_tc_acc_hi` or above) — see `grasp_prefetcher.cc`'s
`tc_mode == 5` branch for the exact formula. Every run prints its full
effective configuration in the `GRASP_CONFIG:` log line.

Logs land in `result/log/`;
each run ends with an `EXPERIMENT SUMMARY` block reporting completion
status and, in `--grasp` mode, the aggregated coverage/timeliness/accuracy/
storage counters described in the paper.

`run_batch_experiments.sh [baseline|grasp|all]` launches many `traceL1`
invocations in parallel with a simple memory/OOM monitor; it is a
convenience wrapper only, not required to reproduce any single result.

## 4. Obtaining traces

This repository does not ship instruction traces (they are large and
workload-specific). Two options:

- **Use published traces.** `./get-accel-sim-traces.py` is the official
  Accel-Sim tool for downloading pre-built traces from the public
  Accel-Sim trace archive; point `traceL1`'s trace directory at the result.
- **Generate your own.** Trace any CUDA binary with the standard Accel-Sim
  NVBit tracer (`util/tracer_nvbit/` in the upstream framework) against
  your own workload sources, then add an entry to `traceL1`'s `TRACE_MAP`
  pointing at the resulting trace directory.

GRASP's register-chain catalog (`data/golden_chains/`) identifies IMA
(indirect-memory-access) load chains by kernel function name, instruction
PC, and SASS opcode, and matches them against the kernel PCs that appear
in your trace at runtime. The shipped catalog was extracted from the
workload binaries used in the paper (its `algorithm`, `impl`, `sm`, and
`function` columns record which). A trace recorded from a different build
of a workload has different PCs and needs a catalog extracted from that
build.

## 5. Baseline Configuration

The three SOTA prefetchers compared against GRASP in the paper — Snake
(variable-length stride-chain prefetching), CAPS (CTA-aware prefetching),
and Spare Register (the only prior GPU indirect-memory-access prefetcher)
— are **not** included as source code in this repository (their
implementations are re-creations of the original published designs, not
released here to keep the artifact focused). What follows is the complete
set of effective parameter values used to produce the paper's baseline
numbers, extracted read-only from the implementation's option defaults.
No workload received bespoke tuning; every workload runs against the same
default values below.

### Snake (variable-length stride-chain prefetching)

| Parameter | Value | Source |
|---|---|---|
| PC-indexed table entries / SM | 32 | matches original publication's default |
| Training warp count | 3 | implementation default |
| Maximum chain length | 2 | matches original publication's default |
| Decoupled prefetch buffer | **not implemented** | see note below |

Notes:
- The implementation uses a single PC-indexed table that blends the
  original design's separate Head-Table and (promoted) Tail-Table state,
  sized by the entry above. That (unreleased) implementation's own option
  parser separately registers a distinct 10-entry tail-table size, but it
  is not read anywhere in the prefetcher's implementation — it has no
  effect and is therefore omitted from the table above.
- The original Snake design includes a prefetch buffer decoupled from
  L1D; this implementation writes prefetched data directly into L1D instead
  (a documented simplification, expected not to change the qualitative
result that stride-based chains fail to help indirect-memory-access
loads). This is a mechanism-level simplification, not a table-size or
threshold deviation.

### CAPS (CTA-aware prefetching)

| Parameter | Value | Source |
|---|---|---|
| PerCTA table entries / CTA | 4 | matches original publication's default |
| DIST table entries / SM | 4 | matches original publication's default |
| Misprediction threshold | 128 | matches original publication's default |

### Spare Register (the sole prior GPU indirect-memory-access prefetcher)

| Parameter | Value | Source |
|---|---|---|
| Value-tag table entries | 16 | **no traceable origin in the original publication; not exercised by this implementation (see note)** |
| Load-pair table entries | 8 | **no traceable origin in the original publication; not exercised by this implementation (see note)** |
| Training iterations | 3 | implementation default |
| Prefetch distance | 1 | implementation default (see note — the original mechanism is adaptive) |
| Prefetched-data destination | L1D cache only, unconditionally (not gated by any option — the implementation has no register-write path to disable/enable) | see note |

Notes:
- The original publication's detection mechanism is a 2-bit tag per
  architectural register (a fixed hardware structure, not a
  parameterized table). This implementation substitutes a pair table
  that associates an index load's PC with its dependent data load's PC —
  a trace-driven-environment approximation of the paper's register-tag
  hardware, chosen for the same reason GRASP's own chain detector uses an
  offline chain catalog rather than runtime register-value tracking:
  trace-driven replay does not expose load return values. Because the
  pair table replaces the value-tag/load-pair hardware entirely, the two
  "table entries" parameters above are registered options but do not
  affect prefetch decisions in practice.
- The original publication's mechanism writes prefetched data into
  *both* L2/L1 cache and spare architectural registers. This
  implementation retains only the cache-write path. The original paper
  itself reports an ablation that writes cache only ("SPREF1-C"), scoring
  lower than its full design; this implementation is a strict subset of
  even that ablation (it additionally omits the paper's register-resident
  head-load handling).
- The original publication uses a shadow iteration counter to estimate
  how many loop iterations remain and adapts the prefetch distance
  accordingly. This implementation uses a fixed distance (registered
  option, default 1) instead of that adaptive mechanism.

## 6. Attribution

If you use this repository, please cite the GRASP paper:

Haonan Sun, Chen Yang, Xiaobin Men, Zhuowei She, and Hu He, "GRASP:
ISA-Exposed Register-Chain Prefetching for Indirect Memory Access on GPUs,"
in Proc. IEEE International Conference on Computer Design (ICCD), 2026.

```bibtex
@inproceedings{sun2026grasp,
  title     = {{GRASP}: {ISA}-Exposed Register-Chain Prefetching for Indirect Memory Access on {GPUs}},
  author    = {Sun, Haonan and Yang, Chen and Men, Xiaobin and She, Zhuowei and He, Hu},
  booktitle = {Proc. IEEE International Conference on Computer Design (ICCD)},
  year      = {2026}
}
```

This work is based on the Accel-Sim framework's `dev` branch
(post-v1.3.0, upstream commit `d7f397a`), which wraps GPGPU-Sim. Please cite
the Accel-Sim and GPGPU-Sim papers when using this simulator. The
GRASP-specific files in this repository are released under the BSD 3-Clause
License (see `LICENSE`); files derived from Accel-Sim and GPGPU-Sim keep their
original license headers. The upstream license texts are
reproduced in `third_party/ACCEL-SIM-LICENSE` and
`third_party/GPGPU-SIM-COPYRIGHT`.

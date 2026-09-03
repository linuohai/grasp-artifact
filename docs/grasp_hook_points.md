GRASP Hook Points — Summary
============================

This document summarizes where and how GRASP integrates into the upstream
Accel-Sim / GPGPU-Sim simulator. It is generated from a line-level diff
against the upstream `dev` branches of `accel-sim-framework` and
`gpgpu-sim_distribution` (post-v1.3.0), so the line counts below are
approximate for this snapshot (comment-only edits made after this table
was generated shift them by a few lines per file; they will also drift
further as upstream advances).

New files (GRASP's own logic, no upstream equivalent)
-------------------------------------------------------

| File | Purpose |
|---|---|
| `grasp_tables.{h,cc}` | Fixed-capacity table primitives (CD/CT/TT/PRB entry types) shared by the prefetcher. |
| `grasp_chain_detector.{h,cc}` | Chain Detector (CD): observes committed loads per warp, matches them against the offline chain catalog. |
| `grasp_tracer.{h,cc}` | Optional CSV tracer for GRASP-internal events (debug/analysis only, not required for a functional run). |
| `grasp_prefetcher.{h,cc}` | Top-level prefetcher: Chain Table (CT), Target Table (TT), Prefetch Request Buffer (PRB), Index/Data Prefetch Units (IPU/DPU), and the throttle controller (TC). This is GRASP's main file (~1,500 lines). |
| `ima_prefetcher.{h,cc}` | Legacy single-hop IMA prefetcher, kept as a fallback path selected when `grasp_enable=false`. Retained because `shader.h`/`shader.cc` still reference it unconditionally at compile time. |

New files (generic tracing infrastructure GRASP's evaluation depends on)
--------------------------------------------------------------------------

These are not GRASP-specific, but the build is not self-contained without
them: `CMakeLists.txt` lists them as required translation units, and
`shader.cc`/`gpu-sim.cc` call into them unconditionally.

| File | Purpose |
|---|---|
| `l1_tracer.{h,cc}` | Per-access L1D cache event trace (used to compute the paper's coverage/timeliness/accuracy numbers offline). |
| `l2_tracer.{h,cc}`, `l2_bw_tracer.{h,cc}` | L2 partition access and bandwidth trace. |
| `hbm_partition_tracer.{h,cc}` | HBM/DRAM partition bandwidth trace. |
| `icnt_tracer.{h,cc}` | Interconnect (NoC) trace. |
| `issue_tracer.{h,cc}` | Per-warp instruction issue trace (used for the N-level scheduler / stall-reason analysis, orthogonal to GRASP). |
| `stall_reason_pc_stats.{h,cc}` | Per-PC stall-reason statistics aggregation. |

Modified upstream files
--------------------------

| File | Diff size (upstream → this snapshot) | What changed |
|---|---|---|
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/shader.cc` | +1,875 / −(part of 91) lines | Core integration point. Owns the L1D access path: on a demand load miss/hit it calls into `m_grasp`/`m_ima_prefetcher`; drains the prefetch queue (`m_prefetch_queue`) into the L1D pipeline; instantiates GRASP/IMA prefetcher objects per-SM at kernel launch (`init_warps`); prints `GRASP_*` stat blocks at kernel end. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/shader.h` | +296 lines | Declares `m_grasp`, `m_ima_prefetcher`, GRASP config fields (`grasp_config_t`-equivalent members, `grasp_enable`, `grasp_tc_*`, `grasp_speculative_stride`, etc.), and the `print_grasp_stats`/`print_grasp_config` interface. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.cc` | +521 lines | Command-line option registration for every `-grasp_*` flag; aggregates and prints per-SM GRASP stats into the global `EXPERIMENT SUMMARY` block at end of run. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-sim.h` | +213 lines | GRASP-related fields on the shared GPU config object. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-cache.cc` | +207 lines | L1D reservation-fail accounting distinguishes prefetch vs. demand requests (`is_ima_prefetch()`); MSHR-occupancy query used by the throttle controller. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/gpu-cache.h` | +119 lines | Declares the reservation-fail reason enum used by GRASP's RFAIL stats, and the MSHR occupancy-ratio accessor. |
| `gpu-simulator/gpgpu-sim/src/abstract_hardware_model.h` | +7 lines | Adds a virtual accessor so `trace_warp_inst_t` can report the full SASS mnemonic (needed by the chain detector to recognize `IMAD.WIDE`/`LDG.E` patterns). |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/mem_fetch.{h,cc}` | +32 lines (`.h`) | Adds `is_ima_prefetch()` / a prefetch-origin flag on the in-flight memory-request object, read by `gpu-cache.cc` and `shader.cc` to separate prefetch traffic from demand traffic in every counter. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/dram.cc` | +31/−2 lines | DRAM-side accounting hook so prefetch-issued DRAM accesses are attributed correctly in the HBM bandwidth trace. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/l2cache.{h,cc}` | +10 / +19 lines | Call-in points for `l2_tracer`/`l2_bw_tracer`; no change to L2 policy itself. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/icnt_wrapper.{h,cc}` | +3 / +17 lines | Call-in points for `icnt_tracer`. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/local_interconnect.{h,cc}` | +2 / +14 lines | Minor counter exposed for the interconnect trace. |
| `gpu-simulator/gpgpu-sim/src/gpgpu-sim/scoreboard.{h,cc}` | +1 / +5 lines | Exposes a scoreboard-occupancy counter consumed by `issue_tracer`. |
| `gpu-simulator/gpgpu-sim/src/cuda-sim/cuda-sim.cc` | +1 line | Defensive `assert(kernel)` in `ptx_sim_kernel_info()`; unrelated to GRASP, kept for diff fidelity. |
| `gpu-simulator/trace-driven/trace_driven.{h,cc}` | +123 / +879 lines | Loads the chain CSV (`data/golden_chains/strict_selected_chain_instances.csv` by default) at trace-parse time and builds the per-kernel `ima_pair_table` GRASP's chain detector consults; also implements `is_ima_data_pc()` used by baseline-mode PC filtering (baseline implementations themselves are not part of this repository). |
| `gpu-simulator/accel-sim.{cc,h}` | +49 / +4 lines | Wires the new `-grasp_*` and `-ima_*` options into the top-level option parser so they reach `gpgpu_sim`. |
| `gpu-simulator/CMakeLists.txt`, `gpu-simulator/gpgpu-sim/src/gpgpu-sim/CMakeLists.txt` | small | Register the new source files above as build targets. |
| `gpu-simulator/trace-parser/trace_parser.{h,cc}` | +1 / +3 lines | Minor field passthrough needed by the extended trace_driven chain-loading path. |
| `gpu-simulator/setup_environment.sh` | +5/−3 lines | Cosmetic; the upstream-repo URL used by its auto-clone fallback has been repointed to the public `accel-sim/gpgpu-sim_distribution` upstream in this release (this repository already ships `gpu-simulator/gpgpu-sim/` directly, so the fallback clone path is normally not exercised). |

Not included in this repository
-----------------------------------

- **SOTA baseline prefetcher implementations** (Spare Register / Snake / CAPS) — the paper's evaluation reproduces them from their original publications; this repository ships only their *effective configuration values* (see `README.md`'s baseline configuration table), not source code.
- **Workload sources / datasets / pre-generated traces** — obtain via `get-accel-sim-traces.py` (public Accel-Sim traces) or generate your own with the standard Accel-Sim NVBit tracer against your own CUDA workloads; see `README.md`.
- **RTL / synthesis artifacts** — not part of this simulator repository; will accompany the camera-ready release.

How to read this document
-----------------------------

Every file listed above is included in this repository under the same
relative path it has in `accel-sim-framework`. To reconstruct a buildable
tree: clone `accel-sim-framework` (dev branch, post-v1.3.0) with its
`gpgpu-sim` submodule initialized, then overlay (copy on top of, replacing
originals) every file under this repository's `gpu-simulator/` directory.
See `README.md` for the exact commands.

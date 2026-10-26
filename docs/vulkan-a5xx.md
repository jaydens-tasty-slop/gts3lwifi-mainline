# Vulkan for the Adreno 530 on mainline Linux: feasibility report

Scope: Samsung Galaxy Tab S3 (SM-T820, APQ8096/MSM8996, Adreno 530, `qcom,adreno-530.2`, chip id 0x05030002) running mainline Linux v6.19.5 with drm/msm and Mesa main at ab1502f (2026-10-25).

How to read the citations and labels:
- Paths starting with `src/` or `docs/` are in the Mesa tree. Paths starting with `drivers/` or `arch/` are in the kernel tree.
- **Verified** means the research pass checked it in source and an independent reviewer checked it again.
- **Inference** means it is reasoning that was not checked against source or hardware.
- **Unknown** means nobody has measured it.
- Effort figures are estimates, not measurements.

---

## 1. Short answer

1. Vulkan on the Adreno 530 under mainline Linux is technically achievable, but only as a new driver. Turnip cannot simply be extended: it rejects gen 5, and it is built on a6xx-only features (bindless descriptors, mode-tagged draw states, shader preambles). The Mesa maintainers have said that an a5xx Vulkan driver would make more sense as a separate driver.
2. The realistic route is a new a5xx driver that reuses Mesa's Vulkan runtime, the ir3 compiler, the a5xx register XML, the layout code and turnip's hardware-neutral parts. The gallium fd5 GL driver serves as the reference for state emission, and descriptors are flattened into per-stage tables at draw time.
3. Qualcomm's own driver ran Vulkan 1.1 on this exact tablet, so the hardware is not the limit. Vulkan 1.0 and then 1.1 is a realistic target, 1.2/1.3 may be possible later, and 1.4 or DXVK is not realistic.
4. For a part-time hobby project with AI help, these are estimates:
   - first offscreen triangle: about 4–8 months;
   - a working Vulkan 1.0 subset that passes a meaningful share of dEQP-VK: about 1–1.5 years;
   - conformance-grade 1.1: longer.
5. The first step is the same whatever is decided about Vulkan: get the GPU and freedreno GL running headless on the owner's tablet, which is weeks of work. lavapipe (CPU Vulkan) can be installed today as a stopgap and as a test harness.

---

## 2. What already exists

### 2.1 Freedreno GL and ir3 on a5xx (verified)

**Gallium fd5 backend** (`src/gallium/drivers/freedreno/a5xx`)
- About 5.8k lines of .c, about 6.6k with headers.
- Exposes GLES 3.1 and desktop GL 3.1 (`glsl_feature_level=140`, ESSL 320 at `src/gallium/drivers/freedreno/freedreno_screen.c:533-535`).

**Mesa CI on the same GPU**
- Mesa CI runs dEQP-GLES2/3/31 and KHR-GLES31 on an a530 in a DragonBoard 820c (APQ8096, the same SoC and GPU). See `src/freedreno/ci/deqp-freedreno-a530.toml`, `src/freedreno/ci/gitlab-ci.yml:68-78` and `src/freedreno/ci/freedreno-a530-fails.txt`.
- Caveats:
  - Pre-merge runs only half the suite (`DEQP_FRACTION: 2`).
  - GLES31 image, SSBO and compute tests appear in the flakes list.
  - One texture-buffer-atomic test is skipped because it causes GPU faults.
- Even so, this gives a ready-made expected-results baseline to diff the Tab S3 against.

**ir3 a4xx/a5xx backend** (`src/freedreno/ir3/ir3_context.c:49-53`, `src/freedreno/ir3/ir3_a4xx.c`)
- SSBOs: ldgb/stgb (32-bit only).
- Images: ldib/stib, typed atomics.
- 64-bit global ldg/stg, ldg.a/stg.a and global atomics (`ir3_a4xx.c:417-512`). The comment there says "only measured on a5xx". Plain ldg is exercised in CI through UBO loads; ldg.a/stg.a and the global atomics have no a530 CI coverage.
- Dynamic texture/sampler indices without bindless via s2en (`src/freedreno/ir3/ir3_compiler_nir.c:3696-3720`).
- Private memory (spilling), the shared register file and 16-bit ALU are available from gen 5.

**ir3 gaps on gen 5**
- No preambles, bindless, shfl, getfiberid or getlast (`src/freedreno/ir3/ir3_compiler.c:253-321`).
- No clip/cull distances (`ir3_compiler.c:293-294` TODO).
- No tessellation or GS (`ir3_compiler.c:338-340`).
- The 448-vec4 const window, with the UAV table in the top 64 vec4 (`ir3_compiler.c:329-336`).

**Other shared code**
- `src/freedreno/registers/adreno/a5xx.xml`.
- An `fd_dev_info` entry for 530/540 (`src/freedreno/common/freedreno_devices.py:111-127`).
- A minimal `fdl5` layout (`src/freedreno/fdl/fd5_layout.c`, 96 lines).
- The a5xx format table (`src/gallium/drivers/freedreno/a5xx/fd5_format.c`, 438 lines, including BC1–7, ETC2 and ASTC).
- An a5xx backend for computerator (`src/freedreno/computerator/a5xx.cc`, 540 lines), a ready tool for poking packets and state on real hardware.
- The cffdump/decode tools.

### 2.2 Turnip and prior Vulkan attempts

**Turnip has never supported a5xx (verified)**
- `src/freedreno/vulkan/tu_device.cc:1744-1766` accepts only gen 6/7/8 and otherwise returns "device %s is unsupported".
- `docs/drivers/freedreno.rst:19`: "There are no plans to port to a5xx or earlier GPUs."
- Two reviewers checked the Mesa 19.1 tarball: the first turnip accepted only gpu_id 630. The 2019 press stories about "Adreno 500 and 600" support were wrong. A third reviewer could not fetch the tarball.

**No open a5xx Vulkan project found**
- GitHub repo-name searches found nothing for "a5xx vulkan", "adreno 530 vulkan", "turnip a5xx" and similar. Community Turnip builds list only 6xx/7xx/8xx.
- Not checked: GitLab forks and MRs and the #freedreno IRC logs, which sit behind a bot wall. Absence is plausible but not proven.

**Maintainer position, mesa-dev, November 2025 (verified)**
- Connor Abbott ([msg225076](https://www.mail-archive.com/mesa-dev@lists.freedesktop.org/msg225076.html)):
  - a6xx added bindless;
  - turnip's input-attachment strategy "falls apart" without it;
  - draw-state differences make command emission much more awkward;
  - it is not impossible but needs an expert, and "no one currently working on turnip is particularly keen";
  - he also wrote that descriptor_indexing "and Vulkan 1.2 are impossible on a5xx". The 1.2 part goes further than the spec; see 2.4.
- Rob Clark ([msg225079](https://www.mail-archive.com/mesa-dev@lists.freedesktop.org/msg225079.html)):
  - the SET_DRAW_STATE differences need a different approach to command-stream building for hardware binning;
  - a5xx Vulkan would make more sense as a different driver, perhaps with shared helpers.

### 2.3 Kernel side, v6.19.5 drm/msm (verified)

**Ready, no work needed**
- msm 1.13 (turnip needs 1.6 or later).
- Render node, syncobj and timeline syncobj.
- The GPU_ID, CHIP_ID, GMEM_SIZE (1 MiB), GMEM_BASE, FAULTS and TIMESTAMP params.
- 4 rings with preemption, since the linux-firmware PFP passes the WHERE_AM_I check (`drivers/gpu/drm/msm/adreno/a5xx_gpu.c:563-580, 1751-1756`).

**Missing: per-process pagetables**
- a5xx has `.create_vm` but no `.create_private_vm` (`drivers/gpu/drm/msm/adreno/a5xx_gpu.c:1804`).
- Consequences:
  - one GPU address space shared by all processes;
  - VA_START/VA_SIZE and SET_IOVA are refused;
  - no sparse binding and no capture-replay.
- Turnip's msm backend has a fallback for this (kernel-assigned IOVAs via GET_IOVA, `src/freedreno/vulkan/tu_knl_drm_msm.cc:548-596`). Whether that path still works is not tested; no current turnip target exercises it (inference).

**Bug: EN_VM_BIND is accepted on a5xx**
- `drivers/gpu/drm/msm/adreno/adreno_gpu.c:498-510` only rejects it when `ctx->vm == gpu->vm`, but `ctx->vm` is still NULL on a fresh fd.
- A later VM_BIND then runs on a scheduler that was never initialised. This looks like a NULL dereference reachable from any render-node user (inferred from source, not tested).
- Upstream fixed it in commit 1b8029394fb7, "drm/msm: Fix per-process-pgtables check". The commit has no stable tag, so 6.19.5 still has the bug.
- For a turnip-derived driver, a userspace crash would probably come first: has_vm_bind ends up true while has_set_iova is false, so `dev->vma` is never initialised (`src/freedreno/vulkan/tu_device.cc:2944-2948` versus `src/freedreno/vulkan/tu_knl_drm_msm.cc:552-553`). This is inferred.

### 2.4 What Qualcomm's driver shows the hardware can do (verified on vulkan.gpuinfo.org)

Reports used:
- [Report 14491](https://vulkan.gpuinfo.org/displayreport.php?id=14491): this exact device (SM-T820, Android 9), driver 512.331.0, Vulkan **1.1.66**, deviceID 0x5030002. On Android 7/8 the same tablet reported 1.0.31 to 1.0.61.
- [Report 37197](https://vulkan.gpuinfo.org/displayreport.php?id=37197): a LeEco LEX820 on Android 13, driver 512.454.0, Vulkan 1.1.128.

| Item | Qualcomm A530 value |
|---|---|
| maxBoundDescriptorSets | 4 |
| Per stage: samplers / sampled images / UBOs / SSBOs / storage images / input attachments | 16 / 128 / 14 / 24 / 4 / 8 |
| maxPerStageResources | 158 |
| maxPushConstantsSize | 128 bytes |
| Compute: shared memory / max invocations | 32 KiB / 512 |
| Subgroups | size 32, BASIC + VOTE, compute only |
| True | multiview (6 views), variablePointers, samplerYcbcrConversion, shaderInt16, vertexPipelineStoresAndAtomics, shaderClipDistance, all four `*ArrayDynamicIndexing` |
| False / absent | descriptor_indexing, geometryShader, tessellationShader, textureCompressionBC, shaderInt64, drawIndirectFirstInstance, shaderStorageImageExtendedFormats |

How to interpret this:
- **The hardware can do Vulkan 1.1.** Whether Qualcomm's driver was Khronos-conformant is unknown.
- **Driver choices, not hardware limits:**
  - GS and tessellation: the hardware has HS/DS/GS register blocks (`src/freedreno/registers/adreno/a5xx.xml:2484-2486, 2587-2604`).
  - BC textures: BC1–7 are in the a5xx format table (`fd5_format.c:275-292`).
  - int64 can be emulated.
- **Probably a real limit:** drawIndirectFirstInstance = false matches freedreno's note that a5xx firmware "just can't ... do draw_indirect with base_instance" (`src/gallium/drivers/freedreno/freedreno_screen.c:510-516`).
- **Why it matters:** the per-stage limits are a realistic target, and well above gallium's generic cap of 16 textures (`freedreno_screen.c:286-287`).
- An older Vulkan 1.0.38 report from an Adreno 540 ([report 2037](https://vulkan.gpuinfo.org/displayreport.php?id=2037)) shows lower numbers: 4 SSBOs, no VS stores, no int16. The later 1.1 driver is the better reference.

What the Vulkan spec requires (verified in `src/vulkan/registry/vk.xml`):
- Core 1.2 and 1.3 do **not** require descriptorIndexing (`vk.xml:20934-20936`). A bindful driver can therefore reach 1.3 in principle; v3dv does with no bindless hardware (`src/broadcom/vulkan/v3dv_device.c:87`).
- 1.4 adds requirements a5xx struggles with: drawIndirectFirstInstance, 8/16-bit storage, all dynamic indexing, 7 sets and 256-byte push constants.
- DXVK master requires Vulkan 1.3 plus descriptorIndexing, update-after-bind, runtimeDescriptorArray and BC (DXVK `src/dxvk/dxvk_device_info.cpp:890-916`). **Even a complete a5xx driver would not run current DXVK/Winlator.**

---

## 3. The hard problems, ranked

### 1. Descriptor binding without bindless (blocker for reuse; needs a redesign)

**Why it is hard (verified)**
- Turnip is fully bindless:
  - it writes set addresses into SP_GFX/CS_BINDLESS_BASE (`src/freedreno/vulkan/tu_cmd_buffer.cc:4782-4805`);
  - it lowers every resource to `bindless_resource_ir3` (`src/freedreno/vulkan/tu_shader.cc:428-606`);
  - it sizes every slot as a 64-byte FDL6 descriptor (`src/freedreno/vulkan/tu_descriptor_set.cc:45-75`).
- a5xx.xml has no BINDLESS_BASE, and the docs say bindless starts at a6xx (`docs/drivers/freedreno.rst:225-230`).
- a5xx binds state per stage instead:
  - textures (12 dwords) and samplers (4 dwords) go into SB4_xS_TEX via CP_LOAD_STATE4;
  - one SB4_SSBO UAV table is shared by all graphics stages, and SB4_CS_SSBO serves compute;
  - UBO pointers live in the const file (`src/gallium/drivers/freedreno/a5xx/fd5_emit.c:315-445`, `src/gallium/drivers/freedreno/a5xx/fd5_image.c:16-24`, `src/freedreno/ir3/ir3_nir_analyze_ubo_ranges.c:634-639`).

**Way around**
- A NIR pass maps (set, binding, element) to flat per-stage TEX/SAMP slots and UBO-pointer const slots at pipeline creation. panvk-Bifrost's per-shader "copy tables" are the closest structural match (`src/panfrost/vulkan/panvk_vX_nir_lower_descriptors.c:1213-1247`). v3dv is a looser precedent: it writes state *addresses* into per-draw uniform streams (`src/broadcom/vulkan/v3dv_uniforms.c:139-210`).
- At draw and dispatch time, emit CP_LOAD_STATE4 SS4_DIRECT from CPU-visible set memory, as fd5 does. This is the starting point.
- Later, measure two cheaper options, both **unknown**:
  - SS4_INDIRECT, so the CP loads descriptors straight from set memory. It is verified only for constants and shader code (`fd5_emit.c:71-76`, `src/gallium/drivers/freedreno/a5xx/fd5_program.c:39`).
  - The unused 64-bit TPL1_{VS..CS}_TEX_CONST/TEX_SAMP base registers (`a5xx.xml:2617-2645`). The comment there reads "not sure if ... the CP_LOAD_STATE mechanism is deprecated". The TP counters PERF_TP_BINDLESS_STATE_CACHE_REQUESTS/MISSES (`a5xx.xml:537-538`) hint at memory-resident descriptor fetch.
- Lower SSBOs to 64-bit bounded global access through ldg/stg. This avoids the shared 24-entry SB4_SSBO table and gives dynamic indexing. The blob's 24 SSBOs per stage with VS stores suggests it did something similar (inference). Storage images stay in the UAV table, at 4 per graphics stage, which matches the blob.
- Input attachments: in sysmem mode they are ordinary textures. In GMEM mode, split the pass, or store and reload at the dependency, unless GMEM sampling turns out to exist (unknown).

**Consequences**
- No descriptorIndexing.
- Update-after-bind is likely unavailable, though not proven impossible if SS4_INDIRECT works.
- Limits close to the blob's: 16 samplers (4-bit immediate field, `src/freedreno/isa/ir3-cat5.xml:84-88`) and 128 sampled images. The s2en path means the real state-block size is unknown.

**Size**
- About 1.5–2k lines of driver code plus a 1–2k-line NIR pass (estimate).

### 2. Command-stream model: no mode-agnostic replay (blocker-level architecture mismatch)

**Why it is hard (verified)**
- Turnip records draws once and replays them for binning, GMEM and sysmem. That relies on:
  - the CP_SET_DRAW_STATE BINNING/GMEM/SYSMEM bits;
  - CP_SET_MARKER;
  - CP_SET_MODE;
  - the RENDER_MODE compare in CP_COND_REG_EXEC (24 sites).
- All of these are A6XX- only (`src/freedreno/registers/adreno/adreno_pm4.xml:483, 533, 1104-1107, 2005`).
- Even turnip's sysmem path emits CP_SET_MARKER (`tu_cmd_buffer.cc:321-354, 3376`).
- Gallium a5xx records each draw twice, once for the draw ring and once for the binning ring (`src/gallium/drivers/freedreno/a5xx/fd5_draw.c:110-120`). Its only use of CP_SET_DRAW_STATE is DISABLE_ALL_GROUPS (`fd5_emit.c:1047-1053`), so draw-state groups are **untested on a5xx in Mesa**.

**Way around**
- Start with sysmem-only direct rendering, using a5xx CP_SET_RENDER_MODE (`adreno_pm4.xml:511`) instead of CP_SET_MARKER.
- Next, GMEM without hardware binning: replay the draw IB per tile and emit mode-dependent state outside it.
- Last, hardware binning with a separate binning stream, as gallium does.
- If draw-state groups misbehave on the a530 firmware, re-emit dirty state inline, as fd5 does.
- Predicate emulation has options that are **untested on a5xx firmware**:
  - CP_COND_REG_EXEC with PRED_TEST/REG_COMPARE is ungated in the XML;
  - memory-based CP_COND_EXEC (`adreno_pm4.xml:308`);
  - CP_REG_TEST's a5xx payload layout is unknown (`adreno_pm4.xml:1951`).

### 3. Volume of a5xx state emission (major in volume, mostly mechanical)

**Why it is large (verified)**
- About 300–400 CHIP-templated functions, roughly 17.6k of turnip's 59k .cc lines, all against a6xx registers. They cover:
  - VPC/link map, shader config, vertex input, raster, blend, depth/stencil, viewport (`src/freedreno/vulkan/tu_pipeline.cc:367-3802`, `src/freedreno/vulkan/tu_shader.cc:2153-3013`);
  - GMEM, binning and VSC.
- Instantiating `CHIP=A5XX` is not a shortcut:
  - registers that have variants hit `assert("invalid variant")` and emit register 0;
  - roughly 449 a6xx registers without variants would silently write a6xx offsets (`src/freedreno/registers/gen_header.py:278-296, 705-719, 929-953`).

**Way around**
- Write a separate backend against a5xx.xml, using `fd5_program.c` (787 lines) and `fd5_emit.c` as working references.
- The packing layer (`fd_reg_pair`, CP_CONTEXT_REG_BUNCH) carries over.
- This is where AI assistance helps most. It still needs hardware tests, because each mistake tends to be a GPU hang rather than a compile error.

### 4. Clears, blits, resolves and GMEM load/store (major)

**What is limited**
- fd5's 2D blitter does only same-size NEAREST copies up to 32 bpp, with no MSAA and no compressed formats (`src/gallium/drivers/freedreno/a5xx/fd5_blitter.c:31-179`).
- BLIT_OP_SCALE and BLIT_OP_FILL exist in the CP_BLIT enum (`adreno_pm4.xml:1750-1752`) but are not reverse-engineered.
- Event-blit limits come from gallium's implementation, not proven hardware limits:
  - MSAA resolve is hard-disabled, although RB_CLEAR_CNTL has an MSAA_RESOLVE bit;
  - Z32 fast clears are rejected;
  - depth/stencil restore uses a BLIT_MRT0 hack (`src/gallium/drivers/freedreno/a5xx/fd5_gmem.c:~503, 650-651`).

**Way around**
- Use turnip's shader-based r3d path for nearly everything. Its texture bind (`src/freedreno/vulkan/tu_clear_blit.cc:1532-1548`, CP_LOAD_STATE6 into SB6_FS_TEX) maps directly onto CP_LOAD_STATE4 into SB4_FS_TEX.
- Keep CP_BLIT for plain copies, and do MSAA resolve in a shader at first.

### 5. Constants and push constants (medium; the dossier first rated this major, the reviewer downgraded it)

- ir3 asserts if `push_ubo_with_preamble` is set without preambles (`src/freedreno/ir3/ir3_compiler.c:387-388`), so set it to false.
- Turnip still has a per-stage push-constant path (`src/freedreno/vulkan/tu_pipeline.cc:321-339`; `tu_cmd_buffer.cc:7770+` emits ST6_CONSTANTS DIRECT). It maps one-to-one onto CP_LOAD_STATE4 ST4_CONSTANTS.
- For gen<6, ir3 still computes the promoted UBO ranges (`ir3_nir_analyze_ubo_ranges.c:658-680`).
- Respect the 448-vec4 window.

### 6. Image layout, views and formats (medium)

- `fdl6_view` pre-fills a6xx registers (`src/freedreno/fdl/freedreno_layout.h:394-434`).
- Work needed:
  - write an `fdl5_view`;
  - move `fd5_format.c` into common code;
  - extend `fdl5_layout`.
- TEX_CONST dwords 6–11 are undocumented (`a5xx.xml:2957-2962`). Gallium works without them, so they should not block anything.

### 7. Kernel and memory-model gaps (minor to medium; the fixes are known)

- Driver side:
  - never call EN_VM_BIND on gen 5 unless VA_START succeeds;
  - use the GET_IOVA path and treat it as untested.
- Kernel side: cherry-pick 1b8029394fb7.
- Fault reporting:
  - Use the per-queue `MSM_SUBMITQUEUE_PARAM_FAULTS`, as GL does (`src/freedreno/drm/msm/msm_pipe.c:65-68`).
  - Turnip instead checks the global counter (`src/freedreno/vulkan/tu_knl_drm_msm.cc:134-145`). On a shared VM, that would turn any GPU hang anywhere into DEVICE_LOST for every Vulkan app.
- No process isolation. GL on a5xx already has the same limitation.

### 8. Shader and feature gaps (minor for 1.0/1.1)

- **robustBufferAccess** (required in 1.0): UBO loads are plain ldg with no bounds, so use `nir_lower_robust_access` or bounded-global addressing. Whether ldgb/stgb/ldib/stib bounds-check in hardware is unknown.
- **Subgroups**:
  - Size 1 is legal, but ir3 uses the real wave size in compute on a5xx (`src/freedreno/ir3/ir3_shader.c:1396-1406`), so it must be forced there too.
  - Alternatively, wave32 with vote: the blob proves vote works, but getone/movmsk/bany/ball are untested in Mesa on a5xx.
- **Multiview** (required in 1.1): replay draws per view.
- **Clip/cull distances**: optional, but need new ir3 work.
- **GS/tess**: optional and not implemented. The blob does not expose them either.
- **Indirect draws**: no CP_DRAW_INDIRECT_MULTI (`adreno_pm4.xml:421-423`), so loop single draws. No firstInstance (firmware limit) and no drawIndirectCount.
- **LRZ and UBWC**: disable both. Turnip already has `nolrz`/`noubwc` debug options.

---

## 4. Options compared

| Option | Work | Risk | Upstream chances | What you get |
|---|---|---|---|---|
| **(a) Extend upstream turnip with CHIP=A5XX** | All ~300–400 templated functions need a5xx bodies, and the shared core (descriptors, draw-stream replay, constants) has to change without regressing a6xx–a8xx. More work than (b). | Very high | Very low: "no plans" (`docs/drivers/freedreno.rst:19`), and both maintainers prefer a separate driver | The same as (b), if it ever landed |
| **(b1) New a5xx driver forked from turnip's neutral parts ("tu5")** | Keep about 25–30k lines: kernel backends, device/queue/WSI/sync, `tu_pass`/`tu_tile_config`, NIR lowering, pipeline cache. Delete about 6–8k (a7xx/a8xx, RT, FDM, LRZ, UBWC, autotune). Write about 15–20k a5xx-specific lines. (Estimate.) | Medium-high: the hardware unknowns in section 7, plus drift from upstream turnip | Uncertain. A separate driver "with some shared helpers" is the shape Rob Clark suggested. It needs a maintainer. a530 hardware already exists in Mesa CI (db820c), which helps. A wholesale fork may be received less well than factored-out helpers (inference). | Vulkan 1.0, then 1.1 (Qualcomm parity); 1.2/1.3 plausible later. No descriptorIndexing, no DXVK. |
| **(b2) New driver from scratch on Mesa's vk runtime, modelled on panvk-Bifrost/v3dv** | About 20–40k new lines (v3dv is 40.9k, all of panvk is 41.2k). Similar total effort, less reuse. | Similar to (b1) | Similar to (b1); maybe a cleaner fit as an a5xx-native design | Same as (b1) |
| **(c1) lavapipe** | None: Alpine/pmOS package `mesa-vulkan-swrast` | Low | Already upstream | Vulkan 1.4 feature set on the CPU. Speed on MSM8996 is unmeasured (inference: fine for bring-up and harness testing, slow for real use). |
| **(c2) Qualcomm driver via libhybris** | Needs the downstream 3.18 KGSL kernel and Android vendor userspace | n/a | n/a | Hardware Vulkan 1.1, but it cannot coexist with the mainline port |
| **(c3) Venus, or Vulkan-on-GL layers (Ashes, gfx-portability)** | n/a | n/a | n/a | Not viable. Venus needs a host Vulkan driver. Ashes needs desktop GL 3.3+ and a5xx exposes 3.1. gfx-portability is in maintenance mode. Zink goes the other way (GL on Vulkan). |

**Recommendation:** use (b1) as the base. Fork turnip's hardware-neutral code, write a5xx-native emission with fd5 as the reference, and take the descriptor ideas from panvk-Bifrost. Install (c1) now as a stopgap and test harness.

For context on the payoff: freedreno GL ES 3.1 already covers compositors, browsers and most apps. Native a5xx Vulkan would mainly serve Vulkan-only native apps and the project for its own sake, not DXVK.

---

## 5. Prerequisites, in order

Steps 1–5 are needed for GPU work of any kind, GL or Vulkan.

### 1. Kernel config
Nothing to do. The pmOS config already has DRM_MSM=m, QCOM_SCM, ARM_SMMU_QCOM, QCOM_MDT_LOADER, REGULATOR_QCOM_SPMI, NVMEM_QCOM_QFPROM and INTERCONNECT_QCOM_MSM8996 (verified).

### 2. Device tree, about 30 lines

`arch/arm64/boot/dts/qcom/apq8096-samsung-gts3lwifi.dts` has no `&gpu` today (verified). `msm8996.dtsi:1263-1339` already has the gpu node, the OPP table and the zap-shader child.

Rough sketch; the regulator values are not validated on this hardware:

```dts
&gpu { status = "okay"; };
&gpu_zap_shader { firmware-name = "qcom/apq8096/samsung/gts3lwifi/a530_zap.mdt"; };
&pm8004_spmi_regulators {
	vdd_s2-supply = <&vph_pwr>;          /* parent supply not verified */
	vdd_gfx: s2 {
		regulator-name = "VDD_GFX";
		regulator-min-microvolt = <1015000>;  /* downstream CPR ceiling; inference */
		regulator-max-microvolt = <1015000>;
	};
};
&mmcc { vdd-gfx-supply = <&vdd_gfx>; };
```

Notes:
- **GFX rail.** It is PM8004 S2 at SPMI 0x1700 (downstream DTB `fdt.dts:9900-9915`). For comparison, db820c pins its GFX rail at 980 mV and Sony at 1015 mV.
- **Start below the top frequencies.** Mainline has no MSM8996 GFX CPR, no GFX mem-acc programming and no per-frequency VDD_MX votes. Disable or cap the 510–624 MHz OPPs at first (inference).
- **apq8096-ifc6640** enables the GPU without `vdd-gfx-supply`, but whether its GPU works is unknown. Still set the supply.
- **Do not include `msm8996-v3.0.dtsi`.** The board's msm-id is 0x30001.
- **DRM binding.** Boot the owner's tablet with `msm.separate_gpu_kms=1` (`drivers/gpu/drm/msm/msm_drv.c:57-66`). The GPU then registers as a standalone render-only `msm` device, independent of MDSS, which is still in display bring-up.
  - Without it, the GPU is a component of the MDSS master. With MDSS disabled the GPU never appears. With MDSS enabled, a GPU that fails to bind takes the display down with it.
- These are DTB and kernel changes, not module-only changes, so full images are needed rather than the module tarballs.

### 3. Firmware

- **CP firmware.** Depend on `firmware-qcom-adreno-a530`, which contains exactly `a530_pm4.fw`, `a530_pfp.fw` and `a530v3_gpmu.fw2` (verified). The device package currently has no such dependency.
- **Zap shader.** Pull Samsung's signed `a530_zap.mdt` and `.b0x` files from the tablet's own vendor firmware.
  - Their location is unknown: `/vendor/firmware`, `/system/vendor/firmware` or the NON-HLOS partition.
  - The kernel mdt loader handles split images, so pil-squasher is optional (`drivers/soc/qcom/mdt_loader.c:68-91`).
- **Do not let it fall back** to linux-firmware's `qcom/a530_zap.mdt`. That is the db820c-signed image. Samsung TrustZone will very likely reject it, and then `hw_init` fails (`drivers/gpu/drm/msm/adreno/a5xx_gpu.c:977-1003`; inference).
- **Packaging.** Ship the zap in a private firmware package and keep it out of the public repo.
  - Firmware loads on first open of the DRM device, so the rootfs is enough.
  - Only list the files in `30-gpu-firmware.files` (the scorpio precedent) if something opens the GPU from the initramfs.

### 4. Kernel fix

Before any Vulkan experiment, cherry-pick upstream 1b8029394fb7 ("drm/msm: Fix per-process-pgtables check") into the pmaports patch series. Optionally also take the companion lazy-VM commits (00dfa76bdfc2 and others). This does not affect GL, because freedreno GL never calls EN_VM_BIND.

### 5. Gate: freedreno GL working headless

Checks, in order:
- dmesg shows the a530 probing, the zap loading and 4 rings.
- `/dev/dri/renderD128` exists.
- `eglinfo` on the surfaceless platform reports `FD530` and GLES 3.1.
- deqp-runner with Mesa's own `src/freedreno/ci/deqp-freedreno-a530.toml`, diffed against `freedreno-a530-fails.txt` and the flakes list.

This validates the ir3/fd5 base that every Vulkan stage depends on.

### 6. Headless test loop on the owner's tablet

The owner's tablet has a broken display, but it reaches USB ssh. Almost all GPU work needs no display:
- surfaceless EGL;
- offscreen rendering read back as PNG;
- compute;
- vulkaninfo;
- deqp-vk, whose tests mostly need no WSI.

This takes the remote tester off the critical path for everything except real-screen WSI and display work. GPU hangs usually recover through the kernel's recovery path; a hard lockup needs a physical reset, which the owner can do.

### 7. Optional, later: a5xx per-process pagetables

- Port Jordan Crouse's unmerged 2019 a5xx patch: CP_SMMU_TABLE_UPDATE in `a5xx_submit`, SMMU info in the preemption records, `a5xx_create_private_vm`.
- Lift the msm8996 TTBR1 exception in `drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c:289-295`.
- Estimate: a few hundred lines plus several hardware rounds, with SMMU/TrustZone risk.
- Downstream KGSL on this tablet does run per-process pagetables, which suggests the hardware can.
- Not needed for core Vulkan. It unlocks process isolation and capture-replay.

---

## 6. Staged plan

All efforts assume one part-time hobbyist with AI assistance. They are estimates; the dossier's figure for an experienced full-time freedreno developer was about 3–6 months to vkcube in sysmem mode. AI speeds up drafting register emission from fd5 and a5xx.xml, but it cannot shorten the hardware unknowns or the hang debugging.

| Milestone | Content | Testable output | Effort |
|---|---|---|---|
| **M0: GPU + GL headless, plus a lavapipe harness** | Section 5, steps 1–5. Also install lavapipe and build deqp-vk and deqp-runner on the device, so the Vulkan test harness is proven before any driver exists. | `FD530` in eglinfo; dEQP-GLES31 results diffed against the db820c expectations; `vulkaninfo` and a deqp-vk run on lavapipe | 1–4 weeks, mostly the zap and regulator unknowns |
| **M0.5: answer the architecture questions** | computerator a5xx experiments: CP_SET_DRAW_STATE groups (do they work, how many), CP_COND_REG_EXEC PRED_TEST and CP_COND_EXEC, SS4_INDIRECT into SB4_xS_TEX and SB4_CS_SSBO, the TPL1_CS_TEX_CONST/SAMP base registers, ldg.a/stg.a and global atomics, ldgb bounds behaviour, getone/movmsk/bany/ball in compute | A written yes/no table that fixes the descriptor and command-stream design | 2–6 weeks |
| **M1: vkEnumeratePhysicalDevices + compute "hello"** | Driver skeleton on the vk runtime; turnip's msm backend with gen-5 handling (no EN_VM_BIND, GET_IOVA, per-queue faults); WC and cached-non-coherent memory types; queue and syncobj; ir3 gen-5 compute pipeline; HLSQ_CS_NDRANGE plus CP_EXEC_CS (structurally the same as turnip, `src/gallium/drivers/freedreno/a5xx/fd5_compute.c:131-168`); SSBOs via global addressing | `vulkaninfo` lists "Adreno 530"; a compute shader writes invocation IDs into a buffer and they read back correctly; first deqp-vk `api.info` / basic compute cases | 1–3 months |
| **M2: offscreen triangle, sysmem-only** | Render pass and dynamic-rendering bookkeeping from turnip; VS/FS state, vertex input, raster/blend/depth from fd5; CP_SET_RENDER_MODE direct rendering; clears via r3d; copy-to-buffer | A PNG over ssh; `dEQP-VK.api.smoke.triangle`; an offscreen vkcube variant | 2–4 months |
| **M3: deqp-vk subset** | Flattened descriptors (textures, samplers, UBOs, push constants, storage images); r3d blits and copies; occlusion and timestamp queries; looped indirect draws; robustBufferAccess lowering; subgroup size 1 | Tracked deqp-runner pass rates on a curated Vulkan 1.0 list (api, memory, compute, ubo/ssbo, texture basics, copy/blit, query_pool, draw); target a large majority of that list | 4–9 months |
| **M4: WSI** | `VK_EXT_headless_surface` first (testable on the owner's tablet), then Wayland. With `separate_gpu_kms` the render device and display device differ, so present needs a dma-buf import or copy (inference). | Swapchain tests headless; on-screen vkcube needs a working display, so a remote tester again | 1–2 months, plus display-dependent time |
| **M5: GMEM + binning, Vulkan 1.1** | GMEM path without binning, then hardware binning with a separate binning stream; multiview by replay; wave32 vote subgroups; samplerYcbcr; variable pointers; then conformance hunting | Performance comparison against sysmem; deqp-vk 1.1 pass rate | 6–12+ months |

Rough totals:
- first offscreen triangle: about 4–8 months;
- a meaningful Vulkan 1.0 subset: about 1–1.5 years;
- conformance-grade 1.1: more than 2 years.

---

## 7. Open questions and what to measure first

Ordered by when they block progress.

1. **Zap shader.** Where is Samsung's `a530_zap` on this tablet, and does Samsung TrustZone accept it for PAS 13 at mainline's `gpu_mem` (0x90f00000)? That address lies inside the downstream peripheral region, which is promising, but it is untested.
2. **VDD_GFX.** Is PM8004 S2 already on at boot? What voltage and which OPPs are stable without GFX CPR, mem-acc or VDD_MX votes? Measure with dEQP-GLES31 at a capped frequency, then raise it.
3. **GL baseline.** How does the Tab S3 dEQP-GLES31 result compare with the db820c CI expectations?
4. **lavapipe performance** on the Kryo cores (offscreen vkcube or vkmark), as the fallback reference.
5. **CP_SET_DRAW_STATE on a530 firmware.** Do groups work, with deferred execution and skipping of unchanged groups, and what is the maximum group count? Turnip assumes up to 32.
6. **Conditional execution.** Do CP_COND_REG_EXEC (PRED_TEST/REG_COMPARE) and CP_COND_EXEC work on a5xx, and what is CP_REG_TEST's a5xx field layout?
7. **Descriptor fetch.** Does SS4_INDIRECT work for texture, sampler and UAV state? What do the TPL1_xS_TEX_CONST/TEX_SAMP base registers do, and what is the TP "bindless state cache"? This is the single most important reverse-engineering question for the descriptor design and for update-after-bind.
8. **State-block sizes.** How large are the TEX/SAMP blocks really when indexed through s2en: is it 16/128, or larger?
9. **Bounds checking.** Do ldgb/stgb/ldib/stib bounds-check in hardware? This decides how robustBufferAccess and the 1.3 robustImageAccess are implemented.
10. **Global memory.** Do ldg.a/stg.a, global atomics and u8/u16 ldg/stg work on a530? This decides bufferDeviceAddress (1.3) and 8/16-bit storage.
11. **Subgroups and demote.** Do getone/movmsk/bany/ball work, is compute dispatch linear, and does `kill` keep helper invocations alive, which demote requires in 1.3?
12. **GMEM and blits.** Can GMEM be sampled directly for input attachments? How do MSAA resolve via RB_CLEAR_CNTL's MSAA_RESOLVE bit and the BLIT2DSCALE path work?
13. **Kernel IOVA path.** Does turnip's GET_IOVA (no SET_IOVA) path still work in current Mesa?
14. **Blob traces.** Can Qualcomm's Android Vulkan driver be traced with freedreno's rd capture tooling on a device still running Android/KGSL? This is unknown, and it would answer questions 5–8 fastest.
15. **Upstream interest.** Would Mesa take a separate a5xx Vulkan driver, and has anyone discussed it on GitLab or IRC? Those sources were not searchable during this research.
16. **Qualcomm conformance.** Was Qualcomm's A530 Vulkan driver ever Khronos-conformant? Unknown, and low priority.
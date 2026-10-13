# Changes to the vendored Xenia

`xenia/src/xenia` is Xenia's D3D12 GPU, UI/presenter and base code from
[xenia-canary](https://github.com/xenia-canary/xenia-canary) at commit
`1c81b867473806b2845a1946b2d185c9d888a319` (the one `installer/deps.py` pins;
Xenia is BSD-licensed, see `xenia/LICENSE`), edited in place to run inside the
DAO runtime. Changes in upstream files are marked `DAO` where they are
not obvious.

To see a file's changes against that commit:

```bash
git -C tools/xenia-canary show 1c81b867:src/xenia/gpu/texture_cache.cc | diff -u - xenia/src/xenia/gpu/texture_cache.cc
```

(The installer's checkout of xenia-canary is sparse; `git show` reads the file
from its history either way.)

## Added files

These are this project's (MIT, the repository's `LICENSE`), not Xenia's.

- `dao_host.h`: the runtime services Xenia's code calls instead of its kernel and
  CPU modules (interrupts, guest threads, the window).
- `gpu/d3d12/dao_upscale.*`: temporal upscaling (FidelityFX), motion vectors, motion
  blur: `dao_upscale.h` is its interface, `dao_upscale_impl.h` the class its files
  share; `dao_upscale.cc` the frame (draws, resolves, the swap, the composite),
  `dao_upscale_fsr.cc` FidelityFX, `dao_upscale_motion.cc` the motion vectors,
  `dao_upscale_hdr.cc` true HDR, `dao_motion_blur.cc` motion blur.
- `gpu/d3d12/dao_frame_trace.h`, `gpu/d3d12/dao_gpu_timing.h`: diagnostics.
- `gpu/shaders/dao_*`, `ui/shaders/dao_*`: the upscaler's, resolves' and HDR
  output's shaders (compiled by `scripts/build_xenia_shaders.py`).

## Changed upstream files

- `memory.h`: replaced; Xenia's `xe::Memory` cut to the subset the GPU uses,
  backed by DAORecomp's guest memory (`DAORecomp/gpu/xenia_memory.cpp`).
- `base/mapped_memory_win.cc`,
  `base/platform_win.h`.
- GPU: `gpu/command_processor.*`, `gpu/pm4_command_processor_implement.h`,
  `gpu/graphics_system.*`, `gpu/gpu_flags.cc`, `gpu/draw_util.*`, `gpu/primitive_processor.cc`,
  `gpu/shader_storage.h`, `gpu/shared_memory.*`, `gpu/texture_cache.*`.
- D3D12: `gpu/d3d12/d3d12_command_processor.*`, `d3d12_graphics_system.*`,
  `d3d12_render_target_cache.*`, `d3d12_texture_cache.*`,
  `deferred_command_list.*`, `pipeline_cache.*`.
- UI: `ui/presenter.*`, `ui/d3d12/d3d12_presenter.*` (HDR output and its quality High, scRGB throughout; vsync),
  `ui/d3d12/d3d12_provider.*`, `ui/imgui_drawer.*`, `ui/window_win.*`,
  `ui/windowed_app_context_win.h`.

The first port was made by a patch script; the tree has been edited directly
since, so this file, not a script, is the record.

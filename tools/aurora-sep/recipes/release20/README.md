# Release 20 package recipes

**Kernel** (`kernel/`): the release 19 recipe and config, built from `c46d7dcdb8712ea9aeb0ce47b439ffae76371e55` (#136). The recipe config file is unchanged. The package build runs `olddefconfig`, which changes 16 symbols against release 19: user4k is compiled in (`ARM64_USER4K_EXPERIMENTAL=y` and the symbols it selects, including `PAGE_EXTENSION`) but stays off unless the kernel is booted with `user4k=on`; the five separate Neo graphics symbols are gone because the Neo uses the shared drivers; `DRM_GEM_DMA_HELPER` becomes a module; two new options stay off. The T6030 performance marker names this source.

**Native OpenGL for the M3 Pro** (`native-pro/`): source `d616d0650d6fa80f72a750a0121442ab917ed6cf` and its source archive are unchanged from release 19. Package release 10 changes only `mesa-m3-pro-check`, which now admits the release 16, 18, 19 and 20 kernels by their exact markers. It then requires the exact T6030 runtime identity, as before.

This remains experimental native OpenGL support, without a full conformance claim.

**Vulkan for the M3 Pro** (`pro-vulkan/`): the separate Honeykrisp package keeps the release 16 binaries and compression settings.

**Other Macs:** M1/M2 and the Air HAL200 profiles keep their existing selection. The Neo keeps its selection rules, but its Mesa doesn't match this kernel's GPU interface, so a Neo session uses software rendering until a Neo Mesa built for this kernel ships.

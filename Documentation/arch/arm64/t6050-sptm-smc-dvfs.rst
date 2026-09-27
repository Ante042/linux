T6050 SMC telemetry and CPU frequency qualification
===================================================

The resident SPTM profile maps the SMC SRAM aperture directly and forwards
its mailbox accesses. The T6050 MFD match currently exposes telemetry only.
The measured 143 float temperature channels use literal SMC key names;
physical sensor names and active cooling policies have not been inferred.

CPU frequency control uses directly mapped cluster command pages, with no
per-transition SPTM MMIO trap. Native macOS 27 firmware code establishes
64-bit commands, five-bit state selectors, SET bit 25 and BUSY bit 31.
The ADT performance-state index has a two-state hardware offset. The tested
M0/M1 ranges are 1.344--4.380 GHz and the P range is 1.308--4.608 GHz.
The driver's frequency readback reports the requested state; it is not an
independent clock measurement.

J714s hardware qualification, 2026-09-27:

* All 18 CPUs online and 143 temperature inputs with matching key labels.
* Each cluster executed 400 million dependent additions at min/max/min,
  pinned respectively to CPU 0, 6 and 12, with correct checksums.
* M0: 298.083 / 91.426 / 297.860 ms (3.2604x work-rate change).
* M1: 297.943 / 91.385 / 297.848 ms (3.2603x work-rate change).
* P: 306.743 / 87.076 / 306.716 ms (3.5227x work-rate change).
* Ratios track the requested frequency ratios within 0.044%; restored-low
  timing differs by less than 0.08%.

This verifies real work-rate scaling, not just register readback. It does
not qualify every intermediate OPP, concurrent governor transitions, or
sustained thermal control. The short diagnostic left userspace/min active;
normal runs must select their intended governor after testing. Baseline
reported temperatures were 25.375--42.659 degrees C, sampled before load.

# Deep sleep on t6000 (M1 Pro): reference branch

This branch adds suspend-to-RAM ("deep" in `/sys/power/mem_sleep`) for the M1 Pro, t6000, as a
driver of its own (`drivers/soc/apple/s2r.c`) plus changes in the drivers that lose state in the
power cut. It works on my 14" MacBook Pro (J314s): several hundred sleeps, all wake sources,
three displays, dock, sound, and overnight power in the same range as macOS. I wrote it before
`sleep.c` and the shared sleep infrastructure landed, so it now overlaps with that and no longer
applies to the current tree, and I'm not going to submit it as it is. It's here as a reference
for a t600x port onto upstream's infrastructure, with everything I learned about the hardware.

## What's in the two branches

Kernel, `t600x-s2r-reference`: upstream `integration/aurora` at 990e00b72b5 (11 Nov 2026, just
before `sleep.c` was merged) plus three commits and this file:

1. `usb: typec: take the altmode module reference when the mode is already active`. Independent
   fix, one file. A DP alt mode entered before `typec_displayport` is bound takes no module
   reference, but leaving the mode drops one, so the count goes to -1 and every later replug
   warns. A physical replug can hit it on any machine, so it can go upstream on its own.
2. `soc: apple: add suspend-to-RAM for t6000 through m1n1`, the main commit (85 files,
   +8674 −136). Its message describes what it does per subsystem.
3. `fix: let deep sleep and the pmu rtc driver share the alarm vector` (2 files, +65 −7). After my
   rebase the tree had gained `rtc-apple-pmu`, which uses the same NUB SPMI alarm vector and
   refused every suspend target but s2idle. This lets deep sleep through and makes s2r.c save
   the SPMI vector enables before the sleep and write them back after it.

m1n1, `t600x-s2r-reference` on my fork (https://github.com/Ante042/m1n1/tree/t600x-s2r-reference):
omacom/m1n1 at bc79acc4, the m1n1 my distribution packaged at the time, plus four commits,
+347 lines: the warm resume mailbox in `/chosen/asahi,s2r-mailbox`; parking the boot CPU on a
quiet reset without a resume entry (m1n1 sets the mailbox's `quiet` flag as its last step before
the kernel runs, and resets after that stay silent); restarting an armed watchdog on the wake
path; and setting the internal PCIe controller up again on the wake path. None of it is upstream.

The build I tested also carried three local changes that are not in the branch, so nobody is
surprised by them: the DRM driver named "asahi" instead of "aurora" so my distribution's Mesa
finds the GPU; J314s fallbacks for the speaker amplifier settings, which this m1n1 does not pass
in the device tree; and two small display fixes that belong in their own PRs (a dock port with
no sink taking a display pipeline, and 120 Hz).

## Building and booting

- `CONFIG_APPLE_S2R=y` (bool, default n; needs `HOTPLUG_CPU`, `SUSPEND`, `ARM_ARCH_TIMER`).
- Boot through the m1n1 from the m1n1 branch. I ran it as the second stage with the device trees
  from this kernel branch appended; with them, `rtc-apple-pmu` shows up as `rtc1`.
- Stage 1 on my machine is not the stock one, and I should have said so from the start. While
  bisecting on 1 Nov I installed a thin stage 1 (m1n1 built with `-DTHIN_STAGE1`: MMU, primary
  watchdog off, PMGR tables, then chainload) and never went back, so every sleep in these notes
  ran with it. It is the branch `t600x-s2r-thin-stage1` on my m1n1 fork: the first three commits
  of the reference branch plus two. The wake path itself runs in stage 2 only, because the
  chainloaded stage 2 lands at the same base and RVBAR then points into it. But I never retested
  with a full stage 1, so I can't say whether its extra init matters for the sleep.
- At boot the driver looks for the mailbox and checks for `apple,t6000`. On anything else, or
  without the mailbox, it does nothing at all (no CPU hotplug, no deep sleep). With it you get
  `apple-s2r: m1n1 mailbox at ..., N CPUs, CPU hotplug through m1n1`.
- Deep sleep stays off until `1` is written to `/sys/module/apple_s2r/parameters/enable` (one
  way; `-ENODEV` without the mailbox). That calls `suspend_set_ops()`, which makes deep the
  `mem_sleep` default unless `mem_sleep_default=` is on the command line; s2idle stays
  selectable. I did it from a small boot service: load `apple_sep` (with `xart_writes=0
  provision_keybag=0`), write 1 to `enable`, write `deep` to `/sys/power/mem_sleep`.
  `apple_s2r.enable=1` on the kernel command line won't work as the code stands: the parameter
  is parsed before the early initcall maps the mailbox, so the setter returns `-ENODEV`. That
  is from reading the code; I never tried it.
- Optional: `tps6598x_core.s2r_source_off=Y` switches USB-C ports that power a device off for
  the sleep (see known issues for why it's off by default).

## How a sleep goes on t6000

The sequence follows macOS 13 on t6000 (XNU, AppleSMC, AppleT6000PMGR, some of it from
hypervisor traces of macOS 13.7.8). In kernel order:

1. `PM_SUSPEND_PREPARE` notifiers: the SEP gets SLEEP, the display driver releases Type-C DP
   links whose CRTC is off, dwc3-apple marks its roles for teardown (done in its prepare callback).
2. `apple_s2r_begin()`: saves the power words it will touch, the WD1 setup user space asked for,
   registers for SMC events, sends `slpw`, and writes 4 to the PMP telemetry sleep word, as macOS
   does right after `slpw`.
3. Device suspend: tipd to S3 (or S5), NVMe's ANS to IDLE, DCP link to sleep, and so on.
4. `apple_s2r_prepare()`: holds wakeup events from here (`pm_wakeup_hold()`), parks the GPU
   firmware (AP idle, IOP idle, detach, clear RUN) and stops the AOP AFK queues (0xa4 on
   endpoints 0x20–0x2b) before its AP power 0x201. The GPU park has to happen here: its power
   domain is off once the noirq phase is through, and moving it later hung the machine hard.
5. `suspend_late`: the display driver and apple-sio park their coprocessors.
6. `apple_s2r_prepare_late()`, after noirq, the last point the SMC answers: waits up to 5 s for
   the SEP power state to reach 0 (refuses otherwise), takes the display blocks down through the
   PMP device requests (`apple_s2r_disp_down()`, macOS order disp0, dispext0, dispext1, plus the
   GPU fabric port), drives SMC GPIO `gP1a` (SD reader power) to 0, writes `pmVC` and `WKTP`,
   sends `slpa`, sends the SMC firmware AP power 0x201, then drops the PMP's device requests,
   idles its DVFS words and stops it (`apple_s2r_pmp_set()`).
7. Secondaries go offline and park in `apple_s2r_cpu_die()` until the boot CPU commits. They
   can't just go down: the global deep sleep bit hangs a core that idles in WFI before then.
8. `apple_s2r_enter()`: refuses if a CPU is in an unknown state, writes the PMGR wake mask,
   clears the PMGR "wait for SEP power gate" bit if the SEP has no driver, then `cpu_suspend()`.
9. `apple_s2r_finisher()`: gives up if an IRQ is pending, writes the resume entry and the PCIe
   restore request into the mailbox, quiesces (`apple_s2r_quiesce()`, in the order of
   AppleT6000PMGR::quiesceHW: memory controller drain, error handler and counter zeros, the
   APCIe handshake, ACC, DCS/AFR, cluster targets, clock muxes, PLL), writes the PMGR stop of
   each parked core, arms WD1, writes the raw power states from `ps_late[]` so the PMGR looks as
   it does under macOS at that point, turns on the RTC alarm's SPMI vector, and writes 1 to the
   PMGR S2R timer, which is GO. All cores then write back L1, set the deep sleep overrides and
   WFI without retention (`apple_s2r_cpu_sleep()`, `s2r-sleep.S`).

The facts someone porting would otherwise have to rediscover:

- **SMC:** `MBSE` = `slpw` at begin; `pmVC` = `00 01 00 00` (0x100) and `WKTP` = 2 between
  `slpw` and `slpa`; `slpa` after noirq; then RTKit AP power 0x201 to the SMC. On wake the SMC
  needs AP power ON before any key access (an idle AP gets every key refused), then `gP1a` back
  on, read `MBSE` and `MBSW` (64-bit event bits, names in `apple_s2r_mbsw_bits[]`), write
  `waka`. The open M4 Max port (#1664) writes the same `pmVC` and `WKTP` values; we got there
  independently.
- **Reset vector:** on my J314s the boot core's RVBAR is locked by iBoot to the boot loader (I
  read it back). So every wake starts in m1n1, which is why the mailbox approach works. The
  driver compares each core's RVBAR with m1n1's at boot and warns on a mismatch.
- **Mailbox:** `struct s2r_mailbox` in s2r.c (magic "S2RMBOX1", version 1): `rvbar`,
  `boot_mpidr`, a `resumes` counter, `restore`/`restored` for the PCIe setup, and per-CPU slots
  with the PMGR start registers, a resume `entry` and a `resets` counter. The resume counter is
  how the kernel tells a real power cut from a sleep that never happened.
- **CPU power-down:** CYC_OVRD and ACC_OVRD (`apple_s2r_cyc_ovrd()`, `apple_s2r_acc_ovrd_deep()`).
  The PMGR start bits don't wake a core that powered itself down; an IPI wakes it through a
  reset into m1n1. After a deep round the cluster's registers may not answer, so cores are woken
  blind and the kernel waits for their reset count. Writing a PMGR stop before a plain hotplug
  WFI keeps the core from powering down, but for S2R every parked core needs its stop. AICv2
  can't steer IRQs, so an offlining core sets its SIQ route to none first.
- **SEP:** a PMGR control bit set at boot makes the S2R sequencer wait for the SEP to power-gate
  (`T6000_PMGR_SEP_PG`, bit 2). With the SEP driver bound the enclave gets SLEEP and gates
  itself; without it the bit is cleared for the sleep.
- **Wake sources:** the PMGR mask gets the values macOS 13.7.8 writes at boot; the ones left on
  are named in `t6000_ap_wake_sources[]` (AOP, NUB incl. the SPMI software IRQs, SMC, ATC). In
  practice the SMC wakes for power button, lid, keys and trackpad, charger and USB-C plug
  events, and the PMU RTC alarm (SPMI vector 2, shows as `NUB.SPMISw3IRQ`). USB is off, so a USB
  keyboard can't wake it. In deep sleep the SPI keyboard controller is not a wake source; keys
  and trackpad come through the SMC.
- **Dark wake:** `apple_s2r_suspend_again()` sleeps again when `MBSW` only has charger/plug or
  lid-closed bits; power arriving with the lid closed is a full wake, so docking works.
- **Point of no return:** `slpa`. A sleep given up between `prepare()` and `slpa` survives (GPU
  firmware off until the next sleep, which heals it). After `slpa` the SMC and PMP don't come
  back without the power cut (NVMe, PCIe and the other CPUs die), so the driver restarts the
  machine (`apple_s2r_check_no_return()`).
- **Wake path in m1n1:** if an armed watchdog is running it restarts it, sets APCIe up again when
  `restore` asks and reports in `restored`, bumps `resumes` and jumps the boot CPU to
  `cpu_resume`; secondaries without an entry go back to its spin table and from there to the
  holding pen.
- **Kernel wake order:** WD1 gets 170 s until devices are back, then `.wake` brings back what
  the PMGR lost (`apple_s2r_psall_restore()`, up to 30 passes 10 ms apart, as fabric parents can
  be slow), `.finish` restarts the PMP, AOP (queue start 0xa3), GPU and the SMC side, and the
  normal device resume relinks displays and restarts SIO, tipd and dwc3.

## What was tested

Everything ran on one J314s, with a 2560x1440 display and a 1080p display on USB-C, a dock with
USB3 and LAN, Wi-Fi and NVMe.

On exactly this kernel code, 12 Nov 2026:

- 20 of 20 deep sleeps in one boot, alternating 20 s and 3 s between a wake and the next sleep.
- After a full system update (Hyprland 0.57, new Mesa), 26 more in one boot.
- RTC alarm wake through the raw PMU registers and through `rtcwake -d rtc1`.
- All three screens lit again within about 8 s of a wake (checked with a camera).
- Speakers working after 22 sleeps; keyboard answering after sleep.
- kexec refused while deep sleep is selected: `kexec_file_load` fails with EBUSY and the kernel
  says "Can't kexec: CPUs are stuck in the kernel."
- The tree builds. KUnit and the host tests, run on the rebase before its last two small fixes,
  gave the same results as plain upstream at the base.

On the previous base (9bec84ba269), with the same driver code before the rebase:

- A few hundred deep sleeps across 9–11 Nov, plus several hundred on an earlier 7.2-based
  version of the same driver (one run of 300 in a row).
- Wake by key, trackpad, power button and lid; charger plugged and pulled while asleep (dark wake,
  back to sleep); clamshell (charger plugged with the lid closed gives a full wake, a display
  cable alone stays asleep); HDMI through sleep; headset jack and its mic; speakers by ear;
  internal microphones (10 of 10); DisplayPort audio after 5 sleeps (heard through a mic);
  USB3 hub, LAN and charging through a sleep; a Bluetooth keyboard after a wake.
- Device resume about 1.2–1.5 s (median) with both external displays attached.
- Sleep power over two single nights of 7–10 h (one on this base, one on the 7.2-based version),
  nothing plugged in: about 130–170 mW, against about 150–185 mW for a night of macOS measured
  the same way. The fuel gauge stops counting at these currents, so these come from battery
  voltage calibrated against it. s2idle drew about 1.3 W in the one night I measured.
- Give-up paths with injected failures: a wakeup during entry becomes a full wake; failures
  before `slpa` survive; failures after `slpa` restart the machine by themselves.

Not tested at all: any machine other than this one J314s; the alarm wake with the RTC driver
unbound. Not repeated on the rebased code: everything in the second list, including physical
key, lid and charger wakes (only RTC wakes there).

## Known issues

- Re-suspending 3 s after a wake once crashed the external display coprocessor of one display
  ~30 s after the next wake ("MBI queue overflow", "DCP has crashed"; dark until reboot): 1 in 8
  tries on the previous base, 0 in 20 on this one. Cause unknown.
- The dock sometimes comes back with USB2 only: about 24 of ~750 logged wakes on 8–11 Nov (older
  bases), none in the loops on 12 Nov. The host's SuperSpeed port then stays in RxDetect from the
  start, as if the dock presented no termination. Nothing on the host side brings it back (port
  disable, warm reset, port power, a USB2 reset, another sleep, SSPS S5 and back); only a Type-C
  detach and attach does. Stretching the windows on either side of the sleep didn't clearly move
  the rate (small runs). I wrote a kernel-side replug and dropped it: it needs a vendor
  debug-mode command and misbehaved when a suspend followed quickly.
- Once, the first plug-in of both display cables after a night's sleep left one display dark
  (DPTX and xHCI setup timeouts on both ports; a DPMS off/on fixed it). Seen 1 of 3 times that
  morning, not reproduced.
- Bus-powered USB-C devices keep drawing in sleep: with two USB-C to DP adapter cables plugged
  in the machine drew about 340–375 mW asleep, and about 33 mW (the gauge's floor) with
  `s2r_source_off=Y`. That option removes the load, but every wake then replugs the displays,
  which come back later and less reliably, so it's off by default.
- After a sleep given up between `prepare()` and `slpa`, the GPU firmware is off until the next
  sleep and the desktop session may need a restart; DisplayPort audio is off until the next
  sleep; the display behind a USB-C to DP adapter stayed dark in 4 of 5 such tests (once even
  through the next sleep, until a reboot).
- One unexplained reset during a deep sleep with kprobes armed on the previous base (1 of 7
  traced sleeps, none in about 60 untraced ones since).
- With deep selected, `echo freeze` runs part of the deep preparation in the PM notifiers (SEP
  sleep, AIC mask flag, DCP link release). It's all undone afterwards.
- s2idle is unchanged, including an existing problem where an external display coprocessor dies
  after s2idle; deep sleep refuses to run while a display coprocessor is stuck.
- Addresses are hardcoded for t6000 (taken from the ADT, no DT binding). The M1 Max and Ultra
  are refused on purpose. NVMe's lane tunables should come from the DT.

## How it maps onto upstream

Mostly from reading upstream's tip of 12 Nov (97469189051) and the open M4 PRs; I haven't built
any of it. Against that tip, 36 of the 87 files in the full patch conflict in a 3-way apply.

| Area | This branch | Upstream | For a t600x port |
|---|---|---|---|
| Resume mailbox | m1n1 `asahi,s2r-mailbox`, per-CPU slots, PCIe restore flag | aura `aurora,s2r-mailbox` (AURAS2R1, 32 bytes) | Take upstream's; aura publishes nothing on M1–M3 today (see open questions) |
| CPU hotplug and parking | offline cores power off, back through a reset into m1n1 | rvbar spin table, cores park coherent in the kernel, `spin_table_power_ops` | Take upstream's; the CYC/ACC_OVRD power-down becomes the t600x `cpu_off` |
| SMC slpw/slpa/waka | in s2r.c | in macsmc.c, same order | Take upstream's; add a t6000 hook for `pmVC` and `WKTP` (as #1664 does for T6041) and `gP1a` |
| Dark wake | `.suspend_again()` | PM-core dark-wake flag | Take upstream's; the charger/lid rules become calls |
| Wakeups on the way down | `pm_wakeup_hold()`, restart after `slpa` | irreversible stops only in `.enter` | Likely unnecessary once the stops move into `.enter` |
| Quiesce and GO | quiesceHW order, `ps_late[]`, S2R timer, 7-word wake mask, 9 cluster words | `commit` register, T8140 tables | t600x-specific, keep |
| Power words after wake | `apple_s2r_psall_restore()` | match-data lists, `sleep_ps_rewake()` | Same idea, t6000 lists |
| Displays | DCP park, Type-C link release, PMP requests, crossbar | T8140 DCP sleep path | Unknown whether upstream's path suits t6000 DCP firmware |
| GPU (G13) | park, fabric port down | G17 only | t600x-specific |
| PMP, AOP, SEP, SIO | own hooks | coprocessor registry, enclave stop | Move onto the registry |
| RTKit, mailbox, SART | own helpers | equivalents under other names | Take upstream's |
| DART, pinctrl, ADMAC | own restore | generic on `pm_suspend_via_firmware()` | Take upstream's |
| AIC | AICv2 restore and mask | AIC3 only | t600x-specific |
| tipd | S3/S0, optional S5 | SSPS via `apple,sleep-ssps` | Take upstream's; S5 is extra |
| dwc3, ATC PHY | role teardown, dummy-PHY bit | same teardown, runs whenever deep is selected | Would run twice: take upstream's, keep the dummy-PHY restore |
| PCIe, NVMe | m1n1 redoes APCIe, kernel relinks; ANS IDLE and tunables | T8140 paths, ANS as a coprocessor | t600x-specific |
| RTC alarm | SPMI vector 2 armed before GO | wake source in DT, notify enables | Reconcile with rtc-apple-pmu |
| spi-hid, bcm4377, MCA/NCO/cs42l84 | driver patches | none on the tip (bcm4377 in #1477/#1664) | Keep as driver patches |
| Watchdog | WD1 across the sleep | WD2 | Different watchdog, same purpose |
| kexec | `cpus_are_stuck_in_kernel()` | `spin_table_cpus_stuck()` | Upstream's covers it |

My rough guess is that 1.5–2.5k of the ~8.7k lines become redundant that way, and the t6000 core
of s2r.c (around 2.5k lines) stays. Upstream's generic paths would run on t6000 for the first
time and the CPU parking model changes, so all of it would need testing on hardware again.

## Open questions

- **RVBAR.** aura treats M1 to M3 as unlocked and publishes no mailbox there, so `sleep.c`
  can't probe on t6000 today. On my J314s the boot core's RVBAR is locked by iBoot to the boot
  loader. I don't know whether that holds on other t600x machines or firmware versions.
- What `s3_5_c15_c6_2` (upstream's per-core deep sleep register) is on M1, if anything.
- Whether a t6000 has anything like T8140's two wake paths (DRAM kept vs. the deep path).
- Whether the t6000 ANS depends on the APCIe block the way T8140's does.
- The PMGR wake mask values are macOS's; that a 1 masks is my inference, not verified.
- The NUB SPMI vector enables are assumed to survive the power cut (always-on block). The driver
  writes them back and logs if they changed; I didn't see that in the sleeps I checked.
- Why the dock loses SuperSpeed, and why the display coprocessor crashed after a quick re-suspend.

Thanks to everyone who built the drivers and tools this stands on, and to Dj and the people
behind `sleep.c` and the M4 ports. I hope something in here saves you a few days on t600x.

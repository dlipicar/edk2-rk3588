# ROCK 5B USB-PD validation on PR #281

Status: **hardware-validation candidate; not validated on a physical ROCK 5B**.
This work extends [PR #281](https://github.com/edk2-porting/edk2-rk3588/pull/281),
snapshot `e7de1c2d2b8e07e20c935999d60ba569187c9251` (13 commits above
`ef2d089d0a095aee1677554097fc0094b1b2aad4`). It does not replace that driver.
The PR can change independently; these findings refer to this snapshot.

## Hardware and policy

The ROCK 5B power input is also its USB-C data/DisplayPort connector. The
FUSB302 is on I2C4 at 7-bit address `0x22`, using I2C4 M1: GPIO2_B5 SCL and
GPIO2_B4 SDA. Its active-low IRQ is GPIO3_B4. Firmware polls, so it masks chip
interrupt output and does not require a GPIO interrupt handler. The connector
uses USBDP PHY0; the SBU DC-blocking controls are GPIO4_A6 and GPIO4_A7.

The board description correctly has `SelfPowered = FALSE`, no source PDOs,
and no `SetVbus` callback. Firmware must keep sink Rd terminations and must
not intentionally send Hard_Reset: losing this connector's VBUS loses board
power. This does not prevent a charger from initiating Hard_Reset.

Radxa documents 5–20 V input and recommends its PD30W adapter. The upstream
sink capabilities start with fixed 5 V / 3 A, followed by a variable 5–20 V
range with a 5 A ceiling. The variable sink capability describes voltages
the board accepts; this firmware still selects only **fixed source PDOs**.
The validation branch retains that ordering and voltage range, but reduces
the ROCK 5B range ceiling to 3 A. This is a conservative test policy, not a
claim that the upstream 5 A board description is electrically invalid.

Selection now maximizes usable power after applying the board current limit,
breaking equal-power ties in favor of higher voltage, as upstream U-Boot does
for its power preference. For example, 9 V / 3 A wins over 20 V / 1 A. An
ordinary 5/9/12/15/20 V list whose entries each offer 3 A selects 20 V / 3 A;
an advertised lower current is respected. The first source object must be
fixed 5 V. Zero-current and over-5-A fixed advertisements are not requested.
Battery, variable source PDOs, PPS and EPR remain outside this sink policy.
Requests also set the PD No_USB_Suspend bit: a powered computer cannot honor
USB suspend's low-power draw limit as an input-power obligation.

## Architecture and boot timing

PR #281 already provides these layers:

* `Fusb302Dxe.c`: I2C IO driver binding, chip setup and CC orientation.
* `Fusb302Pd.c`: FIFO framing and synchronous sink negotiation/recovery.
* `Fusb302Source.c` and `Fusb302Vdm.c`: existing source and DP alternate-mode
  support. ROCK 5B's board description prevents sourcing.
* `UsbTypeCPort` protocol: orientation, contract and alternate-mode results.
* `UsbDpPhyDxe`: consumes orientation and programs lane/SBU routing.

The platform manager publishes the board description during DXE. In
`PlatformBootManagerBeforeConsole`, after EndOfDxe/deferred dispatch, firmware
connects I2C controllers recursively. FUSB302 driver start runs synchronously:
detect CC, listen for Source_Capabilities, recover a silent link with
Soft_Reset, select/request a fixed PDO, await Accept and PS_RDY, and briefly
check for repeated advertisements. Only after I2C connection returns does BDS
install `gRockchipPlatformPowerReadyProtocolGuid` and dispatch dependent
peripheral drivers. This is a gate after the negotiation **attempt**, not a
guarantee of sufficient power: failures still allow boot to continue.
Display setup can connect I2C earlier; the binding still finishes synchronously
before gate release. Initial PHY configuration also precedes this gate, which
controls host/peripheral startup rather than every SoC power domain.

USB and PCIe use the readiness dependency. The original SATA callback used
`EfiCreateProtocolNotifyEvent`, which signals immediately on registration,
and mistakenly initialized SATA even when readiness was absent. The fix
checks protocol presence before closing the notification and enabling SATA.
Configured SATA can therefore no longer bypass the gate through that initial
notification. The usual ROCK 5B NVMe boot uses PCIe, but SATA configuration
shares the same initialization path and needed correction.

This is early BDS, not SEC/SPL. Pre-UEFI load and elapsed time until this point
are unchanged. `PlatformEarlyInit` still enables the Wi-Fi rail, and other
on-board loads already exist. The first negotiation timestamp must be checked
against charger attach time on hardware; source review cannot prove a charger
will keep the board powered long enough to reach this code.

## Confirmed findings and changes

These findings are established by source review and host regression tests;
they are not reports of observed ROCK 5B electrical behavior.

| Baseline issue | Hardening |
| --- | --- |
| Immediate SATA notification bypasses PD readiness | Require the readiness protocol before SATA initialization |
| Sink-only port can briefly be probed with source Rp | Skip source probing unless source capabilities and a VBUS switch exist |
| Highest voltage can choose less power, including a zero-current PDO | Validate advertisements and rank usable power within board limits |
| Reject/Wait ends negotiation immediately | Bounded Wait retry and lower-ranked fixed-PDO fallback |
| Blind delay between Soft_Reset retries can miss a waking source's deadline | Poll and service messages during retry intervals |
| Incoming Soft_Reset can leave an obsolete Accept/PS_RDY wait active | Restart from Source_Capabilities with a bounded recovery budget |
| Some reset/read errors can be hidden or leave stale contract metadata | Propagate errors and report an unconfirmed supply honestly |
| Duplicate messages can trigger policy twice | Track received message IDs, resetting them with the link |
| Logs do not expose the complete power-gate boundary | Add CC samples, elapsed/result/contract and readiness logs |

## Known limitations retained for review

1. **The PD policy runs once.** After driver startup/alternate-mode discovery,
   there is no persistent PD service. AUTO_GCRC can acknowledge packets but
   cannot answer a later Get_Sink_Cap, Soft_Reset or Source_Capabilities.
   The RX FIFO can eventually fill. Long UEFI/GRUB waits, charger changes and
   post-boot renegotiation therefore remain unvalidated and potentially unsafe
   for board uptime. A bounded, persistent policy state machine is follow-up
   work; these patches do not pretend to provide one.
   The bounded startup capability loop stops reading after its last completed
   exchange; this cannot prevent hardware from acknowledging later packets
   and does not solve persistent source chatter.
2. **Linux takeover is not enabled by the bundled mainline DT.** Its FUSB302
   node remains `status = "fail"`; this branch deliberately does not change
   that. The driver makes no runtime I2C accesses, so it does not contend with
   a later OS driver, but that alone is not a validated handoff. An OS using a
   different DT may initialize the controller and renegotiate. Test the exact
   Ubuntu kernel and DT combination before enabling the node separately.
3. **A failed negotiation cannot force safe power.** Retaining Rd and avoiding
   Hard_Reset does not prove that VBUS is 5 V or that the source will preserve
   an earlier contract. Zero voltage/current in an unconfirmed protocol result
   means unknown. Firmware has no board current limiter here and continues
   to enable peripherals, so an inadequate 5 V source can still brown out.
4. **Live replug/orientation changes are not a complete hotplug implementation.**
   Both initial cable orientations have mappings in source; their electrical
   correctness and USB operation require testing. Removing the board's only
   supply normally causes a complete power cycle.
5. The PR's broader source/DP-alt-mode support on other boards was not given
   physical validation by this ROCK 5B audit. Successful compilation is not
   USB-PD compliance certification.
6. CC sampling has no full attach debounce or filtering against active BMC
   traffic. The added raw status logs aid diagnosis; analog stability must be
   checked using both orientations and different attach timings. A separate
   source-mode audit is also warranted: the existing source-side VBUS helper
   maps an I2C read failure to absent VBUS. ROCK 5B has no sourcing callback and
   cannot enter that path; it is not repaired by these sink-focused patches.

## Build and host checks

Build the pinned PR first, then this branch, using its pinned submodules:

```sh
git submodule update --init --recursive
./build.sh --device rock-5b --release DEBUG
cp RK3588_NOR_FLASH.img rock-5b_DEBUG.img
./build.sh --device rock-5b --release RELEASE
cp RK3588_NOR_FLASH.img rock-5b_RELEASE.img
```

The supplied build logs and manifest record actual compiler versions, hashes
and results. The build uses the default open TF-A path and default repository
Secure Boot settings; it does not provision custom Secure Boot keys. No test
image was flashed by this work. Use the DEBUG image for first UART validation.

The project CI builds firmware; there is no repository-wide dedicated test or
style command in its workflow. The added host tests exercise production code
with simulated I2C messages/notifications. They establish control-flow and
policy behavior, not analog timing, BMC signal integrity or charger compliance.

Run the retained tests from the repository root:

```sh
edk2-rockchip/Silicon/Rockchip/Drivers/I2c/Fusb302Dxe/Tests/run-host-tests.sh
python3 edk2-rockchip/Silicon/Rockchip/Drivers/I2c/Fusb302Dxe/Tests/test_attach.py --baseline-ref e7de1c2d
python3 edk2-rockchip/Silicon/Rockchip/RK3588/Drivers/RK3588Dxe/Tests/test_power_gate.py
git -c core.whitespace=cr-at-eol diff e7de1c2d..HEAD --check
```

The PD harness compiles production `Fusb302Pd.c` with real EDK2 headers, strict
compiler warnings, AddressSanitizer and UndefinedBehaviorSanitizer. It covers
15 groups, including malformed PDOs, lower-power higher-voltage offers, Reject,
Wait, reset recovery, duplicate IDs, I2C failures, absent PS_RDY and silent/late
sources. The attach harness runs eight cases against the production function;
the SATA harness compiles the actual notification callback. For negative
controls, pass `e7de1c2d` to the PD script or `--revision e7de1c2d` to the SATA
script; failures on that baseline are expected and are retained in the logs.
These mocks do not simulate chip FIFO capacity or real transport timing.

See [the hardware checklist](rock5b-pr281-hardware-test.md) for expected logs
and acceptance criteria. Any upstream submission should remain marked
**ROCK 5B hardware validation pending** until those results exist.

## Primary references and provenance

* [PR #281](https://github.com/edk2-porting/edk2-rk3588/pull/281), including its
  maintainers' comments on power sequencing and reboot loops.
* [Radxa power requirements](https://docs.radxa.com/en/rock5/rock5b/getting-started/power-supply).
* [ROCK 5B v1.3 schematic](https://dl.radxa.com/rock5/5b/docs/hw/radxa_rock5b_v13_sch.pdf),
  power input and Type-C sheets (20 and 29).
* Bundled upstream DTS at `devicetree/mainline/upstream`, revision
  `1826bba6fb1c8c0b6d845f8308590dcfd1f7f859`; particularly
  `rk3588-rock-5b.dts`, `rk3588-rock-5b.dtsi` and the common
  `rk3588-rock-5b-5bp-5t.dtsi`.
* [Upstream U-Boot TCPM policy](https://github.com/u-boot/u-boot/blob/master/drivers/usb/tcpm/tcpm.c)
  and [ROCK 5B enablement rationale](https://lists.u-boot-project.org/pipermail/u-boot/2024-May/554967.html).
* [onsemi FUSB302B/D, revision 5](https://www.onsemi.com/pdf/datasheet/fusb302b-d.pdf),
  register definitions, FIFO handling and automatic GoodCRC limitations.
* [Collabora RK3588 notes](https://gitlab.collabora.com/hardware-enablement/rockchip-3588/notes-for-rockchip-3588),
  checked at `dc22c25cc26f6faf1a4dec95abcae0cb2e2ea0e9`.

The extension retains BSD-2-Clause-Patent licensing. Linux/U-Boot were used as
hardware/protocol and policy references; no GPL implementation was imported.

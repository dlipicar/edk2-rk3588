# ROCK 5B PR #281 physical validation checklist

**Status: not run on physical hardware.** These are expected observations,
not captured results. This image is a validation candidate. See
[the audit](rock5b-pr281-validation.md) for confirmed defects and remaining
limitations, especially the one-shot PD policy and disabled Linux FUSB302 node.

## Setup and flashing

Use the hardened **DEBUG** image first. Record its SHA-256 from the delivered
manifest and verify the downloaded file before flashing. The supplied `.img`
is the project's combined `RK3588_NOR_FLASH.img`, with GPT, loader, TF-A and
UEFI; it is not a Linux or NVMe disk image. Keep a known-working firmware and
Maskrom recovery method available. Use the upstream
[flashing instructions](https://github.com/edk2-porting/edk2-rk3588#getting-started)
and [Radxa SPI instructions](https://docs.radxa.com/en/rock5/lowlevel-development/bootloader_spi_flash)
for the identified SPI target. Do not write this image over the NVMe OS disk.
No flashing was performed as part of this work.

Connect UART2 TX/RX/GND with a suitable 3.3 V serial adapter at **1500000 baud,
8N1**, using the board documentation for pin identification. Do not connect
the adapter's power output. Start capture before applying USB-C power and
save the entire log through Ubuntu boot. UART counters are not measurements
of elapsed time since first VBUS; use a PD analyzer for that relationship.

Record: board revision and RAM size, image SHA-256, UEFI settings, power
supply model and advertised PDOs, cable model/rating, NVMe model, USB devices,
Ubuntu ISO/kernel version, selected ACPI/Device Tree mode and DT source.
Use Device Tree/Mainline for the intended Ubuntu test and record any overlays.
An inline PD analyzer is strongly preferred for reset and current tests;
UART reports protocol intent, not actual VBUS voltage or load current.

## Expected UART sequence

The following are literal message formats emitted by the branch, with variable
fields in angle brackets. Other driver messages may be interleaved. Chip and
orientation messages may precede the I2C connection marker if display setup
already connected the controller. The final contract/attempt must finish
before the power gate is released.

```text
PlatformPower: <counter-ms> ms: connecting I2C devices for synchronous PD negotiation
Fusb302HwInit: FUSB302 device ID 0x<id> (version <version>)
Fusb302ProbeRole: as sink: CC1 STATUS0=0x<cc1> BC_LVL=<level>, CC2 STATUS0=0x<cc2> BC_LVL=<level>
Fusb302Start: PHY 0 Type-C plug orientation: normal
Fusb302PdNegotiateSink: source offers <count> power data object(s); board accepts 2
Fusb302PdSelectPdo:   PDO <position>: fixed <millivolts> mV, <milliamps> mA
Fusb302PdNegotiateSink: requesting object <position>: <millivolts> mV, <milliamps> mA
Fusb302PdWaitFor: rx ctrl type 3 id <id> objects 0 rev <revision>
Fusb302PdWaitFor: rx ctrl type 6 id <id> objects 0 rev <revision>
Fusb302PdNegotiateSink: contract established: <millivolts> mV, <milliamps> mA
Fusb302Start: PHY 0 sink negotiation: Success, <elapsed-ms> ms, PD=1
Fusb302Start: PHY 0 power state: PD=1, <millivolts> mV, <milliamps> mA
PlatformPower: PHY 0 PD contract <millivolts> mV, <milliamps> mA
PlatformPower: <counter-ms> ms: I2C connection complete; releasing USB/PCIe/SATA power gate
PlatformPower: power-ready observed; enabling configured SATA devices
```

Accept is control type 3; PS_RDY is control type 6. The driver uses PD 2.0
(header revision value 1). Do not require particular message IDs: they wrap
and reset. A flipped cable must print `flipped` and exchange the active CC
sample. BC_LVL must be nonzero on the detected active CC. Raw STATUS0 can
include VBUS and activity bits, so do not require a single exact byte value.
Repeated Source_Capabilities/Request exchanges are allowed within the bounded
retry budget. The SATA line checks configured SATA devices; it does not mean
that a SATA device is present on a default NVMe configuration.

A warm reboot may instead begin recovery with:

```text
Fusb302PdNegotiateSink: no advertisement; sending Soft_Reset
```

ROCK 5B must never print `Fusb302PdHardReset: sending Hard Reset` or
`Fusb302PdNegotiateSink: falling back to Hard Reset`. A **partner-initiated**
Hard Reset is different and is logged as `partner sent Hard Reset`; an analyzer
is needed to establish which endpoint initiated it and whether VBUS dropped.

On failure, look for these formats, followed by gate release and continued
boot if the available physical supply can sustain the load:

```text
Fusb302PdNegotiateSink: PD unconfirmed (<status>); retaining Rd, VBUS voltage unknown
Fusb302Start: PHY 0 sink negotiation: <status>, <elapsed-ms> ms, PD=0
Fusb302Start: PHY 0 PD unconfirmed; VBUS voltage/current unknown; a previous PD voltage may remain
PlatformPower: PHY 0 no verified PD contract; input voltage unverified, reported current 0 mA
```

No orientation or an I2C error can take an earlier exit with
`orientation unavailable` / `Type-C state unavailable`; preserve those logs.
Unknown power must not be mistaken for a measured 5 V fallback or a guarantee
that the source can power NVMe and USB devices.

## Test matrix

Run the boot cases in both cable orientations. For initial characterization,
repeat each cold/warm/power-cycle case at least ten times and record every
failure; this is a suggested sample size, not a reliability certification.

| Case | Procedure | Required observations |
| --- | --- | --- |
| Cold boot | Leave board unplugged until discharged; attach known PD source | Request/Accept/PS_RDY before gate release; no reboot loop; NVMe enumerates; Ubuntu boots |
| Warm reboot | Reboot from Ubuntu, and separately use UEFI reset, without removing VBUS | Previous high-voltage state handled by Source_Capabilities or Soft_Reset; no firmware Hard Reset; consistent new contract and boot |
| Power cycle | Cycle charger's AC input; separately unplug/replug USB-C | Each recovers within bounded firmware waits; record differences from warm reset |
| CC1/CC2 | Flip cable at board between complete power cycles | `normal`/`flipped` change with active CC; same supply selection; USB works in both orientations |
| Fixed PD sets | Use sources/emulator offering 5/9/12/15/20 V in supported combinations | Request only an advertised fixed object, at most 3 A and within 5–20 V; measured VBUS matches PS_RDY contract |
| Unequal PDO power | Offer 5 V/3 A, 9 V/3 A and 20 V/1 A | Select 9 V/3 A (27 W beats 20 W); confirm object index and RDO current |
| Equal PDO power | Offer 5 V/3 A, 9 V/3 A, 15 V/3 A and 20 V/2.25 A | Select 20 V/2.25 A over equal-power 15 V/3 A |
| 5 V-only PD | Offer only fixed 5 V with a valid current | Establish a 5 V contract; adequacy for load is a separate measurement |
| Non-PD supply | Use a correctly wired 5 V Type-C supply; start with minimal load | Bounded unsuccessful PD attempt, no firmware Hard Reset, honest unconfirmed result, continued boot if power permits |
| NVMe boot/load | Install NVMe; boot Ubuntu root from it, then perform sustained I/O against disposable test data | No resets, PCIe/AER errors, NVMe timeouts or filesystem errors; stable measured VBUS |
| USB load | Attach intended USB devices; repeat boot and sustained NVMe + USB activity | Gate release precedes USB-A rail enable/host enumeration; no brownouts or enumeration loss |
| Preferred PDO rejected | Emulator Rejects best PDO, accepts next | `Reject for object <n>; trying another fixed PDO`; next request remains within board limits |
| All PDOs rejected | Emulator Rejects every valid object | Finite number of requests; no false contract; continued boot subject to available power |
| Wait reply | Emulator sends Wait, then accepts; separately sends Wait indefinitely | `Wait; retrying object <n> after 100000 us`; at most two retries; indefinite Wait ends with `source kept answering Wait` |
| Late source | Emulator does not answer the first Soft_Reset, then advertises during the retry interval | Advertisement handled while polling, before another reset; no unserviced 250 ms sleep |
| Chattering source | Emulator alternates Wait and new capabilities indefinitely | Global 21-Request budget ends negotiation; no UEFI hang |
| Missing Accept/PS_RDY | Emulator drops Accept or sends Accept without PS_RDY | Timeout and PD=0; no success solely from Accept; never firmware Hard Reset |
| Incoming Soft_Reset | Emulator resets while waiting for Accept and separately PS_RDY | Accept Soft_Reset with restarted IDs; restart Source_Capabilities exchange, bounded reset budget; no stale successful contract |
| Incoming Hard Reset | Emulator resets during Request, transition and post-contract observation | Clear cached contract and recover from new capabilities if board stays powered; report power loss separately |
| Duplicate packets | Emulator repeats messages with same message ID | Duplicate suppressed; no repeated policy action or false contract |
| Malformed/unsupported PDO | Emulator supplies zero-current/out-of-range PDO, missing initial fixed 5 V, or only unsupported source types | Never request an invalid object; ignore unsupported candidates or reject invalid advertisement; finite fallback |
| Long firmware dwell | Remain in UEFI menu for 10 minutes, then GRUB for 10 minutes | Analyzer captures any later PD traffic/reset and VBUS; one-shot-policy limitation may fail this case |
| Ubuntu handoff | Capture boot through stable Ubuntu and inspect running DT/kernel driver bindings | Record whether FUSB302 is disabled or bound; do not infer takeover from successful Linux boot |

Use a programmable PD source/emulator for injection cases. Do not simulate
protocol errors by shorting signals or applying an unadvertised voltage.
Removing the sole USB-C supply is a power cycle, not a live hotplug test.

After DEBUG passes, repeat the core cold/warm/cycle/orientation/load matrix
with RELEASE. Reduced logging changes timing. Compare with the pinned PR
baseline only when isolating a failure; keep image hashes in every result.

## Results and remaining validation

For each run record: case, repetition, image hash, orientation, selected
PDO/RDO, measured VBUS, elapsed negotiation time, boot result, NVMe/USB result,
UART filename and analyzer capture filename. Record failure observations even
if a later retry boots successfully.

Source tests/builds cannot establish CC analog detection under active BMC,
termination debounce, source deadline margin from initial power application,
warm-reset VBUS continuity, lane/SBU electrical correctness, later PD policy
traffic handling, kernel takeover or Ubuntu/NVMe stability. These remain open
until hardware evidence is captured. Do not describe the candidate as a
validated replacement for production firmware solely because builds pass.

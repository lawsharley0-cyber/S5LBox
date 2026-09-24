<!--
  Extracted from the retired docs/AGENT_HANDOFF.md, section 23.9, on
  2026-07-31, unchanged
  apart from heading depth. It was the newest material in that file and had
  never been committed: 224 of the 235 uncommitted lines in the handoff were
  this section. It is the only record of the audio work anywhere in the tree,
  which is why it was lifted out before the handoff was retired rather than
  after.
-->

# Audio: the codec answers and the I²S windows are decoded

**Status: landed.** `AppleWM8991Audio: I2C register read failed (0): device
error` is gone. The codec is modelled in `core/src/soc/wm8991.c` as an I²C slave
at `0x1B` on i2c0, both I²S windows are decoded device models in
`core/src/soc/i2s.c`, and the two landed together for the reason the warning
below gives. `SNAPSHOT_VERSION` went 10 -> 11.

### The proof, from run87

Absence of an error line is not evidence that a probe succeeded — it is equally
consistent with the driver never running. So run87 armed
`--call-probe-kernel` on the four branches of `AppleWM8991Audio::probe`
(`0xc068b078`, codec vtable slot `+0x178`) and read the registers directly:

```
=== CALL PROBE: CONFIGURED PCs (4) ===
    pc 0xc068b0bc  kernel  captured 1             wrong-mode 0
    pc 0xc068b0f8  kernel  captured 0             wrong-mode 0
    pc 0xc068b118  kernel  captured 1             wrong-mode 0
    pc 0xc068b0ec  kernel  captured 1             wrong-mode 0

    @180963570    pc c068b0bc lr c068b218 ...  r0 00008990 ... r3 00008990
    @180974355    pc c068b0ec lr c068b218 ...  r0 00000000 ...
    @180974358    pc c068b118 lr c068b218 ...  r0 00000000 ...
```

Read that against the disassembly. `0xc068b0bc` is the instruction after
`bne 0xc068b118`, so it executes **only when `readCodecRegister(0)` equalled the
literal**: the capture shows `r0 = 0x00008990` against `r3 = 0x00008990`, which
is the emulated codec's answer arriving over the emulated bus and matching.
`0xc068b0ec` is `ands r0, r0, #0x20` on the value read back from register 1;
`r0 = 0` means bit 5 did **not** stick. `0xc068b0f8` is the first instruction of
the WM1817 branch and captured **zero**. Both runs report the failure line zero
times where run62 reported it once:

```
run62: 1     run86: 0     run87: 0
```

The console around the old failure now reads straight through:

```
Jettisoning kext bootstrap segment.
AppleS5L8900XSDIO: registers @ vaddr 0xe9205000, paddr 0x38d00000
* memMapEntries 6
+ AppleMPVDDriver[0xdc877800]::setPowerStateGated()
ApplePCF50635PMU::start: reading DOWN converter voltages
```

Both runs are 300e6 cold boots with the stock arguments
(`-d devicetree.bin -r rootfs.img -R 512`); the probe fires at instruction
180,963,570.

### The correction this section owed you: R1 bit 5 is inverted

The previous text said "**R1 bit 5 must be writable and read back**". That is
wrong, and building to it would have produced a part the driver calls by the
wrong name. The bit-5 sequence is **not a second gate** — it is a variant
discriminator, and both of its branches return the same success value:

```
c068b0ec  ands   r0, r0, #0x20        ; bit 5 of register 1
c068b0f0  strbeq r0, [r4, #0xc0]      ; clear  -> this->flag = 0
c068b0f4  beq    #0xc068b118          ; clear  -> return, still successful
c068b0f8  mov    r1, #1               ; set    -> this->flag = 1
```

and the getter at `0xc068b044` turns that flag into a name — `moveq` selects
`0xc0690158` `"WM8991"` when the flag is **0**, leaving `0xc0690150` `"WM1817"`
when it is 1. **A part whose R1 bit 5 sticks is reported as a WM1817.** The
shipped device tree calls both of its audio nodes `wm8991`
(`audio-control,wm8991` on i2c0, `audio-data,wm8991` on i2s0) and contains no
occurrence of `1817` anywhere in its 40,544 bytes, so on this board the bit must
read back **clear**. The model leaves it unimplemented: writes are discarded,
reads return zero. This is not cosmetic — the flag is loaded from `this+0xc0` at
ten further sites in the kext.

Register 0 is the **only** hard gate, and it was verified three ways: the word
at `0xc068b124` is `0x00008990` read at the byte level; `0xc068b0ac` is the sole
instruction in the entire kernelcache that loads that literal; and entry 0 of
the driver's own default table at `0xc0691030` is the same `0x8990`.

### The wire protocol, from the helpers rather than a datasheet

`AppleWM8991Audio` inherits its transfer helpers from `AppleEmbeddedAudio`
(`0xc053e000..0xc054c000`, ARM) and reaches them through its vtable
(`V = 0xc0690b20`, base `V = 0xc054867c`; the two tables are structurally
aligned and three separate slots — `+0x178`, `+0x39c`, `+0x3a4` — are
superclass calls at the same offset, which is what pins the bases).

- **read**, `0xc053ff94`: one index byte out, then **two bytes in, MSB first**.
  The assembling instruction is the claim: `ldrb r3,[sp,#0xe]; ldrb r0,[sp,#0xf];
  orr r0,r0,r3,lsl #8` at `0xc0540030..38`. On failure this is the helper that
  prints `%s: I2C register read failed (%#x): %s` (`0xc0548048`) — the exact
  format behind the old `(0): device error`, `%#x` of zero rendering bare.
- **write**, `0xc0540050`: index byte, then the value MSB-first — three bytes.
  This is the form the codec uses (`writeCodecRegister` `0xc068b168` dispatches
  to it).
- A second, **packed** form exists at `0xc0540108` — `(reg << 1) | value[8]`,
  then `value[7:0]`, two bytes — but it belongs to the WM8758 sibling driver.

The model implements both and lets the **byte count** select: one byte is a
pointer-only write (the stock controller's read setup, which must not be
committed as a store), two is packed, three is wide, anything else is NAKed and
counted. The counts are distinct, so nothing guesses.

**Only six registers ever reach the bus.** `readCodecRegister` (`0xc068b1b4`)
gates every read on the bitmap `0x0084000f` at `0xc068b21c` via
`ands r3, r2, r3, lsl r1` over `1 << reg`; set bits {0,1,2,3,18,23} mean
registers **`0x00, 0x01, 0x02, 0x03, 0x12, 0x17`** and nothing else. Every other
read is served from a RAM shadow the driver seeds from `0xc0691030`.

### The one thing that is inferred rather than observed

The codec's GPIO configure path holds a poll with **no timeout, no iteration cap
and no delay in its body**, at `0xc068d4ac..0xc068d514`:

```
rmw(0x17, 0x1000, level << 12);           // c068d44c, before the loop
do { write(0x17, computed);               // c068d4cc
     write(0x12, last & 0x0000efff);      // c068d4e8 — clears bit 12
     v = read(0x12); }                    // c068d4fc
while ((v & ~arg & 0x1000) != (level << 12));   // c068d50c
```

It forces bit 12 of `0x12` clear on every write and then waits for that same bit
to read back as `level`. So it cannot be waiting for storage: on a part where
`0x12` bit 12 held what was last written, `level == 1` never terminates. The
model therefore makes **`0x12` bit 12 a read-only mirror of `0x17` bit 12** and
nothing more. That is an inference from the poll's own structure, deliberately
the narrowest one that lets a loop with no exit but the device finish; it is
counted (`status_mirror_reads`) so a boot can say how often it mattered.

### The I²S side: both prior claims verified, independently

`readRegister` (`0xc05a3c84`) really **has no caller anywhere in the
kernelcache**. Four checks agree: its address occurs as an aligned word exactly
once in the file (its own vtable slot `0xc05ad888`); no ARM or Thumb BL/BLX
targets it; no `ldr pc,[Rn,#0x3c0]` dispatch exists with any base but PC; and
slot `+0x3c0` is a virtual the class *introduces* — its parent
`AppleARMIISController`'s vtable ends at `+0x3ac` — so no other kext can reach
it through a base pointer even in principle. The class contains exactly **one**
MMIO load instruction in the whole image and it is dead code.

`writeRegister` writes exactly the seven claimed offsets, enumerated from every
dispatch site: `configure()` `0xc05a3820` writes `+0x00` (cfg|1), `+0x40`,
`+0x04` (cfg|1), `+0x30`, `+0x08` (0), `+0x34` (0), `+0x3c` (1, if TX);
`startTransfer()` `0xc05a3928` writes `+0x08` and `+0x34` with **6**; `stop()`
`0xc05a3ad0` writes both with **0**. Distinct set = `{0x00, 0x04, 0x08, 0x30,
0x34, 0x3c, 0x40}`.

Two further facts worth carrying. **`start()` cannot spin**: `0xc05a3d24..
0xc05a3f58` performs zero MMIO and contains zero backward branches. And the
class is **ARM** while its parent `AppleARMPlatform` is Thumb — its vtable's own
class entries are all even where the inherited ones are odd. The transfer wait
at `0xc05a39a0` is `commandSleep(..., 1)`, i.e. `THREAD_ABORTSAFE` **and
timer-bounded**, and depends on the GPIO-IC interrupt (`0x86` for i2s0, `0xaa`
for i2s1) or the timeout — never on a register value. The five unbounded waits
§23.9 warned about are one layer up, in `AppleARMIISAudio`/`AppleEmbeddedAudio`
(`IOLockSleep` at `0xc053a5f8`, `0xc053a644`, `0xc05429d4`, `0xc0542a74`).

The windows are `0x3CA00000` and `0x3CD00000`, both 0x1000. i2s0 carries the
codec (`/arm-io/i2s0/audio0`, `audio-data,wm8991`); i2s1 carries the baseband
voice path (`audio-data,baseband`). They are still recording **zero** MMIO
traffic after this change — they do not appear in run86's 15-page census — which
is exactly what the dead-`readRegister` analysis predicts, since nothing writes
them until a transfer is actually configured.

### What was deliberately NOT modelled

- **The 63-entry default register table** at `0xc0691030`. It is the driver's
  own belief about an untouched part and the driver never needs the device to
  supply it; copying it in would be the full register map this model refuses to
  invent. Its entry 0 agreeing with the identity literal is recorded above as
  corroboration, not as a reason to ship the other 62.
- **Any semantics for the seven I²S offsets.** The `+0x08`/`+0x34` pattern
  (0 configured, 6 running, 0 stopped) is consistent with a per-direction
  enable and bit 0 is forced set at `+0x00`/`+0x04`, but "consistent with" is
  not "established". The model stores; it does not interpret.
- **Any interrupt line.** There is no `S5L8900_IRQ_I2S0/1` constant, for the
  same reason there is no `S5L8900_IRQ_UART4`: the `interrupts` properties say
  `0x86`/`0xaa` but both nodes name `interrupt-parent = /arm-io/gpio`, so they
  are GPIO-IC lines, and nothing in this model raises either.
- **The other i2c0 nodes** — accelerometer `0x1d`, ALS `0x44`, tethered `0x29`.
  Nothing establishes what they must answer, and an address that NAKs is a
  driver that fails cleanly, which is what all three do today.
- **The PL080, and any host audio sink.** Samples still need the DMAC; host
  playback needs a portable sink that must never block the CPU thread, and
  `core/` has no threading vocabulary. Both are separate work.

### Tests and mutants

`core/tests/test_wm8991.c` — 11 cases, 493 checks, registered as
`wm8991_codec_and_i2s`. Suite 34 -> 35 (36 with the buttons work that landed
alongside). The codec is driven through the **real** I²C controller using the
stock register sequence, so a change to `i2c.c` that broke it fails here rather
than only in a six-minute boot.

**14 mutants, 14 killed, 0 survived.** Two of them are worth carrying:

- *the mutant that survived first.* Removing the seven-bit wrap from the read
  auto-increment changed nothing, because a second mask at the point of use made
  it redundant — and that redundancy was hiding a real bug: a pointer-only write
  of index byte `0xff` stored `ptr = 0xff`, which the snapshot invariant
  (`ptr < 0x80`) then rejected. A guest-reachable byte made the machine unable to
  checkpoint. Narrowing at the point of *store* fixed the bug and made the
  mutant killable.
- *the mutant the harness refused to score.* Changing `WM8991_I2C_ADDR` from
  `0x1b` to `0x1a` survived, because every test used the symbol. Constants need
  a test that spells out the literal and cites where in the firmware it comes
  from; `test_constants_match_the_shipped_firmware` is that test.

The harness (a) rebuilds and requires the mutant to **compile** before believing
any result, and (b) diffs the file and reports `INVALID` rather than "survived"
when an edit does not apply. It caught one genuine no-op edit this way.

### Tooling note

`tools/kdisasm.py`, `dtwalk.py`, `findstr.py`, `vtscan.py` and `kcensus.py` all
hardcode `REPO = F:\JOSHUA_1st_2021\projects\S5LBox`, which does not exist on
this machine — they fail immediately. `machosyms.py` and `findcalls.py` take a
path argument and work. `kdisasm.py`'s `SEGS` table itself is correct for this
image, verified against the file's own `LC_SEGMENT` commands.

**run62 settled the question that branched the plan.** `--call-probe-kernel` on
`AppleS5L8900XI2SController::start`'s tail: `0xc05a3f40` (publishChildren) and
`0xc05a3f44` (return true) each **captured 2**, `0xc05a3f4c` (return false)
captured **0**. Both controllers publish, at instructions 235,126,205 and
236,694,444. So the `AppleARMIISDevice` nub named `audio0` **exists today**, the
two `IODMAEventSource`s are created successfully, and the three NULL-timeout
`waitForService` calls ahead of it already resolve.

Consequence: **the PL080 can be deferred.** Steps 1-2 — the codec slave plus the
I²S window — are 2-3 days and produce a fully attached codec with a live
`IOAudio2Family`. Samples need the PL080 later, and there is **no boot-argument
escape** for it: all 40 `PE_parse_boot_argn` sites were enumerated and the
`<node>_dma_enable` pattern exists only in `AppleS5L8900XSerial`.

The I²S window itself is nearly free. The driver funnels access through two
accessors and **`readRegister` (`c05a3c84`) has no caller anywhere in the
kernelcache** — it writes seven offsets (`0x00, 0x04, 0x08, 0x30, 0x34, 0x3C,
0x40`) and never reads. The FIFOs at `+0x10`/`+0x38` are touched only by the
PL080 as physical addresses in an LLI. That independently explains the zero
MMIO traffic the census shows on `0x3CA00000`/`0x3CD00000`, and makes the window
80-120 lines rather than 250-350.

> **Do not land the codec alone.** Today's failure is loud, correct and
> harmless, and the boot continues past it. There are **five unbounded waits**
> on the audio path — three NULL-timeout `waitForService` in series plus two
> uninterruptible `IOLockSleep`s in the transfer path — so a codec that answers
> `0x8990` with no I²S window behind it converts a good failure into a **silent,
> uninterruptible hang**. That is a regression, not progress. Steps 1 and 2 are
> one unit.

Two things to keep honest about scope. **Nothing makes a sound today** and
nothing would even with a perfect stack: the activation screen is silent and
iPhone OS 3 has no boot chime, so audio output is downstream of touch exactly as
Wi-Fi is. And **you cannot hear it live** — at 200-294× slower than the 412 MHz
part, the smallest UI sound costs about 6 seconds of wall clock and one guest
second costs about 200. The right design is to clock the model off the guest
timebase, capture PCM to a WAV, and let the host play it back afterwards.

One method note worth carrying. Before run62, the evidence for the nub was that
the device tree declares 14 DMA channels while only 10 initialise, a shortfall
of exactly 4 matching i2s0's 2 plus i2s1's 2. That fit was suggestive and
**wrong** — other partitions exist, the analysis labelled it cannot-determine
rather than concluding from the coincidence, and a four-minute read-only run
settled it properly. The tidy arithmetic would have sent the plan down the
expensive branch.

# 23.8 The trap that has now cost three retractions

A per-process trace block that reports one generation does not describe the
whole run, and **host-side counters are not machine state**: `core/include/snapshot.h`
says a restored process starts them fresh, so a zero in a restored run means
"not seen since the restore point", never "never happened". On 2026-07-26 that
misreading produced a committed claim that `AppleH1CLCD::createSurface` never
ran, when it runs and succeeds at instruction ~238,400,000.

**Compare the heartbeat pc stream before reading divergence into differing
counters.** Restore is bit-exact — 20/20 heartbeats identical between cold run52
and restored run56 across 3.0e9-4.9e9, agreeing 862 million instructions past
the restore point, across three different binaries.

# 2026-09-18 The AMC "lock BSU" freeze — real bug, root cause not yet safe to fix

**User-reported, on real hardware, real firmware, reproduces every time:**
opening Settings > Sounds and playing a sound freezes the guest. Confirmed
via the app's own Console output that this is the **same failure that fires
automatically during ordinary boot**, right after `launchd[N] Builtin
profile: MobileMail (seatbelt)`, with no user interaction required — the
Sounds-screen trigger just makes an already-present, already-silently-firing
loop visible in the foreground.

**What the guest console actually shows, every time, in a tight cycle:**
```
Assertion failed in ".../AppleAMCDriver_r1.cpp" at line 350/565/726
ERROR: AMC reset [non-fatal error]: could not lock BSU!
AppleEmbeddedAudioDevice: could not start DMA: operation was aborted
```
repeating forever, separated by a bounded ~1200-cycle WFI delay loop.

## What is confirmed, not guessed

- **This is an active busy-retry loop, not a silent/uninterruptible sleep.**
  The host UI stays fully responsive (confirmed by the user); only the guest
  pegs a CPU core retrying. `bootkernel`'s own `[stall]` sampler independently
  corroborates this: "the guest is looping in N regions... 0xc0062300-0xc006233f
  took 508 of 512 samples" — decoded (`kdisasm.py --arm 0xc00622c0 0xa0`) as a
  standard bounded delay primitive: `mov r3,#0x4b0` (1200), a
  `mcr p15,0,r2,c7,c10,4` (DSB) + `mcr p15,0,r2,c7,c0,4` (WFI) + decrement
  loop. The loop itself is correct, ordinary kernel code; it is the *outer*
  retry that never succeeds.
- **Not the build-time compact AArch64 engine.** This session shipped a
  "Force Interpreter" developer-mode toggle (`VMEngine.h`/`.m`,
  `EmulatorViewController.m`, commit `164c435`) specifically to test this.
  With the interpreter forced on (compact engine fully disabled), the freeze
  reproduces identically. Whatever this is, it is not an AOT translation bug.
- **Not jailbreak/Cydia state.** Reproduces on a freshly created machine with
  no `jb-payload` baked in, exactly as it does on a jailbroken one.
- **Traced to a real write-then-poll-readback register coupling**, the same
  bug *class* `s5l_power_t`'s own header already documents at length for
  `AppleS5L8900XPowerController::start`'s `STATE` register (a storage stub
  cannot fix "write register A, poll register B" — only "poll the register
  you already own"). Two call targets were resolved from this session's own
  extracted `firmware/kernel.macho` (byte-verified, see
  `PERFORMANCE_BASELINE.md`): `0xc0717838` and `0xc0717434`. The second
  writes 0 to offset `+0x400` of some device object's mapped block, reads it
  back, and treats nonzero as "not yet cleared" — a classic
  write-a-request/poll-for-hardware-self-clear pattern.

## What was tried and did not work

`core/src/soc/machine.c`'s `clkrstgen` stub (`AppleS5L8900XClockController`,
`0x3c500000`) was reset-filled with `0xFFFFFFFF` instead of zero on the
hypothesis that a clock/power-gate status bit there was the one being polled
(commit `163f955`). **Tested on the user's real device: no change, freeze is
byte-for-byte identical.** This does not mean the fix was wrong in spirit —
`clkrstgen` may still be worth keeping reset-filled rather than zeroed on
general principle — but it rules out `clkrstgen` specifically as *this* bug's
register. The device the `+0x400` register actually belongs to is still
unidentified.

## Why this session stopped here, honestly

Continuing past the two resolved call targets requires reading literal-pool
values out of `__PRELINK_TEXT` by hand (`kdisasm.py` plus manual byte-order
math, no cross-referencing disassembler). One resolved value along that path
(`0x101c`) does not look like a valid kernel pointer, which means a
transcription or addressing error was made a step or two before it and not
caught — this class of manual reverse engineering is genuinely error-prone
past a certain depth without better tooling, and shipping a fix built on an
untrusted trace would repeat exactly the mistake `clkrstgen` already was.

This session also could not reproduce the freeze locally (Windows x86-64
desktop, pure interpreter, four attempts with progressively closer-matched
boot configuration including the user's exact jailbreak boot-args) — see
`PERFORMANCE_PLAN.md` §8 for the same cross-environment gap affecting the
CPU-performance track. Local reproduction would have made the rest of this
tractable; without it, the only way to get further evidence is from the
device itself.

## What would actually resolve this, for whoever picks it up next

1. **Xcode Instruments (Time Profiler or, better, a live `lldb` attach) on a
   Mac, against the real device, while reproducing the freeze.** This is a
   host-side native-code stack, not the emulated guest, so standard iOS
   debugging tools apply directly and would show the exact call chain and
   register/memory state in seconds, instead of the hours of static
   disassembly this session spent getting two call targets deep.
2. Failing that, redo the literal-pool trace from `0xc0717838`/`0xc0717434`
   from scratch with fresh eyes (do not reuse this session's intermediate
   arithmetic) and a proper cross-referencing disassembler if one becomes
   available, rather than `kdisasm.py`'s manual byte math.
3. The "Force Interpreter" toggle (Developer Mode → machine menu) now ships
   in the app and is generally useful for any future "is this an emulator
   bug or a translation bug" question, not just this one.

# 2026-09-23 Device-side evidence for the BSU loop, without a disassembler

The trace above stopped at "the device the `+0x400` register actually belongs
to is still unidentified". The machine now keeps, always on, the most recent
distinct accesses to hardware it does not model (unmapped addresses and
storage-only stubs), each with the guest pc, the value, and a repeat count
(`s5l8900_t::unmodelled`, `s5l_unmodelled_describe()`). A driver spinning on
a register it is waiting for shows up as a few lines with large counts.

To capture it on the device: boot, reproduce the freeze (it also fires on its
own after `launchd[N] Builtin profile: MobileMail (seatbelt)`; Settings >
Sounds makes it visible), wait for the console's `[stall]` line, which now
prints the most recent unmodelled accesses, then open Performance & Sound
Details and Copy Report. The pcs resolve to kernel symbols with
`machoinfo <kernel> -r <pc>`; the address and region name say which window
(`sram/amc`, a stub such as `clkrstgen`, or plain unmapped) the driver is
polling, and the value says what it keeps reading. That is the input a
register model needs; nothing is changed in the guest by collecting it.

## First device report (iPhone18,2, iOS 27.2, build 3a9ddef, 2026-09-23)

The run was on the forced interpreter (`(interpreter control)`), 6,627.6 M
instructions in, guest time 0.41x wall time; I2S0 received 0 words. The
twelve most recent unmodelled accesses were:

| pc | access | count | region |
|---|---|---:|---|
| c05a7e54 | R 3c500024 -> fffffffa | 19,461 | stub clkrstgen |
| c05a7e60 | W 3c500024 <- fffffffb | 19,479 | stub clkrstgen |
| c05a7e78 | W 38100008 <- 00001000 | 19,461 | stub miu |
| c05a7e78 | W 38100404 <- 00000000 | 19,461 | stub miu |
| c05a7e60 | W 3c50004c <- 0001edcf | 1,003 | stub clkrstgen |
| c05a7e60 | W 3c500048 <- 6fcff3ff | 22 | stub clkrstgen |
| c06f5526 | R 38d00018 -> 00000000 | 10,001 | unmapped arm-io |
| c06f54e8, c06f54ac, c06f54a6, c06f460c, c06f4604 | 38d00010 R; 38d00008 <- 00040045, 38d0000c <- 0, 38d00034 <- 2, 38d0003c <- 3 | 1 each | unmapped arm-io |

What this shows, and what it does not:

- **Observed:** one ARM routine (c05a7e54-c05a7e78) read-modify-writes
  clkrstgen `+0x24`, toggling bit 0 (the stub stores writes, and the last
  read returned the bit clear while the last write set it), and on each pass
  writes the MIU's `+0x008` (0x1000) and `+0x404` (0) — the two MIU offsets
  already recorded as the clock controller's second range. About 19,500
  passes over the run. The `+0x404 <- 0` write is the "writes 0 to offset
  +0x400-ish" of the earlier trace; nothing ever reads it back.
- **Observed:** a Thumb routine (c06f4604-c06f5526) programs an unmapped
  block at 0x38d00000 (+0x3c, +0x34, +0x0c, +0x08 = 0x00040045), reads
  +0x10 once, then polls +0x18 exactly 10,001 times (reads 0) and gives up:
  a bounded poll timing out. `BOOTLOG.md` records
  `AppleS5L8900XSDIO::sendCommand(): Timeout waiting for CMDRDY`; that this
  block is the SDIO controller is a **hypothesis** until the pcs are
  resolved to symbols.
- **Not observed:** any access to the `sram/amc` window (0x22000000) among
  these entries, and nothing reads back a register that the loop above
  could be waiting on. The next build therefore also logs the most recent
  accesses to **modelled** devices (`s5l8900_t::mmio_recent`), because a
  poll on the power controller or another modelled block would not appear
  in the unmodelled table at all.

To resolve the pcs with your own kernelcache on a desktop:
`machoinfo <kernel> -r c05a7e54`, `-r c06f5526`.

## Second device report (build 3f74a82): the BSU lock is AMC +0x400

With every device access logged, the retry loop is fully visible (unmodelled
table, most recent first; counts are since each entry was last inserted):

- `c05a7e54/60/78`: clkrstgen +0x24 bit 0 toggled and MIU +0x008/+0x404
  written, x13,174; `c0717714`: **W 0x38502000 <- 3**, x13,174 — the AMC
  reset, once per retry.
- `c07174cc/c07174d4` then `c07174fc/c0717504`: **W 0x38500400 <- 1, R
  0x38500400 -> 0**, x3 and x3,000 — the lock: write 1, read it back,
  up to 1,000 times per attempt, give up when it stays 0.
- After it, one pass of set-up: 0x38501000..0x3850101c (a channel: enable,
  **0x095b5000** — a DRAM address — and 0xfef, i.e. a 4,080-byte buffer),
  0x38500a00, 0x38500004, 0x38500b04/0b0c, 0x38500204, polls of 0x38500b18
  (x11) and 0x38500010, and two halfword reads of SRAM 0x22028000.

0x38500000 is `/arm-io/amc` reg[0] (child 0x00500000 in the first arm-io
window); 0x22000000 is its reg[1], the SRAM. Neither was modelled, so every
read returned 0 and the lock could never be taken.

Change: both are declared as honest storage (`S5L8900_AMC_BASE`, 0x3000
bytes, the span the driver touched; `S5L8900_SRAM_BASE`, 0x2c000). Reads
return what the guest wrote, so the lock read-back sees the 1 it wrote. That
is storage, not a fabricated status. What it does NOT do: move audio. The
channel at +0x1000 names a DRAM buffer and a length, which looks like the
AMC fetching samples itself rather than the PL080 feeding I2S0; if the next
report shows the driver waiting on +0x0b18/+0x0010 for progress, that is a
real DMA model to build (AMC -> audio sink), with its registers read from
the driver's own accesses.

## Third device report (build 9382179) and the driver's own register protocol

The BSU lock now succeeds (`+0x400` reads back the 1 written), and the next
failure is `AppleEmbeddedAudioDevice: could not start DMA: operation was
aborted`. The user copied the kernel code around every pc that touched the
AMC (opt-in "Copy Audio Driver Code", VA 0xc07170c0, 0x12b0 bytes, SHA-256
f74fa5bc...95f9; disassembled here, not stored in the repository). What it
establishes, as register semantics read from the driver (inference is marked):

| Offset | Access | Driver use |
|---|---|---|
| +0x000, +0x004, +0x008, +0x00c | W 1 | separate "go" strobes (small leaf setters) |
| +0x010 | R | compared with 1; when equal the driver writes +0x00c = 1 and then waits for +0xb18 bit 8 (up to 10 x 1 ms) |
| +0x100 | W | mailbox 0 push (used to restore saved entries) |
| +0x180/+0x184/+0x188 + 16n | W 1 / RW / R | per-mailbox control, data, pending count |
| +0x204 | W 1 | enable |
| +0x340 | W 0/1 | written 0 before the SRAM is cleared and 1 after a copy loop that precedes the excerpt: INFERRED "run" of something that executes from the SRAM |
| +0x400 | W 1 / W 0, R | the BSU lock: write 1 and read back until non-zero (10 ms), write 0 to release |
| +0x404 | R | status returned by every locked save/restore |
| +0x410..+0x428 | RW | seven words saved and restored around a reset, with the mailbox 0 contents (+0x47c read while +0x188 > 0) |
| +0xa00 + 4n | W | per-channel enable |
| +0xb04, +0xb0c, +0xb10 | W | configuration (0x2000; 0x400 or 0x2000; 0x400 or 0x7fff) |
| +0xb18 | R | status: bit 8 awaited after start, bit 9 awaited by a second routine (10 x 1 ms each); the storage stub returns 0, which matches the 44 polls in the report |
| +0xb1c | R | status |
| +0xd00 / +0xd04 | R / W 0,1 | |
| +0xf00, +0xf04 | W | the same value to both |
| +0x1000 | W | channel enable |
| +0x1004 / +0x1008 | W | DRAM start address / length in words |
| +0x100c | W | start (1) / stop (0) |
| +0x1010 | R | current read pointer: the driver computes words remaining as length - (current - start) / 4 |
| +0x1014 | W 1 | abort (with +0xa00 = 0 and +0x1000 = 0) |
| +0x1018 | W 0/1 | INFERRED interrupt enable |
| +0x101c | R bit 0 / W 1 | done status, write 1 to acknowledge |
| +0x2000 | W | n - 1 (a divider) |
| SRAM +0x5000, +0x14000, +0x1c000..+0x28000, +0x28000 | | cleared at init; two 24 KiB sample buffers at +0x1c000 and +0x22000 (0x6000 apart); a halfword read at +0x28000 |

If the SRAM does hold code that the block executes (the +0x340 reading),
then status bits 8/9 and the mailbox replies come from that code, and a
correct model has to reproduce its protocol rather than set bits. The calling
logic sits before this excerpt, so the excerpt window was widened to 16 KiB
before the first accessing pc (and 8 KiB after the last) to capture it.
Nothing is modelled from this yet.

# 2026-09-24 The whole AMC kext (build 6ec47e8) and what it adds

The Full Test Report carried AppleAMC_r1 from its Mach-O header at
0xc0712000 through 0xc0758000 (SHA-256 df16fc63...1a31; read in a scratch
directory, not stored here). Its own load commands put `__text` at
0xc0713000 (0xa338 bytes, ARM), `__const` at 0xc071d338 (0x4e3e4 bytes),
`__cstring` at 0xc076b71c (0xadbe bytes) and `__DATA` at 0xc0777000. The
copy stopped short of `__cstring` and `__DATA`, and the dump window is now
wide enough to include both next time.

**Observed:**

- The low-level layer is a small C HAL over one global at 0xc07770a0:
  word 0 is the mapped SRAM, word 1 is the mapped register block. Every
  register access in the earlier tables goes through it. It holds:
  - one-word setters for the strobes (+0x0, +0x4, +0x8, +0xc) and for +0xd04;
  - `+0x2000 = n - 1`;
  - two DMA starts: ack +0x101c, stop +0x100c, enable +0x1000, then address
    +0x1004 and length +0x1008. One of them also sets +0xa00 = 1 before
    +0x100c = 1;
  - a progress read: +0x1010, returned with `len - (cur - start) / 4`;
  - a wait routine at 0xc071768c. If +0x10 reads 1 it writes +0xc = 1. It
    then polls +0xb18 bit 8, with IOSleep(1) between polls, 10 times.
- The SRAM clear before any use: +0x5000, +0x14000..+0x1c000 and
  +0x1c000..+0x28000 are zeroed with +0x340 = 0 in between. Then +0x28000
  and the 16 KiB after it are zeroed.
- The report's access order is:
  1. `+0x2000 = 3`;
  2. a streaming channel on DRAM 0x095b5000, 0xfb1 words, started with
     +0xa00 = 1;
  3. `+0x4 = 1`;
  4. two halfword reads of SRAM +0x28000, which return 0;
  5. configuration writes (+0xb0c, +0x204, +0xb04);
  6. the BSU lock, three times;
  7. the channel torn down four times;
  8. 44 polls of +0xb18, i.e. four waits of 11 reads, all timing out.
  The assertions (lines 350, 565, 726, 2097) print in that window.
- `__const` begins with a table of (pointer, word count) pairs: eight
  blocks of 0x4ea or 0x4eb 32-bit words, about 5 KiB each, followed by
  one-word entries. The blocks are not ARM or Thumb code (under 2% of
  words carry ARM's always-condition, against 75-83% in `__text`).
  Byte-reversed four-character codes in `__text` read `paac`, `.mp3`,
  `alac`, `aach`, `lpcm`.

**Inferred, to be checked against `__cstring` in the next report:**

- The eight blocks are microcode for the AMC's own processor, one per
  codec or mode, and the block decodes compressed audio (MP3/AAC/ALAC) in
  hardware as well as playing PCM.
- The driver starts that processor and waits for it to answer. The answer
  is a halfword in SRAM at +0x28000 or status bit 8/9 in +0xb18, and
  nothing in this VM ever sets either. That would explain every
  assertion, and it means the fix is a model of what the firmware
  reports, not more storage.

A faithful model would have to run that microcode, and its instruction set
is not documented. The practical model is behavioural: acknowledge what
the driver starts (status bits, the SRAM reply), and play the PCM that the
streaming channel reads from DRAM at the rate +0x1010 must advance. The
next report's `__cstring` names each assertion. That maps the four lines
to the checks that fail, and so to the exact replies the model owes.

# 2026-09-24 The assertions, named (build bc45a3f)

The bc45a3f report carried the whole of AppleAMC_r1 (va 0xc0712000, 0x66000
bytes, SHA-256 d3611f38...626f; read in a scratch directory, not stored
here), `__cstring` and `__DATA` included. Each assertion is a preformatted
string passed to `IOLog`, so a string's address leads straight to its call
site.

**Observed:**

- **The four assertions are one failure.** Line 350 is in the reset routine
  at 0xc0715748. That routine calls the device-tree platform functions
  `function-core_reset` and `function-de_reset` (looked up by name during
  start, at 0xc0716428) with argument 1, then the wait at 0xc071768c. Line
  349 would mean the `de_reset` call failed; it never printed. Line 350 means
  all ten polls of +0xb18 bit 8 read 0. The other three are callers giving
  up:
  - line 565 is in the codec start at 0xc0715a48 (vtable +0x3f8);
  - line 2097 is in the locked reset at 0xc0715804 (vtable +0x478, whose BSU
    error reads "AMC reset [non-fatal error]");
  - line 726 is in 0xc07135fc (vtable +0x3fc), which fails when +0x3f8 does.
- **What the block is.** The strings name the driver classes
  (`AppleAMCDriver_r1`, `AppleAMCDriverManager`) and the codecs they load:
  `Espico_mp3`, `Espico_aac`, `Spirit_aache`, `Spirit_aace`, `ARM_alac`,
  `ARM_acelp`, and `::Encoder`. They also name:
  - the properties it publishes: `input format`, `output formats`,
    `encoder bitrate`;
  - the DSP memory regions it loads code into: `pmem`, `sys_pmem`,
    `ram_mc1`, `ram_mc2` and `AMCSS`;
  - a dependency on `com_apple_driver_FairPlayIOKit`;
  - its own description of what it runs: "starting the transformer without
    an input magic cookie".

  The AMC is a codec offload engine for MP3, AAC, HE-AAC and ALAC. It is not
  the PCM output path.
- **After a successful reset**, the codec start goes on to:
  1. reset the mailboxes and the channel;
  2. write +0x204 = 1, a mailbox word, and +0xb0c = 0x2000;
  3. set +0x1018 = 1;
  4. wait for +0xb18 **bit 9** (0xc0717b94, 10 polls 1 ms apart).
- **Nothing links it to PCM playback.** `__text` contains no call into
  AppleEmbeddedAudio. The report's "calls" into AppleEmbeddedAudio,
  AppleEmbeddedUSBAudio and AppleWM8991Audio are words in `__const` (the
  microcode tables) that happen to decode as Thumb `BL`.

**Not observed:** what `function-core_reset` and `function-de_reset` do in
hardware. The platform function lives in another kext.

**Inferred:** +0xb18 bit 8 says the DSP core is out of reset, and bit 9 says
the loaded codec is running. Both are answers from the DSP's own firmware or
reset logic.

**Decision: these bits are not set.** Setting them without running the codec
would let the driver start a decoder that never produces a sample. For any
app that asks for hardware decode, that turns today's fast, logged failure
into a hang. A working AMC means high-level emulation of the transformer
protocol (the mailboxes, the channel at +0x1000, the two 24 KiB SRAM buffers)
with host decoders behind it. That is a project of its own and comes after
PCM output works.

**The PCM path fails on its own.** The line to diagnose is
`AppleEmbeddedAudioDevice: could not start DMA: device is not ready`, and I2S0
has received 0 words. The report now also carries:

- AppleEmbeddedAudio's kext;
- the kext whose code touches the I2S windows;
- AppleARMPL080DMAC's kext;
- every PL080 channel's registers;
- the I2S windows' storage;
- a separate access log for the I2S windows.

The AMC kext's bytes are left out when their SHA-256 matches the copy already
analysed.

## 2026-09-24 Why DMA never started, and the frame clock (build d09d9b8 reports)

In the three d09d9b8 reports, i2s0 received configure()'s seven writes and
never the `6` that startTransfer() writes to +0x08. The DMA controllers had
no channel pointed at 0x3ca00010. So startTransfer() returned before it
started DMA. Its code (AppleS5L8900X, 0xc05a3928) is a wait:

| Step | Address | What it does |
|---|---|---|
| 1 | 0xc05a3964 | state (this+0x8c) = 1 |
| 2 | 0xc05a3980 | enable the nub's interrupt 0 (vtable +0x220) |
| 3 | 0xc05a3998 | arm a timer from the command's timeout |
| 4 | 0xc05a39a0 | commandSleep on the state word until it is 3 or 4 |
| 5 | 0xc05a3a3c | only for state 3: start the DMA channel, then write 6 |

The interrupt handler at 0xc05a3c2c moves state 1 to 2 on the first
interrupt. On the second it stores 3, disables the interrupt and wakes the
sleeper. The timer stores 4, which returns `kIOReturnNotReady`
(0xe00002d8, "device is not ready"). An interrupted sleep returns
`kIOReturnAborted` (0xe00002eb, "operation was aborted"). Those are the two
messages every report carried.

Interrupt 0 of the i2s0 nub is GPIO-IC line 0x86 (i2s1: 0xaa), and nothing
in the emulator drove either line. That was the whole failure.

**Inferred:** the line is the I2S frame (word-select) clock's pin. The
handler reads no register and no time; it counts two edges and starts DMA.
That is how DMA is usually lined up with a frame boundary.

**What changed:**

- Each I2S window runs a frame clock once configure() has set +0x00 bit 0.
  The clock drives its GPIO-IC line: high for the first half of each frame.
- The clock also drains the TX FIFO, one 4-byte frame per edge. The FIFO's
  DMA request is asserted only while it has room. Before this, a started
  channel would have moved its whole list in one refresh.
- The host sink now gets whole 32-bit frames (left sample in the low half).
  The `dma-channels` template 0x00249000 means 16-bit DMA stores. The old
  path would have played each 16-bit store as a stereo frame.
- The idle fast-forward and the cached interpreter's horizon know the new
  edges. An unmasked frame-clock line names its next level change. A DMA
  channel feeding the FIFO names the frame at which its current item ends.

**Not decoded, and stated in soc.h:**

- The rate is a nominal 44.1 kHz, which is what the app's output assumes.
  +0x04 = 0x01100301 in every report, and nothing says which field is a
  divider.
- The FIFO depth is 64 bytes. The driver never reads a FIFO level.
- 16-bit stereo frames.

The wait's handler cannot observe the rate or the depth. They only set the
pace of the audio.

Snapshots are v33: the clock phase and the FIFO state are guest state.

**What the next report should show if this worked:**

- i2s0 +0x08 = 6.
- A dmac channel with dst 0x3ca00010.
- "frame clock running".
- A rising frames count and non-zero "frames to host".
- No "could not start DMA" lines.

If the lines are gone and the sound is still silent, look next at the app's
"Guest I2S0" line: words received, how many were non-zero, and underruns. The
app plays whatever reaches I2S0; the codec's own registers do not gate it.

Tests: `core/tests/test_audio_dma.c` (153 checks, including a replay of
startTransfer()'s two-edge wait and a WFI-sized refresh that must equal
per-tick refreshes). Three hand mutants were all caught: no pulse on a
multi-frame refresh, FIFO room ignored, and the DMA wake source removed.

## 2026-09-24 The PCM path runs, and plays silence (build 12b2cba)

The first device report with the frame clock (CPU graphics, Settings >
Sounds, 76.7 s window):

| Measurement | Value |
|---|---|
| "could not start DMA" lines | none |
| i2s0 frame clock | running, 1,687,315 frames |
| DMA into 0x3ca00010 (dmac0 ch2, 16-bit stores) | 2,918,160 stores, 5,836,320 bytes, 1,423 items, 90 completions |
| Frames handed to the app | 1,459,080 |
| App: words received / non-zero | 1,459,080 / **0** |
| startTransfer/stop writes to +0x08 | 98 |

So startTransfer() now gets its two edges, DMA runs, and every sample in
the buffer it reads is zero. The DMA reads guest RAM, so those zeros are
what the guest put in its output buffer.

Each ringtone tap in the same report logs AppleAMC_r1 assertions (lines 350,
565, 726, 2097). The AMC kext's own tables are named `aacd_*` and `mp3d_*`:
it is the hardware AAC/MP3 decoder. Ringtones are AAC. With the DSP
unmodelled, the hardware decode fails and nothing is mixed into the output
buffer. The PCM engine keeps running and plays silence.

**Change:** the app now hides `/arm-io/amc` by default (switch "amc", Guest
hardware). AppleAMC_r1 then never matches, and the guest has no hardware
decoder to pick.

**Expected, not yet observed:** iPhone OS 3 decodes AAC and MP3 in software
when no hardware decoder is available, so ringtones should reach the PCM
path. The next report settles it. If the samples are still all zero with the
AMC hidden, the zeros come from somewhere else.

bootkernel has the same switch (`--no-amc`) but leaves the AMC matched by
default, so the ~60 recorded desktop runs keep their meaning.

**Two limits of the underrun counter:**

- The clock keeps running while the audio engine is stopped (stop() writes
  +0x08 = 0). So "underrun" also counts idle time, and 912,940 bytes is not
  a measure of gaps during playback.
- Guest time ran at 0.77x wall time in this report. The app plays at 44.1 kHz
  of wall time, so it will underrun whenever the guest runs slower than real
  time. That is a speed limit, not an audio-model one.

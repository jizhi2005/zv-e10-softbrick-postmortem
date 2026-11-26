# 求助 / Help Wanted

## TL;DR

Sony ZV-E10 (gen 1, ILCE-ZV-E10, SoC CXD90045) is **soft-bricked**:
`/etc/ld.so.preload` points at a crashing library, so every `execve` fails, `init`
never starts, and the camera reboots every ~2.7 s.

**The camera is not damaged.** Kernel, USB, HID, partitions all work. The fix is to
make `/etc/ld.so.preload` empty — **one byte** — or to create a 2-byte file
`/setting/sen/smode` containing `UU` to bring up the USB service interface.

**What is missing is a single write access.** Every external entry point we could
reach has been tested and excluded (see below).

---

## The one question we cannot answer

Upstream projects claim you can enter the **BootROM serial boot** of a ZV-E10 through
**USB-C pin A7 (D−)**, using a Pico/RP2040 driving an open-drain 48000 8O1 UART with a
250 µs slow-block preamble:

- [`cokeenet/zve10-qemu`](https://github.com/cokeenet/zve10-qemu) —
  `fastboot/docs/usb-success.md` records a real success on 2026-10-22:
  injection of `fastboot.bin` produced `18d1:4ee0` and `getvar:version -> OKAY 0.4`
- [`dino2048/zv-e10-toolkit`](https://github.com/dino2048/zv-e10-toolkit) —
  `fastboot/docs/hardware.zh-CN.md` gives the wiring:
  `GP28 → D−(A7)`, `GP27 → 470Ω → D+(A6)`, `CC1/CC2 → 5.1k → GND`, `5V → VBUS`, `GND`

**We reproduced that setup exactly and it does not work.** The camera produces **no
response at all** — not even the BootROM's own `0x100D` parity error (13 short flashes),
which is a self-report that requires no protocol, no unlock token and no CRC.

---

## What we have already verified (please do not re-suggest these)

### Electrical path — measured, not assumed

| Item | Result |
|---|---|
| A7 (D−) drive | open-drain; camera-side pull-up measured **1.73 V**; clean high/low edges into the camera |
| A6 (D+) reset | pulling it low while the camera runs **stops the boot** ⇒ it really is `XRESET_REQ`; low level 0.165 V (legal) |
| Series resistor on D+ | 470 Ω (upstream value) |
| CC | 5.1k to GND |
| VBUS | **5.12 V** (measured with a calibrated meter — the first meter was 21 % high) |
| GND | common |
| Labels on the adapter | verified not swapped (driving the other wire kills the boot = that wire is reset) |

### Waveform — measured at the pad with the camera connected as load

Captured by sampling the RK3588 GPIO `EXT_PORT` register at ~40 ns
(see [`tools/edgecap.c`](../tools/edgecap.c)):

| Item | Expected | Measured |
|---|---|---|
| preamble blocks | 60 (2 + 29×2) | **60** |
| preamble block | 250.000 µs | 249.666 – 250.542 µs |
| carrier half-period | 125.000 µs | **125000.0 ns** |
| data bit | 20.833 µs | mean 20847 ns |
| first frame | start 0, byte 0x0A, odd parity, stop 1 | **all correct** |
| record bytes | `0A 00 00 00 00 00 DC 02 B1 8C …` | **decoded byte-for-byte off the wire** |

### Excluded recovery channels (all tested)

| Channel | Result |
|---|---|
| BootROM serial injection | ❌ correct waveform, zero response, no `0x100D` |
| Loader UART (`ldr_drv.bin`) | ❌ disassembly: only `0xF2910000`(GPIO2), `0xF2912000`(GPIO2 IOMUX), `0xF2403000` — the loader's UART is on GPIO2, not on USB-C |
| userland (shell/service mode/mass storage/WiFi/BT) | ❌ userland is the broken part |
| `ADJUST`/`BIS` forced modes | ❌ `upm.ko`: mode comes from the Loader via `Backup_read`; `is_usbj`/ADJUST flags are dead variables in the initramfs |
| camera key combinations (8 combos) | ❌ no USB identity change |
| SD-card updater (`MS Boot`, `BODYDATA.DAT`) | ❌ real 371 MB firmware on the card, camera behaviour unchanged |
| USB `054C:0994` | ❌ full descriptors dumped: **one IN interrupt endpoint only, no output/feature reports** — impossible to send it anything |
| HDMI CEC / hot shoe / mic / headphone | ❌ no path to the filesystem |
| direct eMMC / JTAG | ❌ BGA, requires soldering |

---

## What would actually help

Any one of these:

1. **A boardview / service-point photo for the ZV-E10 mainboard (Sony part `SY-1121`)**
   showing `BOOT_SERIAL_IN` (**BGA ball G18 / L19**), or the boot-mode straps
   `BOOT_EXT`(N8) / `BOOT_SIMPLE`(P4) / `BOOT MODE`(AL24), or `XRESET_REQ`(L11)
2. **The exact physical connection used in the successful runs** —
   which two pins, where GND was taken, and what state the camera was in
3. **A clear answer: does the ZV-E10 gen 1 support BootROM serial boot at all?**
   (The a6400 service manual — same CXD90045 SoC — shows `BOOT_SERIAL_IN` as a
   dedicated BGA ball, which is not on the USB-C connector.)
4. Or: **any known way to make the loader write 1–2 bytes** to `nflasha2` or `nflasha10`

Please open an issue. Every suggestion will be tested and the result published here,
including the negative ones.

---

## 中文摘要

**相机没坏，是用户态软砖。** `/etc/ld.so.preload` 指向一个崩溃的库 ⇒ 每次 `execve` 失败
⇒ `init` 起不来 ⇒ 约 2.7 秒一轮重启。修复只需让它变空（1 字节），或建 2 字节的
`/setting/sen/smode`（内容 `UU`）来拉起 USB 服务接口。

**缺的只是一次写入。** 所有能从外部触达的入口都实测排除了（见上表）。

**唯一无法回答的问题**：上游声称通过 USB-C 的 **A7(D−)** 能打进 BootROM 串口启动。
我们按完全相同的接线、波形、记录流复现，相机连 `0x100D`（13 短闪）都不报——
**说明 ROM 根本没在听这根线**。

**需要的帮助**：ZV-E10 主板（`SY-1121`）的点位图/测试点照片；或者上游那次成功所用的
**确切物理接法**；或者一句明确的"ZV-E10 一代到底支不支持 BootROM 串口启动"。

欢迎开 issue，每条线索我都会实测并把结果（包括失败的）公开在这里。

# 在你动手救 ZV-E10 之前，先花 10 分钟看这个

> **English summary at the bottom.**
>
> 这份文档的目的不是"求助"，而是**让别人别再走我们走过的弯路**。
> 我们在这台机器上花了三周，其中**大约有十天是白花的**——不是因为难，而是因为
> 一开始就该做的测量没做、一开始就该用的判据没用、一开始就被上游文档带偏了。
>
> 如果你手上也有一台变砖的 ZV-E10（或任何 Type-C-only 的 Sony 机身），按下面的顺序做，
> **第一天就能得到我们三周后才得到的结论。**

---

## 一、10 分钟分诊流程

### 第 0 步：先分清是"软砖"还是"硬砖"（2 分钟）

相机接电脑，看 USB 身份：

| USB 身份 | 含义 | 该做什么 |
|---|---|---|
| `054C:0994` 出现 | **软砖**。内核/USB 活着，只是用户态坏了 | 继续往下 |
| 完全没有 | 可能硬砖 / 供电问题 | 先查电池与 VBUS，再看本文意义不大 |
| `054C:0D95` | 大容量模式，用户态活着 | 不用救砖，直接找 `/etc/ld.so.preload` |

**同时观察指示灯**：约 2.66 秒一轮闪 = 在循环启动；灯灭 + 充电灯常亮 = 已停在充电稳态（SoC 不再启动）。

> ⚠️ **这一条是血的教训**：我们曾长时间对着"已停在充电稳态"的相机注入，**完全无效**。
> 灯不闪的时候，注入等于对着不启动的机器喊话。

### 第 1 步：校准你的万用表（1 分钟）

**先量一个已知电压**——ESP32 的 `3V3` 脚（应 3.30 V）、或一节新干电池（应 1.50 V）。

我们那块表电压档**偏高 21%**（3V3 读成 4.0 V），差点把正常的 5.12 V VBUS 误判成 6.2 V
超压，为此白折腾了一轮"电源假设"。**电阻档是准的，电压档不一定。**

### 第 2 步：先量线，再写代码（3 分钟）

相机通电、不驱动任何脚，量：

| 测点 | 期望 | 说明 |
|---|---|---|
| A9 (VBUS) 对 A1 | ~5 V | 供电 |
| **A7 (D−) 对 A1** | **~1.8 V** | **有上拉 ⇒ 线上接的是活电路**（开漏方案的前提） |
| A6 (D+) 对 A1 | ~1.8 V | 复位线，同样有上拉 |

**如果 A7 量到 0 V，先怀疑接线/接触，不要急着调波形。**

### 第 3 步：用"不依赖协议"的判据检验通路（4 分钟）

**这是全篇最重要的一条：**

> 往 A7 上打一段 **故意无法解析的垃圾方波**（233 µs 高 / 233 µs 低，约 1786 baud），
> 然后冷启动相机（拔电池再装，或 OFF→ON）。
>
> **BootROM 只要在听，就必然报 `0x100D` = 13 次短闪**（奇偶错）。
> 这是它自己的自报错误码——不需要 unlock、不需要 CRC、不需要协议格式。

- **看到 13 短闪** → 通路成立，格式问题可以慢慢调，你有希望了
- **看不到** → **ROM 没在这根线上听**。这时候调波形、调记录格式、调 unlock token
  **全部是浪费时间**，直接跳到第二节

### 第 4 步：只有第 3 步成功，才值得做后面的

---

## 二、八个"别这样"（每条都是我们踩过的坑）

**1. ❌ 别把"灯有变化"当成"信号进了 ROM"**

我们曾看到"载波一上线，相机就不再闪灯"，把它当成通路成立的证据，于是三周都在优化一个
**本来就已经正确**的波形。后来才想清楚：那个变化是因为载波打到了相机的 **USB PHY**，
让它反复进入总线异常状态，**跟 BootROM 没有关系**。

**判据只能是 13 短闪。**

**2. ❌ 别一次注入 600 秒**

相机循环约 **30 轮（~80 秒）就会停下**变成纯充电。600 秒里**后 520 秒全是无效时间**。
正确做法：**每次真的断电重来，只在它闪灯的 80 秒里注入**。

**3. ❌ 别用推挽驱动 D−**

上游明确要求 **OPEN_DRAIN**（借相机侧上拉）。相机侧电平约 1.7~1.8 V，
推挽会往 1.8 V 输入里硬灌，既不合法也有风险。

**4. ❌ 别相信"上游能行"，要先自己实测一个否定判据**

我们按上游文档（A7/A6/470Ω/5.1k/5V）逐项复现，波形实测完全正确，**零反应**。
上游确实有过成功记录，但那台机器的板子内部走线无法从外部验证。
**先花 10 分钟做第 3 步，比读三天文档有用。**

**5. ❌ 别在 USB `054C:0994` 上找命令通道**

我们 dump 了它的全部描述符：**只有 1 个 IN 中断端点，没有 output / feature 报告**。
协议层面它**从不接收主机数据**。别再猜"发个特殊 HID 包会不会有反应"。

**6. ❌ 别移植波形却漏掉 PIO 的隐藏周期**

PIO 里 `set pindirs,0 [22]` 是 23 周期，但后面还有 `jmp y--` 1 周期 ⇒ **共 24 周期 = 250.0 µs**。
我们见过的 ESP32 移植版写成了 `23 * CYC = 239.6 µs`，**60 个前导边沿全长错 4.2%**。
记录间隔也常被写成 2 ms（上游是 14.4 ms）。

**移植时逐项对数：前导块数、块时长、载波半周期、位周期、首帧字节。**

**7. ❌ 别指望 SD 卡升级**

ZV-E10 **一代没有记忆卡版固件**（索尼官网明写"不提供记忆卡版本"，那是 M2 和 α6700 才有的）。
我们实测：把真的 371 MB `BODYDATA.DAT` 放卡根目录、插卡开机，**行为零变化**。

**8. ❌ 别信本地文档里"板上没有某个焊盘"这种话**

我们的接线文档写着"公头板上没有 B5/B6/B7 焊盘"，但实物照片上**明明有**。
**接线前自己看实物丝印，别信二手描述。**

---

## 三、真的有用的工具与资料

### 工具

| 工具 | 为什么有用 |
|---|---|
| **一根杜邦线做 GPIO 回环** | 没有示波器时，一个脚发、另一个脚收，用软件打纳秒时间戳——**能完整验证波形和协议帧**。见 `tools/edgecap.c` |
| **万用表（先校准）** | 判断"线上有没有活电路"的最快手段 |
| **RK3588 / 树莓派当 USB 主机** | 读 `dmesg` 能拿到**毫秒级**的 USB 枚举时序，比在 Windows 上轮询强得多 |
| **capstone / objdump** | 反汇编 `upm.ko`、`ldr_drv.bin`、initramfs 的 `init`——**修复路径就是从这些里面挖出来的** |

### 资料

- **a6400 官方维修手册**（同一颗 CXD90045，含电路图和 BGA 引脚表）：
  `https://photo-parts.com.ua/parts/IMG_STORE/Sony%20A6400%20PCB/ILCE6400_serviceManual_FULL.pdf`
  - `BOOT_SERIAL_IN` = BGA **G18 / L19**
  - `BOOT_EXT` = **N8**、`BOOT_SIMPLE` = **P4**、`BOOT MODE` = **AL24**
  - `XRESET_REQ` = **L11**、`XSYS_RST` = **A8**
  - **这告诉我们：BootROM 的串口入口是 SoC 的专用球，不在 USB-C 上。**
- [cokeenet/zve10-qemu](https://github.com/cokeenet/zve10-qemu)、[dino2048/zv-e10-toolkit](https://github.com/dino2048/zv-e10-toolkit)、[ILCE-7M4-RE](https://github.com/DavidBuchanan314/ILCE-7M4-RE)

---

## 四、如果第 3 步就失败了，结论是什么

**别继续调软件。** 你能做的事只有：

1. 找到 **ZV-E10 主板（SY-1121）的点位图**，接 SoC 的 `BOOT_SERIAL_IN` + 启动模式 strap
2. 找有索尼工厂工具（"基板制作"）的维修点
3. 拆焊 eMMC 用编程器（**风险最高，不建议**）

**而"相机没坏、只差一个字节"这个判断，可以让你在任何时候把这些工作交给别人做——
不要因为"它看起来彻底死了"就放弃或拆掉。**

---

## English summary

**Before you spend weeks on a bricked Sony ZV-E10:**

1. **Classify first.** If it enumerates `054C:0994`, it is a *soft brick* — kernel, USB and
   storage are fine, only userland is broken. The fix is one byte in `/etc/ld.so.preload`.
2. **Calibrate your multimeter** against a known voltage first. Ours read 21 % high.
3. **Measure before coding.** A7 (D−) must idle at ~1.8 V (camera-side pull-up).
4. **Use a protocol-independent criterion:** send deliberately unparseable square wave
   (233 µs high/low) and cold-boot. A listening BootROM *must* report `0x100D`
   (13 short LED flashes). **No flashes = the ROM is not listening on that wire** — stop
   tuning the waveform, records and unlock token.
5. **Traps that cost us weeks:** "the LED changed" does not mean the ROM received
   anything; a 600 s injection wastes ~520 s because the camera stops booting after
   ~80 s; never drive D− push-pull; `054C:0994` has an IN-only interrupt endpoint so it
   cannot carry commands; SD-card firmware update does not exist for the gen 1.
6. **The one fact that matters:** according to the a6400 service manual (same CXD90045 SoC),
   `BOOT_SERIAL_IN` is a dedicated BGA ball (**G18/L19**) — it is not on the USB-C
   connector. Physical access to SoC test points is required.
7. **The camera is not destroyed.** Keep it, and the one-byte fix stays available to
   whoever can reach the storage.

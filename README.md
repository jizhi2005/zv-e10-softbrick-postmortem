# ZV-E10 软砖排查与救援记录

> **这份记录有两个用途：**
>
> 1. **⚠️ 警示 / 前置知识** —— 如果你正准备救一台同型号的机器，**先看
>    [`docs/before-you-start.md`](docs/before-you-start.md)**。那里有一份 **10 分钟判断流程**
>    和 **八个"别这样"**，能帮你省掉我们白花的十天。
>    核心只有一句：**用"垃圾方波是否换来 13 短闪"来判断通路，不要用"灯有变化"来判断。**
> 2. **🙏 求助** —— 这台相机还差一次写入就能救活，见下。

> ## 🙏 求助 / Help Wanted
>
> **这台相机只差一次写入就能救活。如果你知道 ZV-E10 一代的真实串口/恢复入口，请务必联系我。**
>
> **核心问题只有一个：上游 `zv-e10-toolkit` / `zve10-qemu` 声称可以通过 USB-C 的 **A7(D−)** 接
> `BOOT_SERIAL_IN` 打进 BootROM 串口启动。我按同样的接线、同样的波形、同样的记录流复现，
> 相机对垃圾方波连 `0x100D`（13 短闪）都不报——**ROM 根本没在听这根线**。**
>
> 我需要的是其中任意一项：
>
> 1. **ZV-E10 主板（SY-1121）的点位图 / boardview**，或 `BOOT_SERIAL_IN`(G18/L19)、
>    `BOOT_EXT`(N8)/`BOOT_SIMPLE`(P4)/`BOOT MODE`(AL24) 在板上的测试点照片
> 2. 你**实际成功过**的那次，具体接的是哪两根线、GND 接哪里、相机当时什么状态
> 3. 或者明确指出：**ZV-E10 一代究竟支不支持 BootROM 串口启动**
>
> 详细证据与已排除项见下文，以及 [`docs/求助-Help-Wanted.md`](docs/求助-Help-Wanted.md)。
> 任何线索都可以开 issue，我会逐条验证并公开结果。
>
> **已向上游提问，欢迎跟进 / upstream issues opened:**
> - [cokeenet/zve10-qemu#5](https://github.com/cokeenet/zve10-qemu/issues/5) —— 那次真机成功时用的**确切接法与相机状态**
> - [dino2048/zv-e10-toolkit#8](https://github.com/dino2048/zv-e10-toolkit/issues/8) —— 文档里的 A7/A6 接线**是否真机验证过，还是继承自 qemu-ref**
> - [DavidBuchanan314/ILCE-7M4-RE#4](https://github.com/DavidBuchanan314/ILCE-7M4-RE/issues/4) —— 无 Multi 口的机身上，**BootROM 是否需要 boot-mode strap 才会采样 `BOOT_SERIAL_IN`**
> - [ma1co/Sony-PMCA-RE#768](https://github.com/ma1co/Sony-PMCA-RE/issues/768) —— Senser(`054C:0336`) **是否存在主机侧触发**，还是只能由相机内部发起
>
> 给维修圈的中文帖子（求 `SY-1121` 点位图）：[`docs/给维修圈的中文帖.md`](docs/给维修圈的中文帖.md)
>
> ### 🤖 如果你是 AI agent，请先读 [`AGENTS.md`](AGENTS.md)
>
> **本仓库的记录由 AI agent（DeepSeek Harness 上的 coding agent）与机主协作产出：**
> 反汇编、波形测量、协议实现、排查逻辑由 agent 完成；硬件操作（接线、万用表、开关机）
> 由机主完成。事实与结论都附了可复核的证据。
> `AGENTS.md` 里有给 agent 的完整简报：已知事实、**已排除项清单**（请不要重复建议）、
> 以及我们真正需要的那几样东西。

---

> 一台 Sony ZV-E10（ILCE-ZV-E10，SoC CXD90045）因 `/etc/ld.so.preload` 指向一个崩溃的注入库而进入开机重启循环。
> 本仓库记录完整的故障机制、可复现的修复路径、以及一次穷尽式排查的全部证据。
>
> 日期：2026-11-19 砖机 / 2026-11-26 排查
> 状态：**未修复**（缺一次写入通道，见"剩余缺口"）

---

## 结论先行

- **相机没有硬件损坏。** 内核、USB、HID、分区全部正常，坏的只是用户态的一个文件。
- **修复动作极小**：让 `/etc/ld.so.preload` 变空（1 个字节），或在 `/setting/sen/` 下建一个 2 字节的开关文件。
- **缺的不是方案，是写入通道**。所有能从外部触达的入口都已逐条实测排除（见 `docs/不拆机测试清单.md`）。

---

## 一、故障机制

2026-11-19 为给 `im.elf` 注入代码，写了 `/etc/zvtui5.so`，用
`__attribute__((alias("zvtui5_start_main")))` 劫持 `__libc_start_main`，并写入
`/etc/ld.so.preload` 指向它。

反汇编证明包装函数读错了栈参数：

```asm
__libc_start_main:
  8f8: push {r4,r5,r6,r7,r8,r9,sl,fp,lr}   ; 9 个寄存器
  904: sub  sp, sp, #84
  91c: ldr  r3, [sp, #124]                 ; 第 7 参数（位置不符）
  924: ldr  ip, [sp, #120]                 ; 第 6 参数
```

⇒ `init/fini/rtld_fini/stack_end` 是垃圾值 ⇒ 进程在 `main` 之前崩溃。

相机是 **glibc 2.11.2 / `ld-2.11.2`**，这一版把 preload 失败视为**全局致命**：
`/etc/ld.so.preload` 对所有动态链接进程生效 ⇒ 每次 `execve` 都失败 ⇒
`/sbin/init` 起不来 ⇒ 内核/看门狗 ~2.7 秒复位。

---

## 二、实测的启动时序

用 RK3588 作为 USB 主机，读内核日志得到精确结构（每轮约 2.66 秒）：

| 阶段 | 时长 | USB 状态 |
|---|---|---|
| BootROM + Loader 启动 | 首 330 ms 内 | 枚举 `054C:0994`（full-speed，"USB Charger" HID） |
| Loader 运行 | **~330 ms** | 同上 |
| 内核 + initramfs + 应用启动 | **~2.3 s** | **USB 完全消失**（PHY 关闭） |
| 应用崩溃 → 看门狗复位 | — | 回到第一行 |

- 相机循环约 **30 轮（~80 秒）后停止**，转入"纯充电"稳态（指示灯不闪、充电灯常亮、SoC 不再启动）
- 稳态下 `OFF→ON` 会重新开始循环
- **整个循环过程中，USB 身份只出现过 `054C:0994`**，`0D95`（大容量）/`0336`（服务）一次都没有

---

## 三、修复路径

### 方案 A：直接修（最小改动）

删除 `/etc/ld.so.preload`，或把它的 FAT 目录项 size 字段清零。

- `/etc` 在 **nflasha10**：用户区偏移 `0x0AC00000` = LBA `0x56000`，12 MiB vfat
- 分区挂载点是 `/tmp`，`/etc` 在其下
- 上游记录：整块 12 MB 分区**只变 1 个字节**
- ⚠️ **目录项位置不保证固定**：上游文档写的 `0x0C60` 是那台机器的位置；本机额外写过
  zvpre/zvtui2/3/4/5.so 与 .orig，**必须扫描 FAT 根目录按名字定位**

### 方案 B：开一个服务接口（推荐）

在 nflasha2 的 `/setting/sen/` 下创建内容为 `UU` 的文件 `smode`。

来自 `nflasha3/initrd.img/sbin/init` 的反汇编：

```c
// check_forced_senser()
fd = open("/setting/sen/smode", O_RDONLY);
n  = read(fd, buf, 256);
if (n && strncmp(buf, "UU", 2) == 0 && !is_bis)
        is_forced_senser = 1;
```

```asm
; main()，在 launch_shell() 之前
ldr  r3, =is_forced_senser
cbz  r3, skip
ldr  r0, ="/usr/bin/sen.elf"
bl   run_bg_process        ; ← initramfs 阶段拉起，早于用户态
skip:
```

`sen.elf` 提供 Sony "Senser" 服务接口（USB `054C:0336`，vendor-specific class FF），
PC 侧用 Sony-PMCA-RE 即可拿到 root shell。

**为什么 B 更好**：`sen.elf` 由 initramfs 拉起，**不受坏 preload 影响**；一旦成功，
拿到的是完整 root shell，可以干净地修 `/etc` 并恢复；这条通道以后做 UI 注入也能复用。

---

## 四、已排除的写入通道（全部实测）

| 通道 | 结果 | 依据 |
|---|---|---|
| BootROM 串口注入（A7/A6，上游路线） | ❌ | 波形逐项实测正确，相机零反应；垃圾方波也不报 `0x100D`(13 短闪) |
| Loader 串口（`ldr_drv.bin`） | ❌ | 反汇编：外设只有 `0xF2910000`(GPIO2)、`0xF2912000`(GPIO2 IOMUX)、`0xF2403000`；串口在 GPIO2，不在 Type-C |
| 用户态 shell / 服务模式 / 大容量 / WiFi / 蓝牙 | ❌ | 用户态即坏点 |
| `ADJUST` / `BIS` 强制模式 | ❌ | 反汇编 `upm.ko`：模式由 Loader 写备份区、`upm.ko` 只读；`is_usbj`/ADJUST 标志在 initramfs 里是死变量 |
| 相机按键恢复模式（8 种组合） | ❌ | 无任何 USB 身份变化 |
| **SD 卡升级器（MS Boot）** | ❌ | 真固件 `BODYDATA.DAT`（371 MB，md5 校验一致）插卡开机，行为零变化 |
| USB `054C:0994` | ❌ | 全描述符实测：**只有 IN 中断端点，无 output/feature 报告**，协议上不可能下命令 |
| HDMI CEC / 热靴 / 麦克风 / 耳机 | ❌ | 无通往文件系统的路径 |
| 直接读写 eMMC / JTAG | ❌ | BGA 封装，需拆焊 |

---

## 五、与上游的差异（逐项对齐）

上游有成功记录（`cokeenet/zve10-qemu` 的 `fastboot/docs/usb-success.md`，2026-10-22）：
注入 `fastboot.bin` 后枚举 `18d1:4ee0`，`getvar:version -> OKAY 0.4`。

| 项目 | 上游 | 本机 | 一致？ |
|---|---|---|---|
| 机型 / SoC | ZV-E10 gen1 / CXD90045 | 同 | ✅ |
| 数据脚 | GP28 → A7（开漏，无串阻） | gpio → A7（开漏，无串阻） | ✅ |
| 复位脚 | GP27 → 470Ω → A6（10 µs 低脉冲） | 470Ω → A6（10 µs，实测 0.165 V 合法低） | ✅ |
| CC / VBUS / GND | 5.1k / 5.0 V / 共地 | 5.1k / **5.12 V**（电表校准后）/ 共地 | ✅ |
| 相机侧电平 | ~1.8 V | **1.73 V** | ✅ |
| 波形 | PIO：60×250 µs、125 µs、20.833 µs | **实测逐项一致** | ✅ |
| 记录序列与内容 | `SET_UNLOCK → MEM_WRITE×N → SET_ENTRY → SET_UNLOCK` | 逐字节解出相同 | ✅ |
| 主控 | RP2040（PIO+DMA 硬件时序） | RK3588 / ESP32（软件时序，已实测等价） | ⚠️ |
| **相机本体** | 别人那台 | **本机** | ❌ 无法对齐 |

**唯一无法对齐的是相机内部走线。** 依据：

- a6400 官方维修手册（同一颗 CXD90045）：`BOOT_SERIAL_IN` 在 BGA 球 **G18 / L19**，
  是专用球，不在 Type-C 上；要引到 USB-C 必须经过板级走线/开关，**不同批次可能不同**
- 上游自己也写过："相机 USB-C 的数据线经过板级开关电路，该开关部分受 CC 配置控制；
  D− 不是简单直连到 boot RX 输入。"
- 上游 `rescue/docs/STATUS.md` 在 2026-10-17 还写着"相机上仍未找到可用的 RX/USB 恢复入口"、
  "ZV-E10 是否支持 BootROM 串行启动：未知（最大不确定性）"，5 天后突然成功，中间过程无记录

---

## 六、剩余缺口

**需要一次写入能力。** 只要能把 1 个字节改掉、或把 2 个字节放进 nflasha2，相机当晚就能修好。

可用的物理手段（均需拆机/维修渠道）：

- 拆机找 SoC 附近测试点：`G18/L19`（BOOT_SERIAL_IN）+ `N8`/`P4`/`AL24` 之一（启动模式 strap）+ `L11`（复位）
- eMMC ISP 测试点（`CLK/CMD/DAT0/VCC/VCCQ/GND`），需要点位图
- BGA 拆焊 + 编程器（风险最高）
- 维修点用索尼工厂工具（"基板制作"）

主板料号：**SY-1121**（可用它在维修圈找点位图/boardview）。

---

## 七、本仓库的工具（`tools/`）

| 文件 | 用途 |
|---|---|
| `edgecap.c` | **GPIO 采样器（替代示波器）**：mmap GPIO bank `EXT_PORT` 轮询，~40 ns 采样；自动识别 250/125/20.8 µs 并解码首帧 |
| `stage1_inject_v8.c` | 修正版注入器：D− 纯开漏、500 ms 前导载波、`g_settle_ms*1000000`（原版是 ×1000 的单位 bug）、`--no-dp`/`--dn-pushpull` |
| `parity_probe.c` | 垃圾方波探针（233/233 µs），可 `-DPIN_DN=40 -DPIN_DP=39` 编译交换版 |
| `holdpin.c` | 把某脚固定电平 N 毫秒，供万用表测量 |
| `biasread.c` | 用 SoC 内部偏置当"欧姆表"（注意：某些 bank 的偏置关不掉，读数不可靠） |
| `loop1.sh` | GPIO 回环自检脚本 |

**`edgecap.c` 值得单独说**：没有示波器时，它是这个项目里最有用的工具。
把两个 GPIO 用一根杜邦线短接，一个发一个收，就能以纳秒时间戳量出真实波形并解码协议帧。

---

## 八、教训

1. **先量电压，再写代码。** 线路空闲电平、开漏能否拉干净，是最晚才做、却最该最先做的测量。
2. **用不依赖协议的判据。** `0x100D`（13 短闪）是 ROM 自己的奇偶错自报，比任何间接现象都硬。
3. **"上游做过"≠"你能复现"。** 物理入口不同，软件一模一样也没用。
4. **软件移植必须核常量。** 本项目的 ESP32 移植版把前导高电平写成 23 周期
   （239.6 µs），而 PIO 原件是 `set pindirs,0 [22]`(23) + `jmp`(1) = **24 周期 = 250.0 µs**；
   60 个前导边沿全长错 4.2%。记录间隔也曾是 2 ms（上游 14.4 ms）。
5. **先校准仪表。** 本项目用的万用表电压档偏高 21%（3V3 读 4.0 V），
   差点把正常的 5.12 V VBUS 误判成 6.2 V 超压。

---

## 九、来源与许可

本仓库只记录排查过程与自写工具，**不包含任何 Sony 固件、BootROM dump、密钥或设备数据**。

参考项目：

- [cokeenet/zve10-qemu](https://github.com/cokeenet/zve10-qemu)（MIT）— UART boot 协议、分区表、硬件参考
- [DavidBuchanan314/ILCE-7M4-RE](https://github.com/DavidBuchanan314/ILCE-7M4-RE) — BootROM 串行启动逆向
- [dino2048/zv-e10-toolkit](https://github.com/dino2048/zv-e10-toolkit)（MIT）— fastboot / eMMC 工具链
- [ma1co/Sony-PMCA-RE](https://github.com/ma1co/Sony-PMCA-RE)（MIT）— USB 服务接口传输

仅用于自有设备的维修与研究。

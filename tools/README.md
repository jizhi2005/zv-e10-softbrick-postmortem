# tools/ — 构建与用法

这些工具在排查中真的用过，不是示例代码。全部是**只读或可逆**的：没有一条会写相机的存储。

## 构建（在 WSL 或任何带 aarch64 交叉编译器的 Linux 上）

目标板：RK3588（aarch64，Linux 5.10）。默认全部静态链接 libgcc、动态链接 libc：

```sh
aarch64-linux-gnu-gcc -O2 -static-libgcc -Wall -o edgecap        edgecap.c
aarch64-linux-gnu-gcc -O2 -static-libgcc -Wall -o holdpin        holdpin.c
aarch64-linux-gnu-gcc -O2 -static-libgcc -Wall -o biasread       biasread.c
aarch64-linux-gnu-gcc -O2 -static-libgcc -Wall -o parity_probe   parity_probe.c
aarch64-linux-gnu-gcc -O2 -static-libgcc -Wall -o stage1_inject_v8 stage1_inject_v8.c -lm
```

交换数据/复位脚（`PIN_DN`/`PIN_DP` 默认 39/40）：

```sh
aarch64-linux-gnu-gcc -O2 -DPIN_DN=40 -DPIN_DP=39 -o parity_probe_swap parity_probe.c
```

传上板子：`cat > /tmp/xxx.b64 << 'EOF'` + base64，或者任何你喜欢的方式。板子的 `/tmp` 是
tmpfs，**重启后工具会消失**。

## 各工具干什么

### `edgecap.c` — 最有用那个

没有示波器时，用它代替。

它不走内核的边沿事件接口（5.10 上 v2 事件不保证存在，而且 16 事件的缓冲在 48 kHz 下
很容易溢出），而是 **mmap GPIO bank 的 `EXT_PORT` 输入寄存器直接轮询**，采样间隔约
40 ns，用 `CLOCK_MONOTONIC` 给每个边沿打时间戳。

```sh
./edgecap --pin 40 --seconds 14 --max 3000 --window-us 20000 --raw 26
```

它会自动识别并统计这几档间隔，然后尝试解出第一帧：

```
前导块 250.0 us / 载波半周期 125.0 us / 数据位 20.833 us
首帧: start / 8 data bits (LSB first) / odd parity / stop
```

**回环自检的用法**：把两个 GPIO 用一根杜邦线短接，一个发一个收，就能在完全没有相机的
情况下验证波形与协议实现是否正确——这是排除"软件波形不对"最快的方法。

**接相机时的用法**：把数据脚接到相机，`--trigger` 会自动抓住 250 µs 前导块开始记录，
这样能测到**带负载的真实波形**（我们就是靠它发现前导块 249.666–250.542 µs、数据位均值
20847 ns 全部正确的）。

### `parity_probe.c` — 通路判据

把上游 `uart_boot_probe` 移植到 RK3588 GPIO。发**故意无法解析**的方波（233 µs 高/低，
约 1786 baud），然后冷启动相机。

**如果 BootROM 在听，它必然报 `0x100D`（1 长 13 短）**——这是它自己的奇偶错自报，
不需要 unlock token、不需要 CRC、不需要任何协议格式。

**看不到 13 短闪 = ROM 没在采样这根线**，这时候调波形和记录格式全是浪费时间。

```sh
./parity_probe --attempts 1 --wave-ms 180000 --no-reset --gap-us 233
```

### `stage1_inject_v8.c` — 修正版注入器

相对早期版本的修正（每一处都是实测发现的问题）：

| 修正 | 原因 |
|---|---|
| D− 改**纯开漏** | 上游要求；相机侧有 1.73 V 上拉，推挽是错的 |
| 前导载波 `2 ms → 500 ms` | 上游 `TRANSMIT_START` 要求 |
| `g_settle_ms * 1000 → * 1000000` | 原版把毫秒当微秒，600 ms 实际只有 0.6 ms |
| 记录间隔 `2 ms → 14.4 ms` | 上游 `--record-gap 0.0144` |
| 新增 `--no-dp` / `--dn-pushpull` | 回环自检、A/B 对照 |

```sh
./stage1_inject_v8 --payload /tmp/stage1.bin --seconds 600 \
    --record-gap 14400 --reset-us 10 --settle-ms 100
```

### `holdpin.c` / `biasread.c` — 万用表辅助

`holdpin` 把某个脚固定在指定电平 N 毫秒，方便你**一边拿万用表量、一边保持状态**。
`biasread` 用 SoC 内部上下拉当"欧姆表"（注意：某些 bank 的偏置关不掉，读数不可靠，
见 `docs/before-you-start.md` 的说明）。

### `loop1.sh` — 回环自检脚本

自动跑一遍"注入器驱动 → 采样器采集 → 比对前导/位周期/首帧"的完整流程。

### `watch_usb.sh` — 用 USB 身份替代指示灯

**这是 2026-11-27 新加的判据，也是最有价值的一个。**

注入 stage1 成功后，相机会作为 `ZVE10-fastboot` / `18d1:4ee0` 出现。
**指示灯可能根本不会正确反映 payload 状态**（payload 运行时相机处于"被接管"状态），
所以直接看 USB 身份比看灯可靠得多。

```sh
sh watch_usb.sh 300      # 监控 300 秒内 /sys/bus/usb 的每一次变化
```

用法：注入的同时把它跑起来，然后**立刻把 24pin 板换成普通 USB-C 数据线**
接到主机 USB-A 口上，看是否出现新设备。

### `pinreleaser.c` / `setbias.c` — 拆开"我们的上拉"和"相机的上拉"

**它们的用途是纠正一个我们犯过的错误**：`pin 39`/`pin 40` 的 pinctrl 一直是
`input bias pull up`，所以我们读到的每个"相机侧上拉"都混着自己的上拉。

- `pinreleaser <gpio> [secs]` —— 把引脚设成纯输入、无偏置，让相机那一侧单独暴露
- `setbias` —— GPIO v2 字符设备方式请求引脚并指定偏置（本内核上 ioctl 定义需核对，
  未调通，保留作参考）

**教训**：量"对方有没有上拉"之前，先确认自己的偏置是关的。

### `upload_one.py` / `rkcmd.py` — 串口传文件/跑命令

板子的 `/tmp` 是 tmpfs，重启就清空。这两个脚本用 1500000 波特的调试串口
（CH343，COM11）传二进制上去或跑命令，不依赖 adb 或 USB gadget。

```sh
python upload_one.py stage1.bin /tmp/_dyn/stage1.bin
python rkcmd.py "ps | grep parity_probe"
```

## 已知限制

- 采样使用 CPU 忙轮询 + `SCHED_FIFO`；内核默认会在连续 RT 运行 950 ms 后节流，
  `edgecap` 会自己把 `/proc/sys/kernel/sched_rt_runtime_us` 写成 `-1` 来关掉
- 需要 root（`/dev/mem`、`/dev/gpiochip*`）
- 引脚编号写死为 RK3588 的 gpio39/gpio40（JP5 `P1_A7` / `P1_B0`），换板子要改

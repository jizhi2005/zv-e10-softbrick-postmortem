# 已向上游提出的问题（2026-11-26）

> 记录已经问过什么、问的是谁、为什么问他——避免重复提问，也方便后来人接着跟。

| # | 仓库 | 问的那一句 | 为什么只能他答 | Issue |
|---|---|---|---|---|
| 1 | `cokeenet/zve10-qemu` | 那次真机成功时，**A7/A6 是否就是最终接法**？GND 接哪里？相机什么状态？是否需要 BOOT strap？ | 仓库里的 `fastboot/docs/usb-success.md` 有**同型号真机成功记录**（`18d1:4ee0`、`getvar:version -> OKAY 0.4`），是唯一的第一手证据 | [#5](https://github.com/cokeenet/zve10-qemu/issues/5) |
| 2 | `dino2048/zv-e10-toolkit` | 文档里的 GP28→A7 / GP27→A6 接线**是真机验证过的，还是继承自 qemu-ref**？ | `fastboot/docs/hardware.zh-CN.md` 是这套接线的来源文档 | [#8](https://github.com/dino2048/zv-e10-toolkit/issues/8) |
| 3 | `DavidBuchanan314/ILCE-7M4-RE` | 在没有 Multi 口的机身上，**BootROM 是否需要某个 boot-mode strap 置位**才会采样 `BOOT_SERIAL_IN`？ | 串口协议的原作者；他的 A7 IV 用 Multi 口现成引出三个信号，无需 strap | [#4](https://github.com/DavidBuchanan314/ILCE-7M4-RE/issues/4) |
| 4 | `ma1co/Sony-PMCA-RE` | Senser (`054C:0336`) **是否存在主机侧触发**，还是只能由相机内部（用户态/initramfs）发起？ | Senser 服务协议（`StartSenser`）的实现者 | [#768](https://github.com/ma1co/Sony-PMCA-RE/issues/768) |

---

## 提问时的做法（供参考）

1. **一个 issue 只问一个问题** —— 不群发求助，不把多个诉求塞进一条
2. **先说感谢，再说事实** —— 开头承认对方的工作是基础
3. **摆出自己已经做的实测** —— 波形纳秒采样、电平测量、VBUS 校准值。
   这证明"我不是来让你 debug 我的设置的"，是让内行愿意回的最有效方式
4. **能贡献就先贡献** —— 给 PMCA-RE 那条是先交出一个反汇编发现
   （`check_forced_senser` 的行为），再问问题
5. **承诺公开结果** —— 包括失败的结论，这对开源作者是加分项

---

## 一个关键事实（提问时整理出来的）

那份真机成功记录属于 **`qn06142`** 那台相机（该项目的用户，其文档提到有 USB-C 全针引出板
和示波器）。

**我们这台是另一台相机** —— 同型号、不同个体。

所以"上游能行、我们不行"**不构成矛盾**：差别完全可能在这两台机器的内部走线上，
而那段走线只有拆机或点位图才能看到。这也正是我们需要 `SY-1121` 点位图的原因。

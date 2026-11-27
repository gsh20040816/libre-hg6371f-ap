# HG6371F 用户态闭源工具全面开源替换实施计划

> 状态：**M0/M1 启动执行中**（2026-11-25）  
> 目标设备：烽火 HG6371F（Broadcom BCM68252C，ARMv7 Cortex-A7，Linux 4.19.235，纯净 Alpine 3.21.10 `musl + OpenRC`）

## 1. 目标与总体架构

彻底移除当前纯净 Alpine rootfs 中残留在 `/opt/bcm/` 下的 **全部 8 个博通闭源用户态工具 + 21 个 glibc 2.32 softfp 动态库 + `/lib/ld-linux.so.3` + `/etc/get_rootfs_dev.sh`**（未压缩共 `3.7 MiB`），采用 **纯 C 静态库 + 多合一纯 C CLI + Go `cgo` 零 fork 直连** 架构：

```text
bcmopen/                        ← 纯 C99 开源底层库与多合一 CLI
  include/
    bcmopen.h                   ← 统一对外结构体 API（CLI 与 Go cgo 唯一契约）
  lib/                          ← 编译为静态库 build/libbcmopen.a（零外部依赖，纯 musl）
    wdt.c                       ← /dev/watchdog 硬件看门狗
    fcache.c                    ← /dev/fcache FlowCache 裸 ioctl (0x64..0x8b)
    nvram.c                     ← AF_NETLINK proto=31 内核 NVRAM + 用户态文本事务层
    bootstate.c                 ← pureUBI metadata1/2 (UBI_IOCVOLUP) + /dev/brcmboard ONCE
    flashutil.c                 ← bcm_flashutil CLI 复用层
    ethsw.c                     ← SIOCETHSWCTLOPS 交换芯片控制
    bdmf.c                      ← /dev/bdmf_shell + /dev/rdpa_user 会话与命令通道
    wl_core.c                   ← SIOCDEVPRIVATE (0x89f0) + 通用 IOVAR/dump + 15 个复用模板
    wl_*.c                      ← T1–T3 全部 334 条显式 wl 命令（238 个独立处理函数）
  cli/
    main.c                      ← 多合一极薄 CLI（build/bcmctl，按 argv[0] 分发）
  tools/
    re_disasm.py                ← 自动化 ARM32 反汇编 + 字面量池/字符串/ioctl 提取工具
    ap_diff.py                  ← 自动化实机只读命令「原厂 vs 开源」逐字节回归比对脚本
  tests/                        ← 本机主机 C 单元测试（mock ioctl/UBI + 实机 metadata1.bin CRC 验证）

panel/server/                   ← 保留现有 7,200 行零依赖 Go 管理面板
  bcm_cgo.go                    ← CGO_ENABLED=1 经 zig cc 静态链接 libbcmopen.a（零 fork、零正则）
  bcm_stub.go                   ← 非 Linux 宿主机编译桩
```

### 关键技术决策
1. **交叉编译工具链**：本机 macOS 直接使用 `zig cc -target arm-linux-musleabihf -mcpu=cortex_a7` 与 `zig ar`，同时产出静态 `libbcmopen.a`、静态 `bcmctl` 及 `CGO_ENABLED=1` 静态 `ap-panel-srv`，零 Docker 依赖、保持秒级构建体验。
2. **`bcm_bootstate` 写入方式**：直接通过 Linux UBI 内核接口先 `ioctl(fd, UBI_IOCVOLUP, &int64_len)` 再 `write(fd, buf, 1280)` 原子更新 `ubi0_1`/`ubi0_2`（零 fork、不写 `/var/mdata.tmp`、不调 `system("ubiupdatevol")`）。
3. **自动化移植流水线**：优先通过 `bcmopen/tools/` 自动化解析 ELF 命令表、反汇编标注字面量池/字符串/ioctl 号、生成回归对比用例；仅对高安全风险或复杂版本化结构体（`bootstate`、`csimon`、`sta_monitor`、`sta_info`/`counters`/`scanresults` 等）做手工精审。
4. **独立 Review 门禁**：每个里程碑完成后均需经独立 Reviewer 对照原厂二进制反汇编逐项核验，确认**零结构体偏移/ioctl 号/输出格式/退出码不一致**后方可放行。

---

## 2. 实机核验的内核 ABI 基准

以下结论全部经实机设备与原厂二进制反汇编交叉验证（避免盲信外部文档错误）：

| 模块 | 底层通道 | 实机验证结论 |
|---|---|---|
| `wdtctl` | `/dev/watchdog` | `WDIOC_SETPRETIMEOUT (0xC0045708, 3s)`、`WDIOC_SETTIMEOUT (0xC0045706, 4..600s, 默认 80)`、`WDIOC_SETOPTIONS (0x80045704, WDIOS_DISABLECARD=1)`；`ping`/守护写 1 字节 `0x00`；`timeleft` 读 `/sys/class/watchdog/*/timeleft`；`SIGKILL` 不发 `'V'`。 |
| `fcctl` | `/dev/fcache` | 裸整数 ioctl `0x64..0x8b`（非 `_IOC` 编码）；每次调用独立 `open/ioctl/close`；切勿盲读 `/proc/fcache/stats/*`（部分 proc 节点会导致 `pktflow.ko` 挂死）。 |
| `nvram` | `AF_NETLINK` proto `31` + 文件 | 内核态走 Netlink 31（`nlmsg_type=5`, `op=2/3/5/6`）；用户态文件 `/data/.user_nvram.setting`、`kernelset` 读 `/fhconf/data/.kernel_nvram.setting`（`/data` 为软链）。 |
| `bcm_bootstate` | `/dev/ubi0_{1,2}` + `/proc/device-tree/chosen/active_image` + `/proc/bootstate/reset_reason` + `/dev/brcmboard` (`0xc018423e`) | 卷长 `1280 B`，头部 `+0x00=0x100`、`+0x04=0x100`、`+0x08=CRC32(buf[0x0c:0x0c+252])`（实测 `backups/metadata1.bin` 与 `zlib.crc32` 逐位匹配 `0x5dcd9e8d`）；`/proc/mtd` 含 `mtd1: "loader"` 为 pureUBI 新布局；`UBI_IOCVOLUP` 必须先于 `write()` 调用；参数 `6↔'7'`、`7↔'6'` 交叉且 `0/999` 序号环绕。 |
| `bcm_flashutil` | 复用 `libbcm_flashutil` | 5.4 KB 薄壳 CLI，查询/操作闪存与镜像状态。 |
| `ethswctl` | `AF_INET` socket + `SIOCETHSWCTLOPS` | 交换芯片端口/VLAN/ARL/MIB/QoS/EEE 控制，结构体 `struct ethswctl_data`。 |
| `bdmf_shell` | `/dev/bdmf_shell` + `/dev/rdpa_user` | 会话初始化（`-c init`）与命令执行（`-c <id> -cmd ...`）。 |
| `wl` | `AF_INET` DGRAM + `SIOCDEVPRIVATE (0x89f0)` + `AF_PACKET (0x886c)` | 827 个显式 `cmd_t`（20 B）+ 通用整型 IOVAR 兜底（实机 `ampdu_rts`/`phy_rxdesens`/`phy_tempsense`/`atf` 等 10 个已用命令均走此兜底）+ 通用 `dump <section>`（内核直出文本）。 |

---

## 3. 里程碑规划（M0 – M4）

| 里程碑 | 范围与核心交付 | 验收门禁 |
|---|---|---|
| **M0 · 骨架与流水线** | 落盘本计划、`bcmopen/` 目录与 `Makefile`/`bcmopen.h`、自动化反汇编工具 `re_disasm.py` 与实机比对脚本 `ap_diff.py`、验证 `zig cc` 静态 C 与 Go `cgo` 交叉编译打通 | `make build-bcmopen` 产出零 NEEDED 静态 ARMv7 ELF；`make test-bcmopen` 通过 |
| **M1 · 前 7 个小工具全功能复刻** | `wdt.c`、`fcache.c`、`nvram.c`、`bootstate.c`、`flashutil.c`、`ethsw.c`、`bdmf.c`、`cli/main.c` | ① 主机单元测试全过（含真实 `metadata1.bin` CRC 与往返）；② 实机 `/tmp/bcmctl` 只读逐项 diff 原厂零差异；③ **独立 Review 确认零不一致** |
| **M2 · `wl` 引擎 + T1（37 命令）+ 119 查表命令 + Go `cgo` 接入 + 清零 `/opt/bcm/`** | `wl_core.c`、`wl_chanspec.c`、`wl_t1_*.c`、`panel/server/bcm_cgo.go`、更新 `build-alpine-rootfs.sh` 与 `Makefile` 移除 `/opt/bcm/` 与 `ld-linux.so.3` | ① T1 全部命令与原厂逐项比对一致；② 非活动槽 `--no-commit` 试跑验收通过（看门狗/双频 Wi-Fi/面板 6 页签零 fork/iperf3 1.28+ Gbps）；③ 独立 Review 通过后固化 |
| **M3 · `wl` T2 全部 8 个高价值模块** | 补齐 T2 剩余 ~90 个专属函数（客户端管理、11k/11v/11r、CSA/DFS、单终端深度观测、功率监管、Wi-Fi 6 HE/MU/OFDMA/SR/TAF、多 SSID/escan/monitor、事件监听） | 只读命令全量实机 diff 原厂；写入命令单测逐字节核对 ioctl payload；独立 Review 通过 |
| **M4 · `wl` T3 中低频运维/微调收官** | 补齐 T3 剩余 ~105 个专属函数（AMPDU/WMM、速率/扫描/PHY 诊断、雷达阈值/安全状态），完成 T1–T3 全部 334 条显式命令闭环 | 同 M3 全量回归 + 独立 Review 通过 |

---

## 4. 验收与安全铁律

1. **零动态依赖**：`llvm-readobj --dynamic-table build/bcmctl` 不得出现任何 `NEEDED` 项。
2. **输出与退出码等价**：自动化比对脚本 `ap_diff.py` 核对 stdout/stderr 关键字段与退出码一致。
3. **高危操作隔离**：任何涉及闪存/切槽/看门狗的改动先经主机单测 + 非活动槽 `--no-commit` 试跑，绝不在活动槽直接裸替换。
4. **独立 Review 门禁**：每完成一个里程碑，交由独立 Reviewer 对照原厂二进制反汇编逐一审计，发现任何 ABI/偏移/行为不一致必须立即修正并复测。

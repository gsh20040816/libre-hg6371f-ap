# 启动链校验与绕过分析（实测 + 静态逆向）

本文记录 HG6371F 启动链上**每一道校验**的实际行为、证据来源，以及绕过方案。
所有"已确认"结论都有实机实验或数学验证支撑；"推断"与"未验证"单独标注，不混在一起。

---

## 1. 总览

```
BootROM
  → SPL（mtd1 @0x0，base 0x82000000）          无密码学代码
  → TPL（mtd1 @0x8000C，base 0x05000000）     有校验代码，nonsec 下全部跳过
  → U-Boot（bootfs FIT，load 0x1000000）
       └─ image_decomp() 的 LZO 分支里有一段私有 RSA 验签  ← 真正的门槛
  → Linux 内核（同 FIT，LZO，load 0x108000）
       └─ do_mount_root() 验 rootfs（RSA-2048/SHA-256），失败就地改挂另一槽
  → rootfs（ubiblock0_4 / 0_6）
```

一句话：**loader 不验，U-Boot 验内核，内核验 rootfs。**

---

## 2. 四个实测实验（全部在真机上做过）

| 改动 | 结果 | 说明 |
|---|---|---|
| FIT 头部 DTB 的 `timestamp` 改 1 字节 | ✅ 正常启动 | 头部不受保护 |
| 内核 blob 重新压缩（解压后内容完全相同） | ❌ 被拒，回落另一槽 | 说明签名覆盖**压缩后**的字节 |
| `kernel_sig` 改 1 字节（内核数据不动） | ❌ 被拒，回落另一槽 | 说明它确实是校验用的签名 |
| `bootfs1` 原样重打包（逐字节一致） | ✅ 正常启动 | 打包链路无损 |
| U-Boot 版本号字符串改 1 字节 | ❌ 被拒，回落另一槽 | **loader 校验 U-Boot 组件本身** |
| 原厂 `bootfs2` 原样写入 Slot 1（对照） | ✅ 从 Slot 1 启动 | 一次性启动标志正常，排除误判 |

补充：三次失败/成功启动都只有**一条** `reboot_info` 记录，说明是加载阶段被拒，不是内核 panic 后复位。

---

## 3. 各层校验详情

### 3.1 loader / TPL（mtd1）

**结论：nonsec 下不校验任何组件。**

- `fw_printenv` 显示 `bootm_boot_mode=nonsec`，但这个字符串**只存在于 loader**，U-Boot 里没有。
- loader 里 `fit_header_auth()` 在 `sec_state == 0`（OTP 未烧）时直接 `return 0`，跳过 FIT 头 RSA。
- 逐组件 `hash-1` 校验由"策略指针"驱动，指针为空时打印 `Delegation not active` 后整体空转。
- loader 里**没有** `uboot_sig` / `U-Boot-Sig` / `signature-1` / `kernel_sig` 这些字符串 —— 它按通用 FIT 规则处理，不认识私有签名组件。
- SPL（mtd1 起始处）完全没有密码学代码。

**但实测推翻了这个推论。**

- 只改 U-Boot 里一个**完全无害的版本号字符串**（`U-Boot 2019.07-svn25369` → `xvn25369`），FIT 结构自洽、hash-1 正确 → **仍然回落另一槽**。
- 同一时刻把**原厂 bootfs2 原样**写进 Slot 1 → **正常从 Slot 1 启动**（对照组）。

说明：loader（或它之前的阶段）**校验 U-Boot 组件本身**，最可能是 `uboot_sig`（RSA-2048）。
nonsec 只放过了 FIT 头部与内核路径，**没有放过 U-Boot**。

**结论：U-Boot 组件不可修改（一个字节都不行）。**

### 3.2 U-Boot：内核验签（关键发现）

校验不在标准 FIT 签名路径上（FIT 里没有 `signature-1` 节点），而是被塞进了
`bootm_load_os` → `image_decomp()` 的 **LZO 分支**：

```
0x1028fd8   LZO 分支入口
0x1028ff0   bl 0x10a7bac        LZO 解压
0x1029004   bl 0x1032e90        SHA-256(压缩后的 kernel)
0x1029014   bl 0x1000fc0        memset(签名缓冲, 0, 256)
0x1029024   bl 0x100109c        memcpy(签名缓冲, 0x4400000, 256)
0x1029030   bl 0x1032edc        RSA 验签（唯一调用者）
0x1029034   cmp r0, #1          ← 1 = 通过
0x1029038   mvnne r2, #0
0x102903c   ldrne r3, [pc, #0x70]
0x1029040   strne r2, [r3]      失败：置全局错误标志 0x12e44d0 = -1
0x1029044   b 0x1028fcc         继续
```

- 算法：RSA-2048 + PKCS#1 v1.5 + SHA-256
- 签名对象：**压缩后**的 kernel blob（这就是重压缩必失败的原因）
- 验签函数 `0x1032edc` 只有一个调用者（`0x1029030`），影响面可控

#### 3.2.1 image_decomp 的 4 路跳转表（绕过点）

`image_decomp`（0x1028e84）按压缩类型分支：

```
0x1028f18:  sub r3, r6, #1
0x1028f1c:  cmp r3, #3
0x1028f1c:  ldrls pc, [pc, r3, lsl #2]      ; 跳转表 @0x1028f24
```

| 类型 | 压缩 | 分支地址 | 是否验签 |
|---|---|---|---|
| 1 | gzip | 0x1028f58（`bl 0x10a7530`） | **无** |
| 2 | bzip2 | 0x1029048 | 不支持，返回 -2 |
| 3 | lzma | 0x1028fb0（`bl 0x109bfc8`） | **无** |
| 4 | lzo | 0x1028fd8（`bl 0x10a7bac` + `0x1032e90` + `0x1032edc`） | **有** |

**验签只挂在 LZO 分支上。** 把 kernel 的 `compression` 改成 `gzip`（或 `lzma`），
`image_decomp` 走另一条分支，整段 RSA 校验根本不会被执行 —— 而且**不需要修改 U-Boot**。

### 3.3 内核：rootfs 验签

- `fhdrv_kdrv_check_signature`（0xc0848428）→ `_pub`（0xc0848260）
- 打开 `ubiblock0_4` / `0_6`，对 `[0, size-256)` 做 SHA-256，再读尾部 256 字节签名做 RSA 校验
- **返回值：1 = 通过，-1 = 失败**（不是 0！）
- 失败时 `fhdrv_kdrv_rootfs_roll_back` 就地改挂另一槽
- 全部调用点 4 处：`roll_back` 内 2 处、`do_mount_root` 内 2 处
- 内核**不自检**：镜像里没有引用 `0x1500000` 附近地址，`fh_rsa_verify_blob` 是死代码

---

## 4. 密钥与签名

### 4.1 同一把 RSA-2048 公钥

| 位置 | 内容 |
|---|---|
| U-Boot `uboot.bin+0xdf4dc` | `e = 65537` |
| U-Boot `uboot.bin+0xdf4e0` | `N`（256 字节，奇数） |
| U-Boot `uboot.bin+0xdf5e0` | `n0inv` |
| U-Boot `uboot.bin+0xdf5e8` | `rr = 2^4096 mod N` |
| 内核 `0xc0c087f8`（文件 0xb007f8） | `N`（与上面**完全相同**） |
| 内核 `0xc0c08a00` | `e = 65537`（8 字节大端） |
| 内核 `0xc0c089fc` | `bits = 2048` |

### 4.2 数学验证（可复现）

```python
sig = bootfs[0x3021f8:0x3022f8]          # kernel_sig
kernel = bootfs[0x3031d4:0x3031d4+0x5c1d23]  # 压缩内核
N = uboot[0xdf4e0:0xdf5e0]
m = pow(int.from_bytes(sig,'big'), 65537, N).to_bytes(256,'big')
# m 应为 PKCS#1 v1.5：00 01 FF...FF 00 || DigestInfo || sha256(kernel)
```

实测结果：还原出的哈希 = `sha256(压缩内核)` = FIT 的 `hash-1(kernel)`。
这一步同时证明了三件事：公钥位置、签名对象、签名算法。

---

## 5. 绕过方案

### 方案 A：改代码（推荐）

**U-Boot（内核验签）**

```
文件偏移    0x29034（uboot 组件内）
原指令      01 00 50 e3    cmp r0, #1
新指令      00 00 50 e1    cmp r0, r0
```

效果：比较永远相等，`mvnne/ldrne/strne` 不执行，错误标志永不置位。
验签函数本身照常运行（浪费几毫秒），其他逻辑完全不变。

**内核（rootfs 验签）**

```
文件偏移    0x740260
原指令      f0 47 2d e9 …（push {r4-r8,sl,lr}）
新指令      01 00 a0 e3    mov r0, #1
            1e ff 2f e1    bx  lr
```

注意：必须返回 **1**，返回 0 会被判为失败。

### 方案 B：换密钥

把 U-Boot 的 `N/n0inv/rr` 与内核的 `N/e/bits` 换成自己的 RSA-2048 公钥，再用私钥签：

- 压缩后的 kernel blob（PKCS#1 v1.5 + SHA-256）
- rootfs（squashfs 主体 + 尾部 256 字节签名）

代码一行不改，但 `n0inv` 与 `rr` 必须按新模数重算，字段错一个就静默失败。

**但方案 A 已被实测否证**：U-Boot 组件只要改一个字节，loader 就拒绝启动（见 §3.1）。所以 A 不可行。

### 方案 C：换压缩方式（当前推荐）

`image_decomp` 的验签只挂在 LZO 分支（见 §3.2.1）。把 kernel 的 `compression` 改成 `gzip`，
换成 gzip 压缩的同一内核，U-Boot 走另一条分支，验签代码根本不会执行 —— **而且 U-Boot 一个字节都不用改**。

关键步骤（`firmware/build-gzip-fit.py`）：

```
1. 解析 FIT DTB，把 kernel 节点的 compression 值 "lzo\0" 扩成 "gzip\0"（属性长度 4→5，对齐后 +4 字节）
2. 头部 totalsize / off_dt_strings / size_dt_struct 各 +4
3. kernel 用 gzip -9 压缩（6,036,771 → 4,348,258 字节），data-size 与 data-position 全部重算
4. 其余 44 个组件原样，hash-1 全部重算
5. uboot 组件保持逐字节不变
```

**动态验证（Unicorn）**

不靠静态推断，直接把 U-Boot 里的 `gunzip`（0x10a7530）跑起来：

```python
mu = Uc(UC_ARCH_ARM, UC_MODE_ARM)
mu.mem_map(0x1000000, 0x400000); mu.mem_write(0x1000000, uboot_bin)
# r0=dst, r1=dstlen, r2=src, r3=&lenp, r9=fake gd
mu.emu_start(0x10A7530, SENTINEL)
```

实测结果：

```
返回 r0=0        *lenp=11,737,552
输出 sha256 5478036a1c0a2934…   与原厂内核逐字节一致
```

裸环境里的两个坑：

- zlib 需要 malloc → 必须 hook U-Boot 自己的 `zalloc`（0x10a7324）/ `zfree`（0x10a7334），提供 bump 分配器
- U-Boot ARM 用 r9 持有 `gd` → 必须伪造一块全零的 gd，否则 `inflateInit2` 直接失败

这个方法可以推广：任何"这段机器码到底能不能吃我的数据"的问题，Unicorn 比纯静态分析可靠得多。

---

## 6. 写保护与刷写

### 6.1 板级 MTD 写锁

`fhdrv_kdrv_board_impl2.ko` 开机锁定 `mtd0/1/2/3/6`，所有 `ubiupdatevol` / `bcm_bootstate` 报 `EPERM`，
`open("/dev/mtdN", O_RDWR)` 也被拒。标准 mtd-utils / `flash_unlock` / `MEMUNLOCK` 全部无效。

唯一入口：

```c
int fd = open("/dev/fhdrv_kdrv_board", O_RDWR);
uint32_t arg[2] = { mtd_num, 0 };   /* 0=unlock, 1=lock */
ioctl(fd, 0x40085319, arg);         /* _IOW('S', 0x19, uint32[2]) */
```

只解锁要写的分区（`2` = image/ubi0），`1`（loader）与 `6`（factory）保持锁定，写完立即回锁。

### 6.2 刷非活动槽

```sh
fh-mtd-wp 2 0
ubiblock -r /dev/ubi0_<非活动 rootfs>   # cmdline 同时绑定了 0,4 与 0,6
ubiupdatevol /dev/ubi0_<bootfs> <bootfs.bin>
ubiupdatevol /dev/ubi0_<rootfs> <rootfs.sqfs>
bcm_bootstate <6|8>                     # 一次性启动
fh-mtd-wp 2 1
```

### 6.3 bcm_bootstate 语义

```
1/2  NEW/OLD_IMAGE
3/4  NEW/OLD_IMAGE_ONCE
5/6  PART1_IMAGE / PART1_IMAGE_ONCE
7/8  PART2_IMAGE / PART2_IMAGE_ONCE
+1/-1/+2/-2  commit / uncommit
```

- 一次性标志写在 metadata 里，**下次启动被消费**，随后回到 committed 槽
- 如果 mtd2 处于锁定状态，`bcm_bootstate` 会**静默失败**（打印的状态只是它自己的意图），这是最容易误判的坑。判据：
  - **写成功**会打印 `wr_metadata: committed …`
  - **写失败**会打印 `ubiupdatevol: error!: cannot start volume … (Operation not permitted)`
  - 只读调用时，mtd2 锁定会附带 `ERROR!!! Could not open /dev/mtd2`，这行本身不代表写失败，容易误判
  - 最可靠的确认方式：写完再只读执行一次 `bcm_bootstate`，看 `Reboot Partition` 与 `>` 箭头是否指向目标槽
- **边界情况（需注意）**：有报告称，一次性启动 Slot 1 后如果 rootfs 验签失败触发就地回退，`COMMITTED` 可能被改写成 1（`COMMITTED=1, SEQ=3,2, ROLLBACK=1`）。本机多次实测未复现（回退后仍为 `COMMITTED=2`），但一旦出现，需手动复位：
  ```sh
  fh-mtd-wp 2 0 && bcm_bootstate 7 && fh-mtd-wp 2 1
  ```
  然后再只读确认 `COMMITTED=2` 与 `Reboot Partition: Second`。
- 正常重启耗时 **80–120 秒**（关闭 SAF LXC 与 6 个运营商子容器），不要误判为死机

---

## 7. 风险与未验证项

### 已确认

- loader 不校验 FIT 头部与内核路径（nonsec + 策略指针为空 + OTP 未烧）
- **loader 校验 U-Boot 组件本身**：改一个无害字符串即被拒，原厂镜像原样则正常启动（对照实验）
- U-Boot 在 LZO 分支验内核（数学验证 + 反汇编）
- **验签只挂在 LZO 分支**：gzip / lzma 分支没有任何校验调用（反汇编确认）
- **U-Boot 的 `gunzip` 能吃标准 gzip 流**：Unicorn 实跑，输出与原厂内核逐字节一致
- 内核验 rootfs，返回 1 表示通过
- 重打包链路无损（原样重打包逐字节一致）
- 写保护 ioctl 可用

### 未验证 / 不确定

- lzma 分支同样无验签，但未验证 U-Boot 的 `lzmadec` 是否接受 `xz --format=lzma` 的输出
- `uboot_sig` 的具体消费者位置：实测证明"改 U-Boot 就被拒"，但没定位到是哪段代码在比对
- STIF 块（FIT 头部 260 字节：`STIF` + 256 B 签名）是否真的完全不校验：timestamp 实验支持"不校验"，但只测了一个字段
- 内核 LZO 容器 flags 差异（原厂 0x300000d，重压 0x3000009）是否被 U-Boot 的解析器接受：未实测（gzip 路线不需要它）

### 已知风险

- 改 U-Boot 是唯一可能变砖的操作：回退逻辑住在 U-Boot 里，U-Boot 起不来就没有任何东西执行回退
- 建议改 U-Boot 前先接 UART（3.3V，5 针焊盘，115200 8N1）
- 接串口时注意：`fhuart_enable=0` 时 U-Boot 与内核 earlycon 有输出，但进入 `/etc/rc.d/rcS1` 后脚本会关闭 Linux 态串口输出。需要 Linux 里继续交互的话，提前执行 `fw_setenv fhuart_enable 1`（用完改回 `0`）。

---

## 8. 建议的验证顺序

**改 U-Boot 的路线已否证（loader 拒绝），改用"换压缩方式"路线：**

```
Gate 0  gzip 压缩的原厂内核（U-Boot 不动）      → 验证 gzip 分支确实无验签
Gate 1  gzip 压缩的 patched 内核（return 1）    → 验证内核补丁生效
Gate 2  patched 内核 + 自定义 rootfs            → 最终目标
```

每一步失败都能通过 `img_index` 与 `dmesg` 的验签日志区分失败原因：

```
img_index=1 + verify nok → verify ok   → 内核起来了，rootfs 回退（预期）
img_index=2                            → 内核被拒，回到 committed 槽
```

---

## 9. Gate 1 实机验证结果（已通过）

### 9.1 最终确认的启动链

```
TPL      验 U-Boot 组件（uboot_sig，RSA）  → 改一个字节即被拒
U-Boot   验内核，但只在 LZO 分支（0x1028fd8 → 0x1032edc）
          gzip 分支（0x1028f58）无任何校验
内核     验 rootfs（fhdrv_kdrv_check_signature_pub）
```

关键规律：**每一层只认证它要跳转的下一级**。loader 不校验整个 payload ——
把 `fdt_968552QEMU` 改一个字节仍能正常启动，但改 U-Boot 组件必被拒。

### 9.2 实测证据

一次性启动 Slot 1，写入两个镜像：

| 卷 | 内容 | sha256（前 16 位） |
|---|---|---|
| `ubi0_3` | `bootfs_patched.bin`（gzip 内核 + `_pub` 返回 1） | `d409d9c6079e154b` |
| `ubi0_4` | `rootfs_custom.sqfs`（rootfs2 + 标记文件，**未签名**） | `34e8f095cebeb8c7` |

启动后：

```
root=/dev/ubiblock0_4     img_index=1
Booted Partition: First
dmesg: checking signature rootfs =0_4      ← 没有任何 verify nok
/etc/fh_custom_rootfs: custom rootfs built 2026-11-24T09:47:27Z by hermes
```

无线（wl0/wl1 均 associated、hostapd ×2）、面板 dropbear 全部正常。

### 9.3 内核补丁

`fhdrv_kdrv_check_signature_pub`（文件偏移 `0x740260`）前 8 字节改为：

```
01 00 a0 e3    mov r0, #1
1e ff 2f e1    bx lr
```

三路独立静态分析结论一致：

- 函数原型 `int f(int part_id)`，**没有输出参数**，调用方只比较 `r0 == 1`
- 原实现 `open → alloc(124KB) → 整卷读 + SHA-256 → free → close → RSA 验签`，alloc/free、open/close 严格配对，提前返回无泄漏
- 8 字节等长替换，零栈操作，`bx lr` 返回地址正确（wrapper 已 `pop {r4,lr}`）
- roll_back 只由**挂载是否失败**触发，补丁不会误触发，也不会掩盖真实挂载失败

### 9.4 rootfs 构建注意

- 推荐在 Linux `root` 下用仓库的 `firmware/build-rootfs.sh` 解包与重打包（`mksquashfs -comp xz -b 131072`，内核只编了 `CONFIG_SQUASHFS_XZ`），这样能完整保留 `/bin/busybox` 的 `4755` setuid、设备节点与原厂个别非 root 文件属主；如手敲命令且全 root 场景才用 `-all-root`
- **`/bin/busybox` 必须是 4755**：以非 root 解包会丢 setuid，非 root 身份下 `su`/`passwd`/`ping` 会失效
- 未签名镜像是**故意**的：内核已不校验；不要附加 256 字节签名
- 大小上限为 `ubi0_4` / `ubi0_6` 卷容量 **44,314,624 字节**（349 LEBs / 42.2 MiB，原厂 `rootfs2` 约 32.1 MiB，余量 ~10 MiB）

---

## 10. 后续更新流程（不需要回到原厂系统）

### 10.1 只改 rootfs（常规情况）

活动 rootfs 通过 `ubiblock0_4` / `ubiblock0_6` 只读挂载，**不能就地更新**，刷入非活动槽即可：

- **纯净 Alpine 阶段（推荐）**：不再加载原厂 `fhdrv_kdrv_board_impl2.ko`，`mtd2` 原生可写，**无需再调用 `fh-mtd-wp`**。运行 `make build-alpine-rootfs` 生成 `build/rootfs-alpine.sqfs`，随后 `make deploy-rootfs IMG=build/rootfs-alpine.sqfs`（或直接通过 Go 面板 **Web OTA** `POST /api/system/ota` 上传刷入非活动槽，重启试跑后点「永久设为默认」即可）。
- **原厂精简阶段（阶段一兼容流程）**：需通过 `firmware/flash-ab.sh` 自动调用 `/opt/upt/apps/ap-panel/bin/fh-mtd-wp` 解锁/回锁 `mtd2` 并解除对应的 `ubiblock`：

```bash
# 1. 在构建机（Linux root）生成新 rootfs
sudo sh firmware/build-rootfs.sh backups/rootfs2.bin /tmp/new.sqfs

# 2. 传到 AP 后刷入非活动槽（Slot 2 的 rootfs = ubi0_6，同时写齐 bootfs）
#    手动步骤等价于：
/opt/upt/apps/ap-panel/bin/fh-mtd-wp 2 0
ubiblock -r /dev/ubi0_6
ubiupdatevol /dev/ubi0_6 /tmp/new.sqfs
ubiupdatevol /dev/ubi0_5 /tmp/bootfs_patched.bin
bcm_bootstate 8          # 一次性启动 Slot 2
/opt/upt/apps/ap-panel/bin/fh-mtd-wp 2 1
sync; reboot

# 3. 验证通过后永久提交
/opt/upt/apps/ap-panel/bin/fh-mtd-wp 2 0 && bcm_bootstate 7 && /opt/upt/apps/ap-panel/bin/fh-mtd-wp 2 1
```

失败时：不再动它，下次启动仍回 committed 的原槽位。

### 10.2 改内核（少见）

需要重走 FIT 重打包（注意 `build-gzip-fit.py` 接收 3 个参数：`<原厂bootfs> <gzip内核> <输出bootfs>`）：

```bash
# kernel.bin（raw vmlinux）→ 打补丁 → gzip -9
python3 firmware/build-gzip-fit.py backups/bootfs2.bin build/kernel_new.gz build/bootfs_new.bin
python3 firmware/emu-gunzip.py build/uboot.bin build/kernel_new.gz build/kernel_new.bin   # 可选：Unicorn 实跑 gunzip 验证
```

不能只改 kernel blob 而保持 LZO —— LZO 分支会验签。

### 10.3 保留原厂作为救砖备份

两个槽的 `bootfs` 现在都是自定义内核，设备上已没有原厂 bootfs；原厂完整镜像
（`rootfs1/2.bin`、`bootfs1/2.bin`、`factory` 等）离线保存在构建机 `/opt/data/fw/`。
需要退回原厂时，按 §10.1 的流程把原厂 `bootfs` + 签名 `rootfs` 写回非活动槽，
一次性启动验证后再提交即可，不需要拆机。

### 10.4 工具与坑

- `fh-mtd-wp` 已持久化放在 AP 的 `/opt/upt/apps/ap-panel/bin/fh-mtd-wp`（重启不丢），`firmware/flash-ab.sh` 会自动查找并调用
- `bcm_bootstate` 在 mtd2 上锁时**静默失败**：可能只打印 `wr_metadata` 但回读仍是旧值，必须先 `fh-mtd-wp 2 0` 解锁后再写，写完再用 `bcm_bootstate` 只读回看确认
- 写 rootfs 前如该槽已被 `ubiblock` 占用，必须先 `ubiblock -r /dev/ubi0_<n>`
- 非 root 解包会丢 setuid 位，打包前确认 `bin/busybox` 为 `4755` 且目录属主未被改乱

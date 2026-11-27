# HG6371F 实机架构与运维手册

以下内容全部经实机核实（`192.168.1.2`，BCM68252C，Linux `4.19.235`，Buildroot glibc 2.32）。

## 1. 硬件与网络拓扑

```text
上级网关 192.168.71.1
       │ (eth1, 1G WAN: 192.168.71.4)
  [ N100 OpenWrt 主路由 + Docker 宿主机 ]  br-lan: 192.168.1.1/24
   ├── eth0 (2.5G) ──▶ 有线终端 / 构建机
   └── eth3 (2.5G) ──▶ HG6371F 纯二层 AP (br0:101 = 192.168.1.2)
                        ├── lan0 (2.5G 上联) / lan1 (1G，目前闲置)
                        ├── wl1 (5G 2×2 160MHz Wi-Fi 6)
                        └── wl0 (2.4G)
```

- **SoC / 芯片**：Broadcom BCM68252C（双核 Cortex-A7 1.5GHz，集成 5G 2×2 160MHz Wi-Fi 6）+ BCM6705（2.4G）+ RTL8221B（2.5G PHY）
- **内存 / 闪存**：512 MB RAM / 256 MB SPI NAND（W25N02KV，PEB 128 KiB / LEB 124 KiB）

## 2. 分区表与双槽现状

| MTD / UBI | 名称 | 类型 / 容量 | 用途与注意事项 |
|---|---|---|---|
| `mtd1` | `loader` | 2 MiB | Broadcom SPL + TPL，**严禁写入** |
| `ubi0_1` / `ubi0_2` | `metadata1` / `metadata2` | static, 1 LEB | A/B 元数据，**严禁 `ubirmvol` / `ubimkvol`** |
| `ubi0_3` / `ubi0_5` | `bootfs1` / `bootfs2` | static, 128 LEBs (15.5 MiB) | FIT 镜像（ATF + U-Boot + kernel + DTBs） |
| `ubi0_4` / `ubi0_6` | `rootfs1` / `rootfs2` | static, 349 LEBs (`44,314,624` B / 42.2 MiB) | 只读 SquashFS 根文件系统 |
| `ubi1_0` | `/opt/upt/apps` | ubifs rw, 68.4 MiB | 可写持久化区（面板、Dropbear、脚本、`fh-mtd-wp`） |
| `ubi1_1` | `/fhconf` | ubifs rw, 4.3 MiB | 持久化配置区 |
| `ubi2_0` | `/fhdata` | ubifs ro | 射频校准与出厂数据 |

- **出厂基线**（均已备份至本地 `backups/`）：Slot 1 原为 `2024-04-10 (BUILDNUM 180)`，Slot 2 为 `2026-05-13 (BUILDNUM 897)`，两版内核与 Broadcom `.ko` 驱动不兼容，刷写时需保证同槽 `bootfs` 与 `rootfs` 版本对齐。
- **当前实机状态**：Slot 2（`ubi0_5` / `ubi0_6`，活动且已提交，`committed=2`）运行自定义内核 + 纯净 Alpine Linux 3.21.10（`armv7`、`musl + apk + OpenRC` + `/opt/bcm` 最小博通驱动底座，`16.27 MiB`，内置 3 字节 `wl.ko` CSIMON Netlink `28/23` 补丁，`/var` 内存 `overlay` + `/etc` 持久化 `overlay` 至 `/opt/upt/apps/alpine-etc/`，零原厂依赖 Go 面板监听 `:80` 强制跳转 `:443` HTTPS，OpenSSH `:22`，上游 `hostapd 2.11` 关闭 WPS）；Slot 1（`ubi0_3` / `ubi0_4`）同为纯净 Alpine 槽，作为 A/B 升级交替槽。
- **两个 `bootfs` 均已替换为自定义内核**（gzip 压缩 + `fhdrv_kdrv_check_signature_pub` 返回 1），因此任一槽都可挂载未签名 `rootfs`；设备上已不再保留原厂 bootfs，原厂镜像离线备份在本地 `backups/`。

## 3. 启动链、校验与看门狗

### 启动脚本链
`/etc/inittab` → `/etc/rc.d/rcS` → `rc3.d/S*`（挂载分区、拉起看门狗与 Broadcom 驱动）→ `fhconf_init.sh` → `fhsys_start` → `rcS1` → `rcS2` → `rcS3 &`（异步拉起 `sysmgr`、原厂 Web 与 Wi-Fi，自定义启动钩子挂在 `rcS3` 末尾异步执行）。

### 分层校验概览（细节见 `docs/boot-chain-verification.md`）
- `fw_printenv` 中的 `bootm_boot_mode=nonsec` 只被 **loader** 读取（U-Boot 中无此开关）；实测三层各管下一级：TPL 校验 U-Boot，U-Boot 在 LZO 分支校验内核，原厂内核在 `mount_root` / `do_mount_root` 中内置了烽火自研的软件 RSA-2048 + SHA-256 验签链路（`# CONFIG_BCM_ROOTFS_SQUASHFS_HASH is not set`；`/proc/kallsyms` 对应符号：`fhdrv_kdrv_check_signature` → `fhdrv_kdrv_check_signature_pub`、`caculate_digest`、`get_digest_from_sign` / `do_sw_decrypt` / `rsa_mod_exp_sw` / `rsa_verify_padding`）。
- 原厂 `rootfs1/2.bin` 均由「4 KiB 对齐的 SquashFS 主体 + 尾部 256 字节 RSA-2048 签名」组成（`rootfs1` = `33,112,064 + 256`，`rootfs2` = `33,665,024 + 256`）。
- 原厂内核验签失败时会打印 `checking signature rootfs =0_4` → `verify nok ret:-1`，`do_mount_root` 返回 `-22 (-EINVAL)` 并触发 `fhdrv_kdrv_rootfs_roll_back` 就地改挂另一槽带签名的 `ubiblock`（如 `0_6`），因此不会因 `rootfs` 验签失败而变砖。
- 内核 SquashFS 仅开启了 `CONFIG_SQUASHFS_XZ=y` 与 `CONFIG_SQUASHFS_ZLIB=y`（`# CONFIG_SQUASHFS_LZ4 is not set`，**不能用 LZ4**）；原厂 `rootfs2` 为 XZ 压缩、128 KiB block（约 32.1 MiB，距 42.2 MiB 卷上限余量约 10.1 MiB），打包必须用 `mksquashfs -comp xz -b 131072`。

- **当前活动槽的内核已打补丁**：`fhdrv_kdrv_check_signature_pub` 被替换为 `mov r0,#1; bx lr`（文件偏移 `0x740260`），rootfs 完整性校验不再执行，因此 Slot 1 可以挂载未签名 `rootfs`；`dmesg` 只剩 `checking signature rootfs =0_4`，不再出现 `verify ok/nok`。补丁细节与三路静态分析见 `docs/boot-chain-verification.md` §9。
- **U-Boot 对内核的验签只在 LZO 分支**：当前内核以 gzip 压缩装入同一 FIT，走无校验的 gzip 分支；`kernel_sig` / `uboot_sig` 保留原厂字节但已不对应当前内核，不能再作为完整性依据。

### 硬件看门狗与自动复位
- `/etc/rc3.d/S26wdtd` 很早就会启动 `/bin/wdtctl -d -t 30 start`（30 秒超时硬件复位）。
- 内核编进了 `CONFIG_PANIC_TIMEOUT=5` 和 `CONFIG_PANIC_ON_OOPS=y`，挂载失败或内核 Oops 会在 5 秒后自动重启。

## 4. 刷写与日常维护避坑清单

1. **板级 MTD 写保护锁（`EPERM` 的真正原因）**
   - 开机时 `fhdrv_kdrv_board_impl2.ko` 会锁住 `mtd0`/`mtd1`/`mtd2`/`mtd3`/`mtd6`，直接跑 `ubiupdatevol` 或 `bcm_bootstate` 写入会报 `Operation not permitted`。
   - 原厂 `libfhdrv_kdrv_board_impl.so` 通过 `/dev/fhdrv_kdrv_board` 的 `ioctl 0x40085319`（参数 `uint32[2]{mtd_num, 0|1}`）只解锁目标分区。仓库对应的零依赖工具是 `firmware/fh-mtd-wp.go`（设备端已持久化在 `/opt/upt/apps/ap-panel/bin/fh-mtd-wp`），只解锁 `mtd2`（`image` / `ubi0`），写完立刻回锁，`mtd1 (loader)` 和 `mtd6 (factory)` 全程保持锁定：
     ```sh
     /opt/upt/apps/ap-panel/bin/fh-mtd-wp 2 0
     # 执行刷写或 bcm_bootstate
     /opt/upt/apps/ap-panel/bin/fh-mtd-wp 2 1
     ```
2. **`bcm_bootstate` 写成功判据与常用参数**
   - 常用参数：`6` / `8` 为一次性启动 Slot 1 / Slot 2；`5` / `7` 为永久提交 Slot 1 / Slot 2。
   - 不管 `mtd2` 锁没锁，跑 `bcm_bootstate` 都会夹杂几行 `ERROR!!! Could not open /dev/mtd2`，别被它误导。真正写入成功会打印 `wr_metadata: committed ...`，但最稳妥的确认方式永远是写完再只读跑一次 `bcm_bootstate`，看 `Reboot Partition` 和 `>` 箭头是否确实切到了目标槽。
   - 如果一次性启动某槽后只因 `rootfs` 验签失败触发了内核就地回退，可能出现「内核是 Slot 1、`/` 挂在 Slot 2、`COMMITTED` 被改写成 1」的混搭态，手动解锁后执行 `bcm_bootstate 7` 即可恢复。
3. **写 `rootfs` 前必须解除 `ubiblock`**
   - `/proc/cmdline` 同时绑定了 `ubi.block=0,4` 和 `ubi.block=0,6`，更新非活动 `rootfs` 卷前必须先 `ubiblock -r /dev/ubi0_<4|6>`（`firmware/flash-ab.sh` 已自动处理）；若需备份或对齐某槽的 `bootfs`，也可直接在 AP 上本地导出（如 `cat /dev/ubi0_5 > /tmp/bootfs2.bin`）。
4. **`br0` 双 IPv4 启动竞态**
   - `br0` 开机很早就会挂上原厂静态地址 `192.168.2.1/24`，而真正的管理地址 `192.168.1.2/24` 由 `udhcpc` 较晚拿到并挂在 `br0:101`。
   - 开机钩子不能只查 `br0` 有没有 `inet `，必须精确等 `start.sh` 里的 `LISTEN_IP`（或回退等 `br0:101`），否则 Dropbear 和 Go 面板会因绑定地址不存在而启动失败。
5. **打包 `rootfs` 的权限与属主陷阱**
   - 必须在 Linux `root` 下解包和重打包，否则 `/bin/busybox` 会丢 `4755` setuid 位、设备节点也会损坏。
   - 注入文件时不要用 `cp -a "$OVERLAY/." "$WORK/root/"`，它会把 `/`、`/usr`、`/usr/sbin` 的属主覆盖成打包机普通用户（UID 1000，在 AP 上对应 `ctuser`）并改掉 `/usr/sbin` 权限。`firmware/build-rootfs.sh` 已改为逐文件拷贝并强制 `chown 0:0`，自动过滤 macOS 的 `.DS_Store`；打包后建议用 `unsquashfs -ll` 对原镜像做一次全量元数据 diff 确认。
6. **传输、重启耗时与命令库路径**
   - Dropbear 不带 `sftp-server`，现代 macOS 默认 `scp` 会直接断连，传文件请用 `ssh ap.example.local 'cat > ...'` 或 `tar | ssh`。
   - 正常 `reboot` 要等 SAF LXC 和 6 个运营商子容器优雅退出，从 `DHCPRELEASE` 到物理断链复位约需 **80 秒**，整机恢复在线共约 **80~120 秒**，不是死机。
   - 在 AP Shell 里直接跑 `curl`、`uci` 等程序时，如果报缺 `libcurl.so.4` / `libubox.so`，加上 `LD_LIBRARY_PATH=/lib:/usr/lib:/fhrom/lib` 即可。
   - 这版 `2026-05-13` 固件里，社区老帖提到的 `/fhrom/fhshell/crond_start.sh` 已改为只读 `/fhrom/fhconf/crontab_conf` 且限定 `area_code="Sichuan"`，`sc_cube_init.sh` 同样限四川且每次从 `/fhrom` 覆写，均不再可用。

## 5. 原厂 Web 接口与应急恢复

### 原厂 Web 接口契约（面板后端依赖）
- 本地 `thttpd` 监听 `127.0.0.1:8840`（对外经 `nginx :8080` 代理）。
- 请求须带 `X-Requested-With: XMLHttpRequest` 和独立 `User-Agent`（避免踢掉浏览器会话）。
- 登录密码与 Wi-Fi PSK 使用 `AES-128-CBC`（PKCS7），`key = iv = "*$" + MAC12大写 + "#&"`；返回 JSON 中 `"session_valid":\t0` 冒号后可能带制表符，需用标准 JSON 解析。

### 应急 Telnet 恢复（脱敏规范）
凭据只存放在已被 `.gitignore` 忽略的本地 `.env`（`chmod 600`）或主路由 `/etc/ap-watchdog.env` 中，**不要写入 Git**。

- **口令推导规则**：
  - `AP_TELNET_USER`：`telnetadmin`
  - `AP_TELNET_PASS`：`FH-` + `<超管密码>` + `<设备 MAC 后 6 位（大写）>`
  - `AP_SU_PASS`：`Fh@` + `<设备 MAC 后 6 位（大写）>`（只和 MAC 有关，改超管密码不变）
- **开启 23 端口 / 找回超管密码**：冷启动后 23 端口默认关闭（且原厂 Web 的 `set_services` 在此版本上打不开 23 端口），需从主路由 `br-lan` 侧走 IPv6 link-local 调用 `/fh_tool/api`：
  - 地址：`POST http://[fe80::100%25br-lan]:8080/fh_tool/api`（`Content-Type: text/plain`）
  - 加解密：`Base64(AES-128-CBC(PKCS7(JSON)))`，其中 `d = sha256(MAC12大写无分隔).hexdigest()`，`key = "".join(d[2*i + 2] for i in range(16))`，`iv = "".join(d[3*i + 3] for i in range(16))`
  - 常用 `func`：`TelnetEnable`（`{"telnet":"1"}` / `"0"` 开关 23 端口）、`GetAdminAccount`（取当前明文超管密码）、`LogDownload`（日志包内 `var/log/fhcfg.log` 含 `TelnetPassword`）
- **手动恢复**：跨网段时先经主路由 `ssh -L 127.0.0.1:12323:192.168.1.2:23 router` 转发，再用 `recovery/fh_shell.py` 登录提权执行 `setsid sh /opt/upt/apps/ap-panel/start.sh </dev/null >/tmp/manual-start.log 2>&1 &`。
- **N100 主路由自动守护（当前 cron 已停用，保留作手工兜底）**：
  - 既然精简版 `rootfs` 已内建 `/usr/sbin/ap-boot-hook.sh` 原生自启，主路由 `/etc/crontabs/root` 中的定时任务已移除（避免与原生自启在启动窗口重叠）；脚本 `/usr/sbin/ap_watchdog.py` 与凭据 `/etc/ap-watchdog.env`（`chmod 600`）仍保留在主路由上，如需恢复自动兜底或手工调用可执行：
    ```cron
    */2 * * * * set -a; . /etc/ap-watchdog.env; set +a; /usr/bin/python3 /usr/sbin/ap_watchdog.py 2>&1 | logger -t ap-watchdog
    ```
  - AP 正常时仅探测 `:22` 和 `:8081` 端口，几毫秒退出；AP 未自启时可在拿到 DHCP 后约 20 秒内自动拉起 `start.sh`，并随即把 23 端口重新关掉。

## 6. 串口（UART / TTL）调试要点

- **接线**：3.3V USB-TTL（严禁 5V），主板预留 5 针焊盘，只接中间 3 针（TX / RX / GND，**VCC 悬空不接**）；不想焊接可用 2.54mm 3P 弹簧探针，GND 也可以直接用鳄鱼夹夹在网口金属屏蔽壳上。波特率 `115200 8N1`，无流控。
- **U-Boot 窗口**：`bootdelay=1`，上电瞬间连续按键即可打断进 U-Boot。
- **Linux 态默认静音**：环境变量默认 `fhuart_enable=0`，U-Boot 和内核 `earlycon` 有输出，但跑到 `/etc/rc.d/rcS1` 时脚本会读 `/proc/driver/fhuart/enable` 并执行 `echo 0 0 > /proc/driver/uart/enable` 关闭串口。想在 Linux 里继续看日志和交互，提前在 SSH 里执行 `fw_setenv fhuart_enable 1`（用完可改回 `0`）。

## 7. 运行态演进实测对比（原厂 → 第一阶段精简 → 纯净 Alpine）

- **原厂基线（原厂 `rootfs`）**：待机内存占用约 `230~238 / 508 MiB`（可用 ~254 MiB），进程数 `168`，SoC 温度（`thermal_zone0`）约 `71~73°C`，平均负载 `~6`，且有 `cloudclient → 218.78.32.124:6880` 与 `link_agent → 120.26.130.222:8883` 两条常驻外联。
- **第一阶段精简（保留在 Slot 1，仅停服务不删二进制）**：摘除 `saf` LXC 及 6 个子插件、`tr069`、`easymeshagent`、`gdecms`、`cloudclient`、`link_agent` 等；待机内存降至 `188 / 508 MiB`（可用 `~300 MiB`），进程数 `120`，公网外联清零，保留完整光功能作回退槽。
- **全面替换纯净 Alpine（当前主力 Slot 2，Slot 1 已同步为同版 Alpine 备用槽，`firmware/alpine/`）**：
  - 彻底移除 `/fhrom` 与 `fhdrv_*` / GPON 模块，仅保留 30 个精选博通 `.ko` 与隔离在 `/opt/bcm/` 下的最小博通工具/glibc 运行时，根镜像由 `32.10 MiB` 减半至 **`16.27 MiB`**。
  - 待机/轻载内存占用降至 **`~150–159 / 496 MiB`**（可用 **`~328–336 MiB`**），整机进程数 **`~92–97`**（用户态仅 ~12 个），平均负载 **`~0.3–1.1`**（CPU 100% idle），空载/轻载温度 **`58–67°C`**。
  - 无线改由上游 `hostapd 2.11`（`driver=nl80211`，关闭 WPS；实测 `wl.ko 17.10.251` `device_ap_sme=1` 走 `WPA2-PSK` 满特性工作，暂不兼容上游 `WPA3-SAE`），射频调优参数统一归口面板 `config.json` 与 `/etc/sysctl.d/99-ap-tune.conf`，Mac 5G 160MHz ↔ N100 2.5G `iperf3 -P 4` 实测 **下行 `1.34 Gbps`（接收 `1.31 Gbps`）/ 上行 `0.92–1.02 Gbps`**，FlowCache L2/L3 硬件加速直通生效。
  - 面板已完成重写与前端规范化重构（零第三方依赖 Go 标准库 + 原生零构建前端，gzip 后仅 ~41 KiB），彻底移除原厂 `thttpd`/`vendor.go` 依赖，直写 `/etc/hostapd/wl{0,1}.conf`、托管 OpenRC 服务与 A/B 切槽，并原生支持 `:443` HTTPS 证书自动热加载（`/opt/upt/apps/ap-panel/etc/tls.{crt,key}`）；前端遵循 HTML5 语义化与 WCAG AA / WAI-ARIA 规范（`#hash` 路由、方向键页签导航、1:1 `ResizeObserver` 无畸变 SVG 图表与悬停准星读数、原生 `<dialog>` 模态框、防竞态背压 `AbortController` 轮询、无 URL Token 泄露的鉴权 Blob 下载），后端统一注入 `CSP`、`X-Frame-Options: DENY`、`X-Content-Type-Options: nosniff`、`Referrer-Policy` 与 `Vary: Accept-Encoding` 响应头。

## 8. 板级 GPIO（状态灯）与 5G DFS 扫描说明

### 前面板双色状态灯与板级 GPIO（`hwcfg=0xba`，已实机核验）
原厂内核自带标准 `/sys/class/gpio/` 接口，无需加载 `fhdrv_kdrv_board*.ko`。以下引脚已由反汇编 `fhdrv_kdrv_board_impl.ko` 并经实机点亮确认（红/蓝 LED 均为低电平点亮：`0`=亮，`1`=灭）：

| GPIO | 原厂定义 | 实际功能 / 颜色 | 说明 |
|---|---|---|---|
| `10` | `OUT_LOS_LED` | **红灯** | U-Boot 上电默认拉低；用于开机、关机及断链/未获 IP 状态 |
| `11` | `OUT_PON_LED` | **蓝灯** | `lan0` 2.5G 上联连通且 `br0` 获取到 IPv4 时常蓝 |
| `2` | `OUT_WAN_LED` | 未接前面板灯 | 实测本机 PCB 前面板为红/蓝双色灯，拉低无亮灯 |
| `31` | `IN_SYS_RESET` | 复位按键 | 输入 |
| `9` | `IN_LED_BUTTON` | 面板按键 | 输入 |
| `13` | `OUT_GPON_POWER` | 光模块供电 | 输出 |
| `12` | `IN_RX_LOS` | 光收端 LOS | 输入 |

- **当前自动联动**：开机（U-Boot、`bcm-early-mount` 与 OpenRC `boot` 阶段的 `/etc/init.d/ap-led`）保持**红灯**；`ap-panel-srv` 每 2 秒在其内置采样循环中检查 `/sys/class/net/lan0/carrier` 与 `br0` IPv4，就绪即切为**蓝灯**，断链或失 IP 自动回落为**红灯**（关机/重启时 `ap-led stop` 亦切回红灯）。

### 5G (`wl1`) 160MHz 与 802.11h (`spect 0`) 说明
- `wl1` 工作在 `48/160`（覆盖 52–64 DFS 频段）时，若保持博通 `wl.ko` 默认 `spect 1`（开启 802.11h），驱动会进入 `DFS In-Service Monitoring (ISM)` 状态，导致全频段/多信道 `wl -i wl1 scan` 被拒绝（`BCME_SCANREJECT (-43)` / `wl: Scan Rejected`）。
- 原厂固件通过 NVRAM `wl1_reg_mode=off` 在启动时关闭 802.11h。`firmware/alpine/rootfs/etc/init.d/bcm-hw` 已对齐原厂，在拉起 `wl1` 前设置 `wl -i wl1 spect 0`，`dfs_status` 保持 `IDLE`，原生 `wl -i wl1 scan` 即可一次性完成 5G 全频段扫描且不影响 `48/160` 运行。

### HTTPS 泛域名证书自动推送（N100 → AP）
- 证书源：N100 主路由每日 `00:00` 由 `/etc/init.d/acme renew` 自动续期 `*.example.local` ECC 泛域名证书（`/etc/ssl/acme/*.example.local.{fullchain.crt,key}`）。
- 推送链路：N100 `/usr/sbin/push-ap-tls.sh`（cron `10 0 * * *`）先比对本地 SHA-256 与 `/etc/acme/.ap_tls_pushed.sha256`，仅在证书更新时通过专用 Dropbear Ed25519 密钥（`/etc/dropbear/ap_cert_push_ed25519`，并在 `/root/.ssh/known_hosts` 固定 AP 主机公钥）以 `tar | dbclient -T` 推给 AP。
- 最小权限接收端：AP 侧 `/etc/ssh/authorized_keys_root`（同步持久化于 `/opt/upt/apps/ap-panel/root/.ssh/authorized_keys`）对该密钥配置了 `command="/opt/upt/apps/ap-panel/bin/recv-tls-cert.sh",no-port-forwarding,no-X11-forwarding,no-agent-forwarding,no-pty`，无法打开 Shell 或执行其他命令；接收端校验 PEM 与哈希后原子写入 `/opt/upt/apps/ap-panel/etc/tls.{crt,key}`，`ap-panel-srv` 自动按 `mtime` 热加载 `:443` 证书并开启 HTTP → HTTPS `308` 强制跳转。

### Wi-Fi 感知雷达（`CSIMON 1.1.0` + `STA Monitor`）、主机名发现、24h 空口记录与 `/metrics`
- **CSIMON 3 字节补丁**：`build-alpine-rootfs.sh` 自动将 `wl.ko` 偏移 `0x30b084` 处原厂 Netlink `26/24` 重映射为空闲的 `wl0→28`、`wl1→23`（解决与内核 Netlink `24–27` 冲突导致的 `-1`）。
- **防挂死保护与 MAD 检测**：Go 面板（`panel/server/radar.go` + `radar_linux.go`）在 `startupDone` 且 `wl -i <iface> assoclist` 确认目标终端在线后才武装 CSIMON（离线 5 秒自动解除），基于 `W=10` 帧滑窗的 RX1 归一化幅度 MAD + 中间 10 子载波双天线相位差圆 MAD 计算运动指数，支持 `confirm_sec` 持续防抖、手动扫描自动消隐、20 秒空房自动标定与 Webhook 测试推送。
- **未关联设备靠近监测（`STA Monitor`）**（`stamon.go`）：调用 `wl sta_monitor` 双频被动捕获任意指定 MAC（硬件 AMT 上限 4 台）的实时 RSSI，按阈值与离开超时判定 `在场 / 离开` 并推送 Webhook。
- **内核 cBPF 控制面嗅探与主机名发现**（`arp_linux.go` + `mdns.go`）：挂载 21 条 cBPF 过滤指令，在零数据面开销下监听 ARP + DHCP（`Option 12/81`）+ mDNS/LLMNR，配合局域网 DNS PTR 与 `/tmp/ap-hosts.json` 纯内存热缓存自动补全二层终端 IP 与主机名。
- **24 小时零写闪存空口记录仪与 Prometheus `/metrics`**（`rflog.go` + `metrics.go`）：每分钟采集双频 `OBSS / Busy / 底噪 / 窗口重传 / 三路温度` 存 1440 点纯内存环形缓冲，自动记录设备上下线与双频切换并支持 CSV 导出（无后台周期扫描）；`/metrics` 原生导出系统、无线与雷达全套监控指标。
- **多 SSID + 802.1Q VLAN**：面板支持创建 `wl0.1`/`wl1.1` 访客/IoT SSID 并绑定独立 `lan0.<vid>` + `br-vlan<vid>` 网桥；所有射频运行态参数已统一收拢到 `/opt/upt/apps/ap-panel/config.json`，`sysctl` 归口 `/etc/sysctl.d/99-ap-tune.conf`。

# 无线与底层调优记录

## 已验证的最优配置

### 5GHz (`wl1`)
- **信道与频宽**：固定主信道 `48/160`（`chanspec 0xeb32`，保留 2402 Mbps 峰值，避开同频强邻区主信道 `36`/`40`），模式 `n,ac,ax`
- **MAC / PHY 参数**（由 `device/tune.sh` 施加，热生效不掉线）：
  - `frameburst 1`
  - `ampdu_rts 0`（取消每帧 A-MPDU RTS/CTS 握手开销）
  - `phy_rxdesens 15`（轻度接收降敏，过滤远距弱邻区干扰）
  - `wme_ac ap be aifsn 2 ecwmin 3 ecwmax 5 txop 47`（优化 AP 侧竞争窗口与突发上限）
- **发射功率**：保持默认自动（22 dBm）。实测手动降到 17 dBm 会导致吞吐下降（DL 1022 → 897 Mbps，UL 915 → 699 Mbps）。

### 2.4GHz (`wl0`)
- 固定 `ch 6 / 20MHz`（`0x1006`，模式 `b,g,n` 兼顾 IoT 兼容性）+ `frameburst 1`，底噪从 `-77 dBm` 降至 `-80/-81 dBm`。

### 系统层
- 创建 `/fhconf/wifi_rxdesens_disable` 停用无用的 `wifi_rxdesens.sh` 每 2 秒轮询。
- `net.core.netdev_max_backlog=4096`、`rmem_max=4194304`、`wmem_max=4194304`。
- Broadcom Flow Cache L2/L3 硬件加速实测卸载率约 **99.3%**（`wl1 ↔ lan0`）。

## 严禁踩踏的雷区

> **绝对不要修改 `/proc/irq/*/smp_affinity`**（尤其是 `wlan_1_m2m`、`wl0`、`brcm_63` 等 Broadcom 中断）。
> 实机验证修改 IRQ 亲和性会破坏 Broadcom 无锁 `wl1-kthrd`/`wfd1-thrd` 调度假设，导致高负载下直接触发内核 Panic 重启，且下行吞吐下降约 200 Mbps。

## 实测无线吞吐基线（Mac Wi-Fi ↔ 主路由 N100）

- `iperf3 -P 4`：**1,200 Mbps 下行 / 1,090 Mbps 上行**
- LibreSpeed (`https://speedtest.example.local/`)：**1,034 Mbps raw (1,096 ×1.06) 下行 / 998 Mbps raw (1,057 ×1.06) 上行**

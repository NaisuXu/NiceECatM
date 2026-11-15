# EtherCAT performance

**English** | [简体中文](performance.zh-CN.md)

Source-checkout guidance for developers and hardware testers; not included in release packages. NiceECatM never changes adapter settings automatically.

Latest result on this machine: disabling Interrupt Moderation reduced typical receive latency and worker CPU substantially. The current Controller still averaged about 1.08–1.10 ms at a 1 ms target and 0.86–0.87 ms at a 500 us target in 30-second runs, with remaining WKC errors. Adapter discovery without IP bindings was fixed and verified on real hardware. All original adapter settings and bindings have been restored; product tuning defaults are unchanged.

## What has been measured

Initial Windows / Intel I219-LM / Npcap results with Interrupt Moderation and EEE enabled: read-only AL-status BRD, one send/receive without retransmission, 5 ms receive limit, approximately 400–2,000 samples per case.

| Requested pause between exchanges | Total RTT, p50 | Send call, p50 | Receive wait, p50 |
| --- | ---: | ---: | ---: |
| None | 489 us | 19 us | 468 us |
| 1 ms | 539 us | 41 us | 496 us |
| 10 ms | 683 us | 113 us | 547 us |

Software timings show receive delivery dominates; column medians need not add up. The former half-period receive budget was too short at 500 us and 1 ms targets on this machine.

The Windows receive window now allows 2–10 ms depending on the target period and returns as soon as a frame arrives. Check actual cycles, jitter and missed cycles: a longer timeout does not establish cycle performance. SOEM applies this timeout per frame, so segmented exchanges can take longer.

A read-only Npcap query of `OID_GEN_INTERRUPT_MODERATION` succeeded, but this I219-LM driver returned `NdisInterruptModerationNotSupported` (state 1). No OID SET request was made. The administrator tests below changed the driver's Advanced settings and restarted the adapter instead; an exposed Advanced-tab setting does not imply support for runtime OID switching. [Microsoft OID contract](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-gen-interrupt-moderation).

Before adapter tuning, short software trials compared Normal/AboveNormal/Highest worker priority, an additional `timeBeginPeriod(1)` and High QoS at 10/5/2/1/0.5 ms targets, for 5 seconds per case. No consistent benefit emerged amid substantial machine variability. The product retains its default scheduling settings; these short trials are not a rigorous benchmark.

## OP tests on this machine

These pre-tuning reference tests used Windows 11 LTSC build 26100, Npcap 1.90, Intel I219-LM and the AX58100/LAN9252 slaves with normal and overlapped mapping. Each cell below covers a separate 20-second run; WKC is expected to be 6. Output patterns `00/55/AA/FF/00` were submitted before each run. Both slaves remained in OP with AL code 0; no automatic stop occurred. Output images were zeroed before returning to PRE-OP and disconnecting. Physical output voltages were not measured.

WKC error counts below refer to cycles with an unexpected working counter; they are not a measured network packet-loss rate.

| Target | Normal: WKC errors / cycles | Overlapped: WKC errors / cycles | Mean interval, normal / overlapped |
| --- | ---: | ---: | ---: |
| 10 ms | 3 / 1992 | 6 / 1992 | 10.04 / 10.04 ms |
| 5 ms | 31 / 3956 | 33 / 3950 | 5.06 / 5.06 ms |
| 2 ms | 103 / 9810 | 141 / 9672 | 2.04 / 2.07 ms |
| 1 ms | 184 / 13605 | 165 / 14353 | 1.47 / 1.39 ms |
| 500 us | 206 / 16472 | 173 / 16729 | 1.22 / 1.20 ms |

These are short observations under changing host load, not strict mapping-mode comparisons or timing guarantees. NIC RX/TX errors and discards remained zero. The 500 us runs consumed approximately 43–45% of one CPU core in the I/O worker, largely polling for receive delivery. Increasing thread priority does not remove that delivery latency. Same-bus dual-port redundancy was not tested.

## Administrator A/B tests, 2026-11-14

The same hardware and controller/backend library were tested with the balanced power plan and default worker scheduling, without GUI rendering or script load. Only the balanced plan was available; no global power plan was created or switched. The original settings were backed up. Each changed configuration was read back after an adapter restart, tested separately, and restored. Both settings were enabled again for the final baseline. Normal mapping was used; each OP target ran for 5 seconds, with output-pattern checks before sampling. Both slaves were observed in OP with AL code 0 and no automatic stop.

At the 1 ms RTT probe interval, each configuration used 2,000 requests. Quantiles below include successful frames only; timeouts are reported separately.

| Configuration: Interrupt Moderation / EEE | RTT p50 | RTT p99 | Timeouts / requests |
| --- | ---: | ---: | ---: |
| Original: on / on | 545 us | 1361 us | 2 / 2000 |
| Off / on | 116 us | 1441 us | 2 / 2000 |
| On / off | 548 us | 1546 us | 2 / 2000 |
| Off / off | 122 us | 1752 us | 4 / 2000 |
| Restored: on / on | 540 us | 1695 us | 2 / 2000 |

The corresponding 1 ms OP target results were:

| Configuration | Estimated mean interval | Worker CPU, one core | WKC errors / cycles |
| --- | ---: | ---: | ---: |
| Original | 1.42 ms | 37.8% | 10 / 3514 (0.28%) |
| Interrupt Moderation off | 1.08 ms | 10.6% | 28 / 4636 (0.60%) |
| EEE off | 1.43 ms | 35.9% | 29 / 3502 (0.83%) |
| Both off | 1.10 ms | 9.7% | 23 / 4568 (0.50%) |
| Restored | 1.44 ms | 41.5% | 29 / 3483 (0.83%) |

Disabling Interrupt Moderation substantially reduced typical receive delivery time and polling CPU cost; restoring it reproduced the original latency range. EEE alone showed no clear benefit on this link, and disabling both did not establish an additional benefit. At the 500 us OP target, Interrupt Moderation off changed the estimated mean interval from 1.23 to 0.87 ms and CPU from 40.6% to 15.6%; the target was still missed substantially.

WKC failures, RTT p99 and timeout counts did not consistently improve. These short sequential tests establish a useful latency/CPU setting for this machine, not deterministic timing or a complete stability fix. Estimated mean intervals use duration/counter deltas and have a small stop-command boundary bias. The probe's maximum-jitter field also includes pre-sampling output checks, so it is not a precise 5-second-window statistic and is omitted above.

Adapter restarts intentionally interrupt the link and can increase ESC lost-link counters. Such cross-configuration increases must not be treated as spontaneous failures during a steady run; inspect counter deltas within each run. Forwarded RX counters need a separate interpretation, explained below. Original settings were restored after this A/B group, and backups/settings/logs were retained under `build/communication-diagnostics/admin-20261114-140501/`.

## Current Controller: 30-second runs

The current Controller was then tested with Interrupt Moderation off, EEE on, Normal worker priority and no spin prototype. Each row is an independent 30-second run, without GUI rendering or scripts. Both slaves were observed in OP (state 8), AL code 0; no automatic stop occurred.

| Mapping | Target | Estimated mean interval | Worker CPU, one core | WKC errors / cycles | Skipped target periods |
| --- | ---: | ---: | ---: | ---: | ---: |
| Normal | 5 ms | 5.043 ms | 6.09% | 22 / 5949 | 51 |
| Normal | 1 ms | 1.082 ms | 14.06% | 137 / 27725 | 2296 |
| Normal | 500 us | 0.860 ms | 15.41% | 160 / 34899 | 25140 |
| Overlapped | 5 ms | 5.034 ms | 6.98% | 31 / 5960 | 40 |
| Overlapped | 1 ms | 1.095 ms | 13.23% | 155 / 27393 | 2630 |
| Overlapped | 500 us | 0.869 ms | 15.21% | 205 / 34539 | 25493 |

At 5 ms, average timing was close to the target, but exchanges still failed occasionally. Neither mapping mode sustained the 500 us target, and overlapped mapping did not establish a timing or stability improvement. The mean-interval and maximum-jitter sampling limitations described above also apply here.

ESC reads bracketed each run, including the output checks and state transitions. In the three normal-mapping runs, slave 2's `0x0308` counter changed 1→66, 0→5 and 0→3; its lost-link counters stayed unchanged. In all three overlapped runs, the forwarded counter stayed 0→0 and the lost-link counters stayed unchanged, yet WKC failures remained. These counters do not establish which EtherCAT exchanges failed or identify mapping as the cause. The original Interrupt Moderation / EEE values were restored to on / on after testing. Logs are `im-long-mapping-0.log` and `im-long-mapping-1.log` in the diagnostics directory above.

## Interpreting ESC error counters

SOEM sets DL Control `0x0100[0]` to 1 on the first slave and 0 on following slaves; read-only checks confirmed these values on this bus. With this bit set, an ESC destroys non-EtherCAT frames and counts an EPU error. A downstream ESC can count the marked frame as a forwarded RX error. A forwarded count alone therefore does not demonstrate a damaged cable or failed EtherCAT frame. [Beckhoff ESC Technology, §§14.1–14.2](https://download.beckhoff.com/download/document/io/ethercat-development-products/ethercat_esc_datasheet_sec1_technology_v2.5.pdf).

A separate 15-second capture found 16 IPv4 and 12 IPv6 frames, with zero capture drops; it was not part of the performance measurements. The first slave's EPU counter `0x030C` was already saturated at `0xFF`, preventing correlation of further increments. The observed downstream forwarded counts are consistent with destruction of ordinary non-EtherCAT traffic, but that explanation has not been proved frame by frame. Sampled `0x0300–0x0307` and NIC error counters remained zero; there is currently no direct evidence of physical damage. The source of the remaining WKC failures is unresolved.

## Adapter binding checks and discovery fix

With Interrupt Moderation off, disabling only the two TwinCAT bindings (`beckhoff_tcether`, `tcrtefilter`) produced mixed results in separate 10-second OP runs at 5 ms, 1 ms and 500 us. The 1 ms RTT p50 changed from 123 to 163 us, while p99 changed from 1344 to 1278 us. These short observations do not establish a stable benefit or demonstrate a TwinCAT driver conflict.

The first IPv4/IPv6-off trial exposed a software bug: the raw RTT probe still succeeded on all 2,000 requests at a 1 ms interval, but the Controller could no longer enumerate the Intel NIC. Its Windows filter depended on IPv4 adapter enumeration. It now resolves the Npcap GUID through LUID / `GetIfEntry2`, independently of IP bindings. The initial trial has no valid OP comparison. The fix passed all 7 CTest checks and the subsequent real-device scans.

After the fix, baseline → IPv4/IPv6 bindings off → bindings restored were tested with Interrupt Moderation off, EEE on and normal mapping/default worker scheduling. Windows also automatically disabled the dependent `ms_server`, `ms_netbt`, `ms_netbios` and `ms_msclient` bindings during the IP-off phase; both TwinCAT bindings and Npcap remained enabled. Thus this phase tests removal of the IP stack and its dependent bindings together. Each OP target ran for 10 seconds, without GUI or script load. Each phase scanned both slaves in PRE-OP with AL code 0 and expected WKC 6; both were observed in OP with AL code 0 during the OP runs, with no automatic stop. The table shows the 1 ms RTT interval and 1 ms OP target; RTT quantiles include successful requests only.

| Configuration | RTT p50 / p99 | Timeouts / requests | Estimated OP mean interval | WKC abnormal cycles / cycles |
| --- | ---: | ---: | ---: | ---: |
| Baseline | 123 / 1075 us | 0 / 2000 | 1.081 ms | 24 / 9260 |
| IPv4/IPv6 off | 120 / 1451 us | 5 / 2000 | 1.117 ms | 51 / 8965 |
| Bindings restored | 122 / 879 us | 2 / 2000 | 1.079 ms | 31 / 9268 |

The 5 ms OP mean intervals were 5.026 / 5.033 / 5.028 ms; at 500 us they were 0.876 / 0.906 / 0.858 ms, in the same phase order. WKC abnormalities remained, so disabling IP did not solve cycle timing or exchange stability. Slave 2's forwarded-count increments at 5 ms / 1 ms / 500 us were 34 / 2 / 1 before disabling IP, 0 / 0 / 0 with IP off, and 3 / 0 / 0 after restoration; lost-link counters stayed unchanged within runs. This supports the background-frame explanation above, without proving it frame by frame or equating forwarded counts with failed EtherCAT cycles.

Final verification found no differences from the original 47 advanced settings or 21 bindings. Interrupt Moderation and EEE were both back on, link status was Up at 100 Mbps, NIC RX/TX errors and discards were zero, and the power plan remained Balanced. The product change is the IP-independent discovery fix; no experimental timing policy was enabled. Complete `fixed-bindings-baseline-*`, `fixed-ip-bindings-off-*`, `fixed-bindings-restored-*` logs and `final-verification.json` are in the diagnostics directory above.

## Software trials with Interrupt Moderation disabled

Normal/Highest worker priority, an additional `timeBeginPeriod(1)` and High QoS were compared again with Interrupt Moderation off. Each of the five target periods ran for 5 seconds. No consistent additional benefit emerged; estimated mean intervals at the 500 us target remained approximately 0.85–0.89 ms.

A separate prototype linked the same backend library to an experimental copy of the Controller. It woke the blocking timer up to 50/100/200 us early, then used a bounded spin until the original deadline; the requested bus deadline was unchanged. At the 500 us target, a 200 us spin allowance changed the estimated mean interval from 0.922 ms with no spin to 0.588 ms, while worker CPU rose from 16.5% to 25.3% of one core. A final no-spin repeat returned to 0.896 ms / 15.3% CPU. The 200 us sample still recorded 1,532 skipped periods and 18 WKC errors in 8,517 cycles over 5 seconds.

This prototype remains under `build/` and is not a release default or a public product option. These sequential short runs indicate a timing/CPU trade-off, not a 500 us guarantee. GUI rendering and script load were not included, and the remaining WKC failures prevent claiming a complete stability fix.

## Tune in this order

1. Dedicate a physical wired NIC to EtherCAT and connect directly to the first slave. A 100 Mbps link is normal; keep working speed/duplex negotiation instead of forcing a speed.
2. Test **Interrupt Moderation = Disabled** first. Batching interrupt notifications increases latency. Disabling it can increase interrupt-processing CPU cost, although this machine's I/O worker spent less CPU time polling for delivery. Validate the trade-off locally. [Microsoft mechanism](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/interrupt-moderation), [low-latency guidance](https://learn.microsoft.com/en-us/windows-server/networking/technologies/network-subsystem/net-sub-performance-tuning-nics).
3. Test **EEE = Disabled** separately. Wake-up can add delay, but EEE requires both ends to support and negotiate it; an enabled option alone proves nothing. DMA Coalescing, if exposed by the driver, is another candidate. [Intel EEE guide](https://edc.intel.com/output/DownloadPdfDocument?id=7604), [DMA Coalescing](https://edc.intel.com/content/www/us/en/design/products/ethernet/adapters-and-devices-user-guide/30.1/direct-memory-access-dma-coalescing/).
4. Compare AC power and a high-performance power plan for remaining latency spikes. Do not use Windows **Realtime** process priority or `THREAD_PRIORITY_TIME_CRITICAL`. [Windows priorities](https://learn.microsoft.com/en-us/windows/win32/procthread/scheduling-priorities).

TCP/UDP offloads, TCP windows and TCP-port/PSH-based Low Latency Interrupt rules do not optimize EtherCAT raw frames. Avoid blanket offload/registry recipes. [Intel setting definitions](https://www.intel.com/content/www/us/en/support/articles/000005783/processors.html).

## Windows helper

From the repository root, inspect the selected adapter first. Replace the example name with the exact dedicated NIC name:

```powershell
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Inspect
```

`Inspect` is the read-only default and shows both settings. `Apply` changes only Interrupt Moderation by default; `-Setting EEE` changes EEE alone. `Apply`/`Restore` require administrator PowerShell; `Restore` requires `-BackupPath`. Disconnect the bus first: both operations restart the selected NIC. Keep each backup.

Test Interrupt Moderation first, then restore the baseline before testing EEE with a different backup:

```powershell
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Apply -BackupPath './ethercat-im-backup.json'
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Restore -BackupPath './ethercat-im-backup.json'
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Apply -Setting EEE -BackupPath './ethercat-eee-backup.json'
./scripts/tune-windows-ethercat.ps1 -AdapterName '以太网 2' -Mode Restore -BackupPath './ethercat-eee-backup.json'
```

`-Setting InterruptModeration,EEE` applies both settings together. `Restore` follows the backup and ignores `-Setting`. Unsupported settings are skipped. Review the action output; combined changes cannot identify which setting helped. This source-only helper never runs automatically. [Microsoft adapter-property API](https://learn.microsoft.com/en-us/powershell/module/netadapter/set-netadapteradvancedproperty).

Change one item at a time and preserve its original value. Repeat identical samples, pauses, cycles and load; record RTT p50/p99/max, send/receive split, WKC failures, missed cycles, jitter and CPU usage.

## Platform boundaries

Windows/Npcap suits scanning, signal tests and automation. Immediate, nonblocking EtherCAT-only capture is already configured. Larger capture buffers primarily address overflow; they do not establish lower RTT or deterministic cycles. [Npcap API](https://npcap.com/guide/npcap-api.html).

For sub-millisecond requirements, compare the same probe on Linux and evaluate PREEMPT_RT plus NIC/worker/IRQ tuning. PREEMPT_RT reduces scheduling latency, not NIC delays; Ubuntu x86/ARM still need hardware validation. A busy-polling FIFO worker must not starve its NIC IRQ thread. [PREEMPT_RT](https://www.kernel.org/doc/html/latest/core-api/real-time/theory.html), [e1000e](https://docs.kernel.org/networking/device_drivers/ethernet/intel/e1000e.html), [Ubuntu IRQ affinity](https://ubuntu.com/real-time/docs/how-to/tune-irq-affinity/).

Dedicated Windows real-time environments/direct NIC drivers add deployment, hardware qualification and possibly licensing costs. Switching standard NDIS capture/filter implementations alone does not guarantee determinism. [Beckhoff RT requirements](https://infosys.beckhoff.com/content/1033/tc3_overview/9309844363.html), [acontis architectures](https://www.acontis.com/en/ethercat-master-windows.html).

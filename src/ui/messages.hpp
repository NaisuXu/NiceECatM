// Copyright (C) 2026 Naisu Xu
// SPDX-License-Identifier: GPL-3.0-only

#pragma once
#include <string>
#include <utility>
#include <unordered_map>

namespace niceecatm::detail {
// Stable tool messages only. Device-returned strings and the SOEM error suffix
// after " | " are intentionally opaque and never translated by substring.
inline const std::pair<const char*,const char*> messageTranslations[]={
    {"Unspecified slave error.","从站未提供具体错误原因。"},
    {"File not found; check the device filename.","文件不存在或文件名不受支持，请检查从站文件名。"},
    {"File access denied.","文件访问被拒绝。"},
    {"Slave storage or memory is full.","从站存储空间或内存已满。"},
    {"Illegal FoE operation.","从站不支持此 FoE 操作。"},
    {"Invalid FoE packet number.","FoE 包号无效。"},
    {"File already exists.","文件已存在。"},
    {"No user is available.","没有可用用户。"},
    {"FoE requires BOOT state.","此 FoE 操作要求从站处于 BOOT 状态。"},
    {"File is not permitted in BOOT; check the device filename.","BOOT 状态下不允许访问此文件，请检查从站文件名。"},
    {"Invalid password or insufficient rights.","密码错误或权限不足。"},
    {"Slave programming failed.","从站编程失败。"},
    {"File checksum is invalid.","文件校验和无效。"},
    {"Firmware is incompatible with the slave hardware.","固件与从站硬件不兼容。"},
    {"File header is missing or invalid.","文件头缺失或无效。"},
    {"Slave flash access failed.","从站 Flash 访问失败。"},
    {"File format is incompatible.","文件格式不兼容。"},
    {"Unknown or vendor-specific error; consult the slave documentation.","未知或厂商自定义错误，请查阅从站文档。"},
    {"FoE is unavailable in Programming mode.","编程模式下不能使用 FoE。"},
    {"Stop the script before FoE transfer.","FoE 传输前请先停止脚本。"},
    {"Connect and scan before FoE transfer.","FoE 传输前请先连接并扫描。"},
    {"Scan and select a slave before FoE transfer.","FoE 传输前请先扫描并选择从站。"},
    {"Switch all slaves to PRE-OP and stop cyclic I/O before FoE transfer.","FoE 传输前请先将全部从站切换到 PRE-OP，并停止周期 I/O。"},
    {"Stop cyclic I/O and move all OP slaves to PRE-OP before FoE transfer.","FoE 传输前请先停止周期 I/O，并将所有 OP 从站切换到 PRE-OP。"},
    {"FoE transfer requires a selected slave with confirmed PRE-OP or BOOT and no AL error.","FoE 传输需要选中的从站已确认处于 PRE-OP 或 BOOT，且没有 AL 错误。"},
    {"FoE transfer cancelled before access.","FoE 传输已在访问前取消。"},
    {"Select a connected slave for FoE.","请选择已连接的从站进行 FoE 传输。"},
    {"Stop cyclic I/O before FoE transfer.","FoE 传输前请先停止周期 I/O。"},
    {"Move all OP slaves to PRE-OP before FoE transfer.","FoE 传输前请先将所有 OP 从站切换到 PRE-OP。"},
    {"FoE requires confirmed PRE-OP or BOOT without AL error.","FoE 要求已确认处于 PRE-OP 或 BOOT，且没有 AL 错误。"},
    {"Slave does not declare FoE support in the normal mailbox.","从站未在普通 Mailbox 中声明支持 FoE。"},
    {"Establish BOOT through the state toolbar before FoE transfer.","请先使用顶部状态按钮建立 BOOT 会话，再进行 FoE 传输。"},
    {"FoE mailbox sizes are invalid.","FoE Mailbox 大小无效。"},
    {"FoE cancelled before mailbox access.","FoE 已在访问 Mailbox 前取消。"},
    {"Slave mailbox did not become empty before the FoE request.","发送 FoE 请求前，从站 Mailbox 未能清空。"},
    {"FoE cancelled locally; accepted slave data is not rolled back.","FoE 已在本地取消，已被从站接收的数据不会回滚。"},
    {"FoE total timeout exceeded.","FoE 已超过总超时时间。"},
    {"FoE no-progress timeout exceeded.","FoE 已超过无进展超时时间。"},
    {"FoE mailbox send failed; the last packet may have reached the slave.","FoE Mailbox 发送失败，最后一个包可能已到达从站。"},
    {"FoE link or mailbox transport is unavailable.","FoE 链路或 Mailbox 传输不可用。"},
    {"FoE response exceeds the slave mailbox payload capacity.","FoE 响应超过从站 Mailbox 的有效载荷容量。"},
    {"Slave reports BUSY; waiting within the no-progress deadline.","从站报告 BUSY，正在无进展超时范围内等待。"},
    {"FoE response timeout; retry limit reached.","FoE 响应超时，已达到重试次数上限。"},
    {"FoE transfer completed.","FoE 文件传输完成。"},
    {"FoE transfer failed.","FoE 传输失败。"},
    {"Select a readable regular file for FoE download.","请选择可读取的普通文件用于 FoE 下载。"},
    {"FoE download exceeds the 128 MiB file limit or its size cannot be read.","FoE 下载文件超过 128 MiB 上限，或无法读取文件大小。"},
    {"Cannot open the local FoE download file.","无法打开本地 FoE 下载文件。"},
    {"Cannot prepare an immutable temporary FoE source file.","无法准备不可变的 FoE 临时源文件。"},
    {"Cannot prepare the complete FoE source file.","未能完整准备 FoE 源文件。"},
    {"Preparing immutable FoE source file.","正在准备不可变的 FoE 源文件。"},
    {"FoE source changed in size during preparation, or temporary source I/O failed.","准备期间 FoE 源文件大小发生变化，或临时源文件读写失败。"},
    {"Choose a local filename for FoE upload.","请为 FoE 上传选择本地保存文件名。"},
    {"Cannot open the temporary FoE upload stream.","无法打开 FoE 上传临时文件。"},
    {"Cannot allocate a temporary FoE upload filename.","无法分配 FoE 上传临时文件名。"},
    {"Cannot flush the received FoE file to disk.","无法将接收的 FoE 文件刷新到磁盘。"},
    {"Cannot close the received FoE file.","无法关闭接收的 FoE 文件。"},
    {"Cannot replace the destination with the completed FoE upload.","无法用完整接收的 FoE 文件替换目标文件。"},
    {"Expected a FoE ACK without data.","预期收到不带数据的 FoE ACK。"},
    {"FoE ACK packet number is out of sequence.","FoE ACK 包号顺序错误。"},
    {"Prepared FoE source has unexpected trailing data.","已准备的 FoE 源文件含有意外的尾部数据。"},
    {"Cannot read the expected bytes from the prepared FoE source file.","无法从已准备的 FoE 源文件读取预期字节。"},
    {"Waiting for final FoE ACK.","正在等待最后一个 FoE ACK。"},
    {"Sending FoE data.","正在发送 FoE 数据。"},
    {"Expected a FoE DATA response.","预期收到 FoE DATA 响应。"},
    {"FoE DATA packet number is out of sequence.","FoE DATA 包号顺序错误。"},
    {"FoE upload exceeds the configured maximum file size.","FoE 上传超过配置的最大文件大小。"},
    {"Cannot write the received FoE data to the local file.","无法将接收到的 FoE 数据写入本地文件。"},
    {"Acknowledging the final FoE data block.","正在确认最后一个 FoE 数据块。"},
    {"Receiving FoE data.","正在接收 FoE 数据。"},
    {"Preparing FoE transfer.","正在准备 FoE 传输。"},
    {"FoE requires valid send and receive mailbox payload capacities.","FoE 需要有效的 Mailbox 收发载荷容量。"},
    {"Device filename is empty, contains a null byte, or exceeds the mailbox payload capacity.","从站文件名为空、含有空字节或超过 Mailbox 有效载荷容量。"},
    {"Choose a local FoE file.","请选择本地 FoE 文件。"},
    {"FoE timeouts must be 1..3600000 ms and retry limit 0..20.","FoE 超时须为 1 至 3600000 毫秒，重试次数须为 0 至 20。"},
    {"FoE upload limit must be 1 byte through 128 MiB.","FoE 上传大小上限须为 1 字节至 128 MiB。"},
    {"Password must be an unsigned 32-bit decimal or hexadecimal number.","密码必须是无符号 32 位十进制或十六进制整数。"},
    {"Enter a device filename.","请输入从站文件名。"},
    {"Choose a local file or destination before starting.","开始前请选择本地文件或保存位置。"},
    {"FoE timeouts must be 1..3600 seconds and retries must be 0..20.","FoE 超时须为 1 至 3600 秒，重试次数须为 0 至 20。"},
    {"Cannot read the selected local file.","无法读取所选本地文件。"},
    {"FoE files must not exceed 128 MiB.","FoE 文件不能超过 128 MiB。"},
    {"The selected simulated slave does not support FoE.","所选模拟从站不支持 FoE。"},
    {"FoE transfer requires confirmed PRE-OP or BOOT.","FoE 传输要求已确认处于 PRE-OP 或 BOOT。"},
    {"Stop the script before entering BOOT.","进入 BOOT 前请先停止脚本。"},
    {"Stop all OP slaves before entering BOOT.","进入 BOOT 前请先停止所有 OP 从站。"},
    {"Stop cyclic I/O by switching all slaves to PRE-OP before BOOT.","进入 BOOT 前请先将全部从站切换到 PRE-OP，以停止周期 I/O。"},
    {"BOOT requires stopped cyclic I/O, no OP slaves, and no AL errors.","进入 BOOT 要求周期 I/O 已停止、没有 OP 从站且没有 AL 错误。"},
    {"SII does not provide valid, separate Bootstrap SM0/SM1 mailboxes.","SII 未提供有效且互不重叠的 Bootstrap SM0/SM1 Mailbox。"},
    {"BOOT confirmed individually; Bootstrap Mailbox configured. Scan again before restarting cyclic I/O.","已逐站确认 BOOT 并配置 Bootstrap Mailbox。重新启动周期 I/O 前请先重新扫描。"},
    {"AL Control requires BOOT, INIT, PRE-OP, SAFE-OP or OP.","AL Control 需要 BOOT、INIT、PRE-OP、SAFE-OP 或 OP。"},
    {"Use a regular BOOT/INIT/PRE-OP/SAFE-OP/OP value for AL Control","AL Control 请使用 BOOT、INIT、PRE-OP、SAFE-OP 或 OP 的普通状态值。"},
    {"Ready","就绪"}, {"Disconnected","已断开"}, {"Scanning","正在扫描"},
    {"Disconnect before changing operating mode.","请先断开连接再切换运行模式。"},
    {"Stop the script before changing operating mode.","请先停止脚本再切换运行模式。"},
    {"Unknown operating mode.","未知运行模式。"},
    {"Operating mode unchanged.","运行模式未改变。"},
    {"Simulation mode selected.","已选择模拟模式。"},
    {"Hardware mode selected.","已选择硬件模式。"},
    {"Programming mode selected; choose an interface and scan.","已选择编程模式，请选择网卡并扫描。"},
    {"Select Programming mode before EEPROM access.","访问 EEPROM 前请先选择编程模式。"},
    {"Stop the script before EEPROM access.","访问 EEPROM 前请先停止脚本。"},
    {"Scan in Programming mode before EEPROM access.","访问 EEPROM 前请先在编程模式下扫描。"},
    {"EEPROM access requires a scanned slave with confirmed INIT.","EEPROM 访问需要已扫描且确认处于 INIT 的从站。"},
    {"Confirm an even EEPROM capacity from 128 to 131072 bytes.","请确认 EEPROM 容量为 128 至 131072 字节之间的偶数。"},
    {"The requested range exceeds the target's declared EEPROM capacity.","请求范围超过目标声明的 EEPROM 容量。"},
    {"EEPROM operation cancelled before access.","EEPROM 操作已在访问前取消。"},
    {"EEPROM image configuration CRC is invalid.","EEPROM 镜像的配置区 CRC 无效。"},
    {"Ordinary workbench and script operations are unavailable in Programming mode.","编程模式下不能执行普通工作台和脚本操作。"},
    {"Stop the script before a programming scan.","编程扫描前请先停止脚本。"},
    {"Programming requires a single interface without PDO mapping.","编程模式需要单网口且不进行 PDO 映射。"},
    {"Checking programming target and EEPROM ownership","正在检查编程目标与 EEPROM 控制权"},
    {"Cancelled before EEPROM access","已在访问 EEPROM 前取消"},
    {"Writing EEPROM","正在写入 EEPROM"},
    {"Reading EEPROM","正在读取 EEPROM"},
    {"Reading back and comparing EEPROM","正在回读并比较 EEPROM"},
    {"Comparing full EEPROM readback","正在比较完整 EEPROM 回读内容"},
    {"Enumerating responding ESCs without PDO mapping","正在枚举响应的 ESC，不进行 PDO 映射"},
    {"Reading programming inventory","正在读取编程设备清单"},
    {"Programming inventory ready; INIT confirmed","编程设备清单已就绪，已确认 INIT"},
    {"Programming scan cancelled.","编程扫描已取消。"},
    {"EEPROM operation cancelled.","EEPROM 操作已取消。"},
    {"EEPROM operation cancelled; no EEPROM data was written.","EEPROM 操作已取消，尚未写入 EEPROM 数据。"},
    {"EEPROM written and fully verified; activation pending. Follow the device's reload or power-cycle procedure.","EEPROM 已写入且完整回读一致，等待生效，请按设备要求重新加载或重新上电。"},
    {"EEPROM read completed; all bytes were received successfully.","EEPROM 读取完成，全部字节均已成功接收。"},
    {"EEPROM read loaded for image export.","EEPROM 读取结果已加载，可导出镜像。"},
    {"EEPROM read is not a valid SII image; save the raw BIN to inspect it.","EEPROM 读取结果不是有效的 SII 镜像，可保存原始 BIN 后检查。"},
    {"The exported ESI must not overwrite the source image.","导出的 ESI 不得覆盖原始镜像文件。"},
    {"SII string is not valid XML 1.0 UTF-8","SII 字符串不是有效的 XML 1.0 UTF-8 文本。"},
    {"SII extended FMMU count does not match its FMMU category","SII 扩展 FMMU 数量与 FMMU 类别不一致。"},
    {"Cannot export an empty or zero-index SII PDO","无法导出空 PDO 或索引为零的 SII PDO。"},
    {"SII PDO references a missing or incompatible SyncManager","SII PDO 引用了不存在或不兼容的 SyncManager。"},
    {"Cannot export unsupported SII mailbox protocol flags","无法导出暂不支持的 SII Mailbox 协议标志。"},
    {"SII mailbox configuration requires MBoxOut and MBoxIn SyncManagers for ESI export","导出 ESI 时，SII Mailbox 配置必须包含 MBoxOut 和 MBoxIn SyncManager。"},
    {"SII mailbox header does not match its SyncManager category","SII Mailbox 头部配置与 SyncManager 类别不一致。"},
    {"SII mailbox SyncManagers require nonzero addresses and sizes for ESI export","导出 ESI 时，SII Mailbox SyncManager 的地址和大小必须非零。"},
    {"Programming target SII identity changed since scanning; scan again.","编程目标的 SII 标识在扫描后发生变化，请重新扫描。"},
    {"EEPROM status read failed (WKC must be 1).","EEPROM 状态读取失败（WKC 必须为 1）。"},
    {"EEPROM busy timeout.","等待 EEPROM 空闲超时。"},
    {"EEPROM error acknowledgement failed (WKC must be 1).","EEPROM 错误确认失败（WKC 必须为 1）。"},
    {"EEPROM error status did not clear.","EEPROM 错误状态未清除。"},
    {"Cannot acquire EEPROM ownership (WKC must be 1).","无法取得 EEPROM 控制权（WKC 必须为 1）。"},
    {"EEPROM master ownership was not confirmed.","未确认主站取得 EEPROM 控制权。"},
    {"EEPROM ownership restoration could not be confirmed.","无法确认 EEPROM 控制权已恢复。"},
    {"EEPROM command failed (WKC must be 1).","EEPROM 命令失败（WKC 必须为 1）。"},
    {"EEPROM data read failed (WKC must be 1).","EEPROM 数据读取失败（WKC 必须为 1）。"},
    {"EEPROM write data staging failed (WKC must be 1).","EEPROM 待写入数据提交失败（WKC 必须为 1）。"},
    {"The selected file could not be saved completely.","所选文件未能完整保存。"},
    {"Cannot open the selected output file.","无法打开所选输出文件。"},
    {"Stopped","已停止"}, {"Running","周期交换中"}, {"Connected","已连接"},
    {"Script: Script stopped before disconnect","脚本：断开连接前已停止脚本。"},
    {"No devices found","未发现设备"}, {"Adapter list refreshed","网卡列表已刷新"},
    {"Simulation ready","模拟器就绪"}, {"Not connected","尚未连接"},
    {"Command queue is full","命令队列已满"},
    {"Stale command rejected","连接代次已过期，命令已拒绝"},
    {"Stale command rejected: the connection generation changed.","连接代次已改变，过期命令已拒绝。"},
    {"Queued command canceled by a stop or disconnect request.","排队命令已被停止或断开请求取消。"},
    {"Disconnected; queued commands from the previous connection are invalid.","连接已断开，上一连接的排队命令已失效。"},
    {"Unknown command.","未知命令。"},
    {"Unknown backend error; connection closed.","后端发生未知错误，连接已关闭。"},
    {"Simulation mode. Hardware is accessed only after selecting real hardware and scanning.","当前为模拟模式，仅在选择硬件并扫描后访问硬件。"},
    {"I/O worker ready. Hardware traffic begins only after selecting an interface and scanning.","I/O 工作线程已就绪，选择网卡并扫描后才会产生真实硬件通信。"},
    {"Built-in simulator / no hardware access","内置模拟器 / 不访问真实硬件"},
    {"Two simulated adapters available.","两张模拟网卡可用。"},
    {"Simulated adapter A","模拟网卡 A"}, {"Simulated adapter B","模拟网卡 B"},
    {"Redundancy requires two different adapters.","冗余模式需要两张不同网卡。"},
    {"Unsupported target cycle.","不支持此目标周期。"},
    {"Target cycle can be changed only while all slaves are INIT or PRE-OP","目标周期只能在所有从站处于 INIT 或 PRE-OP 时修改"},
    {"Target cycle updated while the bus is stopped","总线停止时已更新目标周期"},
    {"Simulation scan complete: 5 slaves, PRE-OP, IN 6 B / OUT 20 B. No real hardware was accessed.","模拟扫描完成：5 台从站，PRE-OP，输入 6 B / 输出 20 B。未访问真实硬件。"},
    {"Scan devices first.","请先扫描设备。"}, {"Slave does not exist.","从站不存在。"},
    {"Unsupported AL state.","不支持此 AL 状态。"},
    {"Mapping is invalid; scan devices again.","映射已失效，请重新扫描设备。"},
    {"Stop cyclic exchange before lowering state.","降低状态前请先停止周期交换。"},
    {"Scan and map devices first.","请先扫描并映射设备。"},
    {"All slaves must be SAFE-OP or OP before cyclic exchange.","开始周期交换前，全部从站必须处于 SAFE-OP 或 OP。"},
    {"Simulated cyclic exchange started.","模拟周期交换已启动。"},
    {"Simulated cyclic exchange running; no real hardware accessed.","模拟周期交换中，未访问真实硬件。"},
    {"Simulated cyclic exchange stopped; OP slaves are SAFE-OP.","模拟周期交换已停止，OP 从站已转为 SAFE-OP。"},
    {"Stop cyclic exchange before register access.","操作寄存器前请先停止周期交换。"},
    {"Stop cyclic exchange before synchronous SDO access.","同步访问 SDO 前请先停止周期交换。"},
    {"Stop cyclic exchange before refreshing adapters.","刷新网卡前请先停止周期交换。"},
    {"Stop cyclic exchange before scanning devices.","扫描设备前请先停止周期交换。"},
    {"Register range must be 1–256 bytes within 0x0000–0x0FFF.","单次寄存器范围须为 1…256 字节，位于 0x0000…0x0FFF 内。"},
    {"Simulated register read complete.","模拟寄存器读取完成。"},
    {"AL Control requires INIT, PRE-OP, SAFE-OP or OP.","AL Control 值必须为 INIT、PRE-OP、SAFE-OP 或 OP。"},
    {"Writable simulated registers: AL Control (0x0120, 2 B) and SM configuration (0x0800–0x08FF).","模拟设备可写寄存器为 AL Control（0x0120，2 B）及 SM 配置（0x0800…0x08FF）。"},
    {"SM configuration is writable in INIT / PRE-OP only.","SM 配置仅在 INIT / PRE-OP 可写。"},
    {"Simulated SM write complete; mapping invalidated. Rescan before process exchange.","模拟 SM 写入完成，映射已失效；开始过程交换前请重新扫描。"},
    {"Slave does not declare CoE support.","从站未声明 CoE 支持。"},
    {"SDO access requires PRE-OP, SAFE-OP or OP.","访问 SDO 需要 PRE-OP、SAFE-OP 或 OP。"},
    {"SDO capacity must be 1–1048576 bytes.","SDO 接收容量必须为 1…1048576 字节。"},
    {"SDO response exceeds receive capacity.","SDO 响应超出接收容量。"},
    {"Simulated SDO read complete.","模拟 SDO 读取完成。"},
    {"Simulated SDO write complete.","模拟 SDO 写入完成。"},
    {"SDO Abort 0x06010002: object is read-only (simulation).","SDO Abort 0x06010002：对象只读（模拟）。"},
    {"SDO Abort 0x06020000: object does not exist (simulation).","SDO Abort 0x06020000：对象不存在（模拟）。"},
    {"SDO Abort 0x06070010: data type length mismatch (simulation).","SDO Abort 0x06070010：数据类型长度不匹配（模拟）。"},
    {"Output preparation requires PRE-OP, SAFE-OP or OP.","准备输出需要 PRE-OP、SAFE-OP 或 OP。"},
    {"Write is outside the slave output image.","写入范围超出从站输出映像。"},
    {"Output image updated; transmission occurs on subsequent simulated cycles.","输出映像已更新，将在后续模拟周期发送。"},
    {"Output image preloaded locally; outputs are not yet applied by OP cycles.","输出映像已在本地预装，尚未通过 OP 周期应用。"},
    {"Hardware mode: choose an interface and scan","硬件模式：选择网卡并扫描"},
    {"Linux AF_PACKET raw Ethernet (network access permissions required)","Linux AF_PACKET 原始以太网（需要网卡访问权限）"},
    {"No network interfaces available","没有可用网卡"},
    {"Stop cyclic exchange before scanning","扫描前请先停止周期交换"},
    {"Select a primary network interface","请选择主网卡"},
    {"Same-bus redundancy requires two different network interfaces","同一总线冗余需要两张不同网卡"},
    {"Network interface identifier exceeds the supported length","网卡标识超出支持的长度"},
    {"Linux network interface names must be shorter than IFNAMSIZ","Linux 网卡名称长度必须小于 IFNAMSIZ"},
    {"Target cycle must be 500, 1000, 2000, 5000, 10000, 20000, 50000 or 100000 us","目标周期必须为 500、1000、2000、5000、10000、20000、50000 或 100000 μs"},
    {"SOEM configuration failed","SOEM 配置失败"},
    {"No EtherCAT slaves responded on the selected interface","所选网卡上没有 EtherCAT 从站响应"},
    {"Slaves discovered, but native mapping/transport extent could not be validated; cyclic exchange disabled","已发现从站，但未通过原生映射 / 传输范围校验，周期交换已禁用"},
    {"Overlapped mapping with a slave that blocks LRW is unsupported in this native build; select normal mapping and scan again","此原生版本不支持对禁止 LRW 的从站使用 Overlapped 映射；请选择普通映射并重新扫描"},
    {"EC_ERROR: general configuration error","EC_ERROR：一般配置错误"},
    {"EC_NOFRAME: no matching EtherCAT return frame was received","EC_NOFRAME：未收到匹配的 EtherCAT 返回帧"},
    {"EC_OTHERFRAME: received traffic did not match the requested EtherCAT frame","EC_OTHERFRAME：接收的报文与请求的 EtherCAT 帧不匹配"},
    {"EC_SLAVECOUNTEXCEEDED: slave count exceeds the compiled capacity","EC_SLAVECOUNTEXCEEDED：从站数量超出编译容量"},
    {"EC_TIMEOUT: EtherCAT request timed out","EC_TIMEOUT：EtherCAT 请求超时"},
    {"WKC_ZERO: frame returned without any slave processing the datagram","WKC_ZERO：帧已返回，但没有从站处理该数据报"},
    {"No connected slave at the requested position","指定位置没有已连接的从站"},
    {"Unsupported EtherCAT state request","不支持此 EtherCAT 状态请求"},
    {"Stop cyclic exchange before requesting INIT or PRE-OP","请求 INIT 或 PRE-OP 前请先停止周期交换"},
    {"Mapping is invalid; scan again before requesting SAFE-OP or OP","映射已失效，请重新扫描后再请求 SAFE-OP 或 OP"},
    {"Scan and validate process mapping before starting","启动前请先扫描并校验过程映射"},
    {"All slaves must be SAFE-OP or OP before starting cyclic exchange","启动周期交换前，全部从站必须处于 SAFE-OP 或 OP"},
    {"Cyclic exchange started; actual AL states remain independent","周期交换已启动，各站实际 AL 状态独立显示"},
    {"Cyclic exchange stopped","周期交换已停止"},
    {"Cyclic exchange stopped; OP slaves confirmed SAFE-OP","周期交换已停止，OP 从站已确认转为 SAFE-OP"},
    {"Select a connected slave","请选择已连接的从站"},
    {"Register read must be 1..256 bytes within ESC address space","单次寄存器读取须为 1…256 字节，位于 ESC 地址空间内"},
    {"Stop cyclic exchange before synchronous ESC access in this build","此版本同步访问 ESC 前需停止周期交换"},
    {"ESC read timed out: no slave working counter","ESC 读取超时：未收到从站工作计数器"},
    {"ESC read completed","ESC 读取完成"},
    {"Invalid register write length","寄存器写入长度无效"},
    {"Use a regular INIT/PRE-OP/SAFE-OP/OP value for AL Control","AL Control 请使用常规 INIT / PRE-OP / SAFE-OP / OP 值"},
    {"Register is read-only or its write permissions are unknown; writable ranges: AL Control, FMMU, SM and watchdog timers","寄存器只读或写权限未知；可写范围为 AL Control、FMMU、SM 及看门狗定时器"},
    {"Configuration register writes require INIT or PRE-OP","写入配置寄存器需要 INIT 或 PRE-OP"},
    {"ESC write timed out; actual result is unknown","ESC 写入超时，实际结果未知"},
    {"ESC configuration written; process mapping invalidated. Scan again to rebuild","ESC 配置已写入，过程映射已失效；请重新扫描以重建"},
    {"ESC register write acknowledged","ESC 寄存器写入已收到确认"},
    {"Stop cyclic exchange before synchronous SDO access in this build","此版本同步访问 SDO 前需停止周期交换"},
    {"SDO receive capacity must be 1..65536 bytes","SDO 接收容量须为 1…65536 字节"},
    {"Slave does not declare CoE support","从站未声明 CoE 支持"},
    {"Stop cyclic exchange before reading the SDO dictionary.","读取 SDO 对象字典前请先停止周期交换。"},
    {"Online SDO dictionary discovery requires confirmed PRE-OP.","读取在线 SDO 对象字典需要确认的 PRE-OP 状态。"},
    {"Slave does not declare SDO Information support.","从站未声明支持 SDO Information。"},
    {"Cannot confirm the selected slave's AL state for dictionary discovery.","无法确认所选从站的 AL 状态，不能读取对象字典。"},
    {"Online SDO dictionary discovery is unavailable.","在线 SDO 对象字典读取尚不可用。"},
    {"Online dictionary result expired; refresh to try again.","在线字典读取结果已过期，请刷新重试。"},
    {"Discovery budget reached; refresh to retry.","已达到字典读取时间或请求数限制，请刷新重试。"},
    {"SDO Information request failed or timed out.","SDO Information 请求失败或超时。"},
    {"Object list exceeds the master capacity.","对象列表超出主站容量。"},
    {"Object count limit reached.","已达到对象数量限制。"},
    {"Invalid or duplicate object index.","对象索引无效或重复。"},
    {"Unsupported object description code.","不支持的对象描述类别。"},
    {"Object has no available entry descriptions.","对象没有可用的条目描述。"},
    {"Discovery stopped before all objects were described.","尚未完成全部对象描述，字典读取已停止。"},
    {"SDO requires confirmed PRE-OP, SAFE-OP or OP","SDO 需要确认的 PRE-OP、SAFE-OP 或 OP 状态"},
    {"SDO read failed or timed out","SDO 读取失败或超时"},
    {"SDO response length exceeds the requested buffer","SDO 响应长度超出请求的缓冲区"},
    {"SDO write must contain 1..65536 bytes","SDO 写入必须包含 1…65536 字节"},
    {"SDO write failed or timed out; actual write result may be unknown","SDO 写入失败或超时，实际写入结果可能未知"},
    {"SDO write acknowledged","SDO 写入已收到确认"},
    {"SDO mapping object written; scan again to rebuild process mapping","SDO 映射对象已写入，请重新扫描以重建过程映射"},
    {"Process mapping is invalid; scan again","过程映射已失效，请重新扫描"},
    {"Output write is outside the slave's process image","写入范围超出从站的过程映像"},
    {"Output writes require confirmed PRE-OP, SAFE-OP or OP","写入输出需要确认的 PRE-OP、SAFE-OP 或 OP 状态"},
    {"Output pointer is outside the validated mapping","输出指针超出已校验的映射范围"},
    {"Output image committed for the next valid cycle","输出映像已提交，将在下一个有效周期发送"},
    {"Local output image preloaded; not applied through an OP cycle","本地输出映像已预装，尚未通过 OP 周期应用"},
    {"Process inputs stale: working counter below expected","过程输入已过期：工作计数器低于期望值"},
    {"Cyclic exchange running; received working counter verified","周期交换中，接收工作计数器已验证"},
    {"Process data failed continuously for 2 s; exchange stopped. Inspect state and explicitly restart","过程数据连续 2 秒失败，交换已停止；请检查状态并手动重新启动"},
    {"No PDOs mapped; read-only AL-status heartbeat running","没有 PDO 映射，正在执行只读 AL 状态心跳检查"},
    {"Capture driver disabled by NICEECATM_DISABLE_PCAP","抓包驱动已被 NICEECATM_DISABLE_PCAP 禁用"},
    {"Capture DLL is incompatible: required pcap API entry point missing","抓包 DLL 不兼容：缺少必要的 pcap API 入口"},
    {"Npcap / WinPcap DLL missing or could not be loaded; install Npcap (WinPcap API compatibility)","Npcap / WinPcap DLL 缺失或无法加载，请安装启用 WinPcap API 兼容的 Npcap"},
    {"Npcap / WinPcap not loaded","尚未加载 Npcap / WinPcap"},
    {"HEX bytes require complete pairs, for example 01 FF 0A.","HEX 字节必须完整配对，例如 01 FF 0A。"},
    {"HEX bytes contain an invalid character.","HEX 字节含有无效字符。"},
    {"HEX data exceeds the 1 MiB editor limit.","HEX 数据超出编辑器的 1 MiB 上限。"},
    {"Enter at least one byte.","请至少输入一个字节。"},
    {"Bit field is outside the process image.","位字段超出过程映像。"},
    {"Value is outside the unsigned bit-field range.","数值超出无符号位字段范围。"},
    {"Enter a value.","请输入数值。"},
    {"REAL values require decimal input; use raw bytes for bit patterns.","REAL 值需要十进制输入；位模式请使用原始字节。"},
    {"Enter a finite decimal REAL value.","请输入有限的十进制 REAL 数值。"},
    {"Value exceeds the REAL32 range.","数值超出 REAL32 范围。"},
    {"HEX integer input must contain unsigned hexadecimal digits.","HEX 整数输入必须为无符号十六进制数字。"},
    {"HEX integer exceeds the data type's bit width.","HEX 整数超出数据类型位宽。"},
    {"Enter a whole decimal integer.","请输入完整的十进制整数。"},
    {"Integer is outside the signed data type range.","整数超出有符号数据类型范围。"},
    {"Enter a nonnegative whole decimal integer.","请输入非负的完整十进制整数。"},
    {"Integer is outside the unsigned data type range.","整数超出无符号数据类型范围。"},
    {"Integer is outside the data type range.","整数超出数据类型范围。"},
    {"Invalid REAL (NaN / Infinity)","无效 REAL（NaN / Infinity）"},
    {"Command queue is full.","命令队列已满。"},
    {"Use an offset in 0…65535 and a length in 1…32 bytes.","偏移范围为 0…65535，单次读取长度为 1…32 字节。"},
    {"Invalid ESC offset.","ESC 偏移无效。"},
    {"Write 1…32 bytes within the ESC address range.","请在 ESC 地址范围内写入 1…32 字节。"},
    {"The write must fit the selected slave's output image.","写入范围必须落在所选从站的输出映像内。"},
    {"Invalid bit offset.","位偏移无效。"},
    {"Invalid Index / SubIndex.","Index / SubIndex 无效。"},
    {"Read capacity must be 1…4096 bytes.","读取容量必须为 1…4096 字节。"},
    {"No matching ESI description","没有匹配的 ESI 描述"},
    {"ESI XML directory","ESI XML 搜索目录"},
    {"No XML files in the ESI directory","ESI 目录中没有 XML 文件"},
    {"ESI directory does not exist","ESI 搜索目录不存在"},
    {"Additional slaves with ESI diagnostics","其他存在 ESI 匹配问题的从站数量"},
    {"Stopped after three consecutive SDO Information request failures.","连续三次 SDO Information 请求失败，已停止查询描述。"},
    {"Simulation fixture","模拟数据"},
    {"Ambiguous ESI device identity","多个 ESI 描述具有相同设备标识，无法唯一匹配"},
    {"ESI revision mismatch","ESI 修订号不匹配"},
    {"ESI matched; online layout unavailable","ESI 已匹配；当前在线 PDO 布局不可用"},
    {"ESI matched; partial PDO description","ESI 已匹配；部分 PDO 条目缺少匹配描述"},
    {"ESI matched; PDO entries differ","ESI 已匹配；PDO 条目不一致"},
    {"ESI matched","ESI 已匹配"},
    {"ESI name exceeds 4096 bytes","ESI 名称超出 4096 字节上限"},
    {"Conflicting ESI names for one language","同一语言的 ESI 名称存在冲突"},
    {"Invalid UTF-8 in ESI file","ESI 文件中的 UTF-8 编码无效"},
    {"NUL byte in ESI file","ESI 文件含有 NUL 字节"},
    {"ESI DTD and external declarations are unsupported","不支持 ESI DTD 及外部声明"},
    {"Missing ESI device descriptions","ESI 缺少设备描述"},
    {"Too many ESI device descriptions","ESI 设备描述数量超出上限"},
    {"Missing ESI ProductCode","ESI 缺少 ProductCode"},
    {"Too many ESI PDO entries","ESI PDO 条目数量超出上限"},
    {"ESI PDO direction exceeds 64 KiB or contains an empty entry","ESI 单方向 PDO 超出 64 KiB 或包含空条目"},
    {"No devices in ESI file","ESI 文件中没有设备"},
    {"ESI library path is not a directory","ESI 描述库路径不是目录"},
    {"ESI library exceeds 1024 XML files","ESI 描述库超出 1024 个 XML 文件上限"},
    {"Cannot access ESI library directory","无法访问 ESI 描述库目录"},
    {"ESI library exceeds 16384 device descriptions","ESI 描述库超出 16384 条设备描述上限"},
    {"Duplicate ESI dictionary data type","ESI 对象字典数据类型重复"},
    {"Cyclic or excessively nested ESI dictionary type","ESI 对象字典类型存在循环引用或嵌套过深"},
    {"Duplicate ESI dictionary object index","ESI 对象字典索引重复"},
    {"Too many ESI dictionary entries","ESI 对象字典条目数量超出上限"},
    {"Duplicate ESI dictionary subindex","ESI 对象字典子索引重复"},
    {"Too many ESI dictionary objects","ESI 字典对象数量超出上限"},
    {"Process mapping invalid; scan again.","过程映射已失效，请重新扫描。"},
    {"PDO description time budget exceeded.","PDO 描述读取已超时。"},
    {"Online SoE signal descriptions are not available.","在线 SoE 信号描述暂不可用。"},
    {"Current CoE PDO assignment could not be read.","无法读取当前 CoE PDO 分配。"},
    {"Current CoE PDO entries could not be read.","无法读取当前 CoE PDO 条目。"},
    {"SII PDO descriptions could not be read.","无法读取 SII PDO 描述。"},
    {"Truncated SII PDO header.","SII PDO 头不完整。"},
    {"Invalid SII PDO entries.","SII PDO 条目无效。"},
    {"SII PDO limits exceeded.","SII PDO 数据超出上限。"},
    {"Zero-width SII PDO entry.","SII PDO 条目的位宽为零。"},
    {"PDO entry limit exceeded.","PDO 条目数量超出上限。"},
    {"Invalid PDO entry layout.","PDO 条目布局无效。"},
    {"PDO direction differs from the mapped SM.","PDO 方向与已映射的 SM 不一致。"},
    {"PDO entries differ from the mapped SM length.","PDO 条目总长度与已映射的 SM 长度不一致。"},
    {"Multiple SMs contain unsupported bit padding.","多个 SM 包含暂不支持的位填充。"},
    {"PDO entries differ from the actual process-image length.","PDO 条目总长度与实际过程映像长度不一致。"},
    {"Vendor ID","厂商 ID"},{"RevisionNo","RevisionNo"},{"PDO index","PDO 索引"},
    {"PDO SyncManager","PDO SyncManager"},{"entry index","条目索引"},{"entry subindex","条目子索引"},
    {"entry bit length","条目位宽"},{"name language","名称语言"},{"type bit size","类型位宽"},
    {"object index","对象索引"},{"dictionary subindex","字典子索引"},
    {"dictionary type bit size","字典类型位宽"},{"dictionary scalar width","字典标量位宽"},
    {"dictionary string length","字典字符串长度"},{"dictionary index","字典对象索引"},
    {"dictionary array bound","字典数组起始索引"},{"dictionary array length","字典数组长度"},
    {"dictionary array bit size","字典数组位宽"},{"dictionary entry bit size","字典条目位宽"},
};
inline std::string withoutPeriod(std::string value) {
    if(!value.empty()&&value.back()=='.')value.pop_back();
    return value;
}
inline std::string translateToolMessage(const std::string& message) {
    const auto separator=message.find(" | ");
    if(separator!=std::string::npos) {
        const std::string slaveMessage=" | Slave message: ";
        return translateToolMessage(message.substr(0,separator))+
            (message.compare(separator,slaveMessage.size(),slaveMessage)==0?
             " | 从站信息："+message.substr(separator+slaveMessage.size()):message.substr(separator));
    }
    if(message.rfind("EEPROM: ",0)==0)return "EEPROM："+translateToolMessage(message.substr(8));
    if(message.rfind("FoE: ",0)==0)return "FoE："+translateToolMessage(message.substr(5));
    const auto plain=withoutPeriod(message);
    static const auto exact=[](){std::unordered_map<std::string,std::string> result;for(const auto& entry:messageTranslations)result[withoutPeriod(entry.first)]=entry.second;return result;}();
    if(const auto entry=exact.find(plain);entry!=exact.end())return entry->second;
    if(message.rfind("FoE error ",0)==0) {
        const auto reason=message.find(": ",10);
        return "FoE 错误 "+(reason==std::string::npos?message.substr(10):
            message.substr(10,reason-10)+"："+translateToolMessage(message.substr(reason+2)));
    }
    const std::string bootPreflight="BOOT preflight failed for slave ";
    if(message.rfind(bootPreflight,0)==0) {
        const auto split=message.find(": ",bootPreflight.size());
        if(split!=std::string::npos)return "BOOT 预检查失败，从站 "+message.substr(bootPreflight.size(),split-bootPreflight.size())+"："+translateToolMessage(message.substr(split+2));
    }
    const std::string missingBootstrap=" has no valid bootstrap mailbox; BOOT was not requested.";
    if(message.rfind("Slave ",0)==0&&message.size()>missingBootstrap.size()&&message.compare(message.size()-missingBootstrap.size(),missingBootstrap.size(),missingBootstrap)==0)
        return "从站 "+message.substr(6,message.size()-missingBootstrap.size()-6)+" 没有有效的 Bootstrap Mailbox，未执行 BOOT 切换。";
    const std::string unavailableCoE=": CoE descriptions unavailable for ",objectCountSuffix=" mapped object(s).";
    if(message.rfind("Slave ",0)==0) {
        const auto split=message.find(unavailableCoE,6);
        if(split!=std::string::npos) {
            const auto start=split+unavailableCoE.size(),suffix=message.find(objectCountSuffix,start);
            if(suffix!=std::string::npos) {
                auto result="从站 "+message.substr(6,split-6)+" 有 "+message.substr(start,suffix-start)+" 个映射对象未能读取 CoE 描述。";
                const auto next=suffix+objectCountSuffix.size();
                if(next<message.size()&&message[next]==' ')result+=translateToolMessage(message.substr(next+1));
                return result;
            }
        }
    }
    for(const auto* prefix:{"Online SDO dictionary complete: ","Online SDO dictionary incomplete: "}) {
        const std::string start=prefix;if(message.rfind(start,0)!=0)continue;
        const auto objectsEnd=message.find(" objects, ",start.size());
        const auto entriesEnd=message.find(" entries.",objectsEnd);
        if(objectsEnd!=std::string::npos&&entriesEnd!=std::string::npos) {
            std::string value=std::string(start=="Online SDO dictionary complete: "?"在线 SDO 字典已读取：":"在线 SDO 字典读取不完整：")+
                message.substr(start.size(),objectsEnd-start.size())+" 个对象，"+message.substr(objectsEnd+10,entriesEnd-objectsEnd-10)+" 个条目。";
            const auto reasonStart=entriesEnd+9;
            if(reasonStart<message.size())value+=" "+translateToolMessage(message.substr(reasonStart+(message[reasonStart]==' '?1:0)));
            return value;
        }
    }
    if(message.rfind("IN: ",0)==0||message.rfind("OUT: ",0)==0) {
        const bool input=message.rfind("IN: ",0)==0;const size_t start=input?4:5;
        const auto next=message.find(input?" OUT: ":" IN: ",start);
        return std::string(input?"输入：":"输出：")+translateToolMessage(message.substr(start,next==std::string::npos?next:next-start))+
            (next==std::string::npos?"":" "+translateToolMessage(message.substr(next+1)));
    }
    if(message.rfind("ESI library: ",0)==0) {
        const auto filesEnd=message.find(" file(s), ",13),devicesEnd=message.find(" device description(s)",filesEnd);
        if(filesEnd!=std::string::npos&&devicesEnd!=std::string::npos) {
            std::string translated="ESI 描述库："+message.substr(13,filesEnd-13)+" 个文件，"+message.substr(filesEnd+10,devicesEnd-filesEnd-10)+" 条设备描述";
            const auto suffix=devicesEnd+22;
            if(suffix<message.size())translated+="；"+translateToolMessage(message.substr(suffix+(message.compare(suffix,2,"; ")==0?2:0)));
            return translated;
        }
    }
    // ESI diagnostics use a filename followed by a generated reason. Preserve
    // the filename and translate only that reason, including batched failures.
    if(const auto xml=message.find(".xml: ");xml!=std::string::npos)return message.substr(0,xml+6)+translateToolMessage(message.substr(xml+6));
    if(const auto xml=message.find(".XML: ");xml!=std::string::npos)return message.substr(0,xml+6)+translateToolMessage(message.substr(xml+6));
    if(message.rfind("Invalid ESI ",0)==0)return "ESI 字段无效："+translateToolMessage(message.substr(12));
    if(message.rfind("Missing ESI ",0)==0)return "ESI 缺少字段："+translateToolMessage(message.substr(12));
    const std::string scanPrefix="SOEM scan result ";
    if(message.rfind(scanPrefix,0)==0) {
        const auto reasonStart=message.find(" (",scanPrefix.size());
        const std::string hint="). Interface opened successfully; check slave power, the slave IN port, cable and link negotiation";
        const auto hintStart=message.find(hint,reasonStart==std::string::npos?0:reasonStart+2);
        if(reasonStart!=std::string::npos&&hintStart!=std::string::npos) {
            const auto code=message.substr(scanPrefix.size(),reasonStart-scanPrefix.size());
            const auto reason=message.substr(reasonStart+2,hintStart-reasonStart-2);
            return "SOEM 扫描结果 "+code+"（"+translateToolMessage(reason)+"）。网卡已成功打开；请检查从站电源、从站 IN 端口、线缆及链路协商"+message.substr(hintStart+hint.size());
        }
    }
    const std::pair<const char*,const char*> prefixes[]={
        {"Cannot replace the destination with the completed FoE upload: ","无法用完整接收的 FoE 文件替换目标文件："},
        {"BOOT configuration/state was not confirmed for slave ","未能确认 BOOT 配置或状态，从站 "},
        {"BOOT preflight could not confirm a live AL state for slave ","BOOT 预检查未能确认实际 AL 状态，从站 "},
        {"FoE cannot confirm a live AL state for slave ","FoE 未能确认实际 AL 状态，从站 "},
        {"Could not restore normal mailbox through INIT for slave ","未能经过 INIT 恢复普通 Mailbox，从站 "},
        {"EEPROM command rejected ","EEPROM 命令被拒绝 "},
        {"EEPROM command/write error ","EEPROM 命令或写入错误 "},
        {"EEPROM image prepared: ","EEPROM 镜像已准备："},
        {"EEPROM image loaded: ","EEPROM 镜像已加载："},
        {"EEPROM read saved: ","EEPROM 读取结果已保存："},
        {"EEPROM image exported: ","EEPROM 镜像已导出："},
        {"ESI description exported: ","ESI 描述已导出："},
        {"Cannot export SII PDO data type: ","无法导出的 SII PDO 数据类型："},
        {"Duplicate SII PDO index: ","SII PDO 索引重复："},
        {"Cannot export SII physical port type: ","无法导出的 SII 物理端口类型："},
        {"Cannot export SII FMMU function: ","无法导出的 SII FMMU 功能："},
        {"Cannot create ESI file: ","无法创建 ESI 文件："},
        {"Failed to completely write ESI file: ","ESI 文件未能完整写入："},
        {"Physical wired Ethernet interfaces enumerated: ","已枚举有线物理网卡数："},
        {"Cannot open programming interface: ","无法打开编程网卡："},
        {"Network interfaces enumerated: ","已枚举网卡数："},
        {"No capture interfaces available: ","没有可用抓包网卡："},
        {"Cannot open primary interface: ","无法打开主网卡："},
        {"Cannot open secondary interface; both interfaces were closed: ","无法打开副网卡，两张网卡均已关闭："},
        {"Backend error: ","后端错误："},{"I/O worker failed: ","I/O 工作线程失败："},
        {"Cyclic exchange failed: ","周期交换失败："},
        {"Simulated AL state confirmed: ","模拟 AL 状态已确认："},
        {"SDO read completed: ","SDO 读取完成："},
        {"Unsupported data type: ","不支持的数据类型："},
        {"Unsupported ESI encoding: ","不支持的 ESI 编码："},
        {"ESI file must contain 1 byte to 32 MiB: ","ESI 文件大小须为 1 字节至 32 MiB："},
        {"Cannot read ESI file: ","无法读取 ESI 文件："},
        {"ESI file changed while reading: ","读取时 ESI 文件发生变化："},
        {"Malformed ESI XML: ","ESI XML 格式无效："},
        {"Not an EtherCAT ESI file: ","该文件不是 EtherCAT ESI："},
        {"Exchange stopped; SAFE-OP confirmation failed. ","周期交换已停止，但未确认 SAFE-OP。"}
    };
    for(const auto& entry:prefixes){const std::string prefix=entry.first;if(message.rfind(prefix,0)==0){
        const auto rest=message.substr(prefix.size());
        return std::string(entry.second)+(prefix=="Exchange stopped; SAFE-OP confirmation failed. "?translateToolMessage(rest):rest);
    }}
    if(message.rfind("Scanned ",0)==0) {
        const auto end=message.find(" real EtherCAT slave(s); mapping prepared; cyclic exchange stopped");
        if(end!=std::string::npos)return "已扫描 "+message.substr(8,end-8)+" 台真实 EtherCAT 从站；映射已准备，周期交换已停止";
    }
    if(message.rfind("Confirmed ",0)==0) {
        const auto end=message.find(" for ");
        if(end!=std::string::npos){const auto who=message.substr(end+5);return "状态已确认："+message.substr(10,end-10)+"，"+(who=="all slaves"?"全部从站":who.rfind("slave ",0)==0?"从站 "+who.substr(6):who);}
    }
    if(message.rfind("State request was not confirmed",0)==0) {
        std::string result="状态请求尚未确认";const auto details=message.substr(31);
        // These labels are generated by the tool. Raw SOEM descriptions in
        // parentheses and address/state values remain unchanged.
        unsigned depth=0;
        for(size_t p=0;p<details.size();) {
            const char ch=details[p];
            if(ch=='(')++depth;else if(ch==')'&&depth)--depth;
            if(depth==0&&details.compare(p,8,"; slave ")==0){result+="；从站 ";p+=8;}
            else if(depth==0&&details.compare(p,8," actual ")==0){result+=" 实际 ";p+=8;}
            else{result+=ch;++p;}
        }
        return result;
    }
    return message;
}
} // namespace niceecatm::detail

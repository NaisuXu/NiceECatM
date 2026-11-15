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
    {"Ready","就绪"}, {"Disconnected","已断开"}, {"Scanning","正在扫描"},
    {"Stopped","已停止"}, {"Running","周期交换中"}, {"Connected","已连接"},
    {"No devices found","未发现设备"}, {"Adapter list refreshed","网卡列表已刷新"},
    {"Simulation ready","模拟器就绪"}, {"Not connected","尚未连接"},
    {"Command queue is full","命令队列已满"},
    {"Stale command rejected","连接代次已过期，命令已拒绝"},
    {"Stale command rejected: the connection generation changed.","连接代次已改变，过期命令已拒绝。"},
    {"Queued command canceled by a stop or disconnect request.","排队命令已被停止或断开请求取消。"},
    {"Disconnected; queued commands from the previous connection are invalid.","连接已断开，上一连接的排队命令已失效。"},
    {"Unknown command.","未知命令。"},
    {"Unknown backend error; connection closed.","后端发生未知错误，连接已关闭。"},
    {"Simulation mode. Hardware is accessed only after selecting real hardware and scanning.","当前为模拟模式，仅在选择真实设备并扫描后访问硬件。"},
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
    {"Hardware mode: choose an interface and scan","真实设备模式：选择网卡并扫描"},
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
};
inline std::string withoutPeriod(std::string value) {
    if(!value.empty()&&value.back()=='.')value.pop_back();
    return value;
}
inline std::string translateToolMessage(const std::string& message) {
    const auto separator=message.find(" | ");
    if(separator!=std::string::npos)return translateToolMessage(message.substr(0,separator))+message.substr(separator);
    const auto plain=withoutPeriod(message);
    static const auto exact=[](){std::unordered_map<std::string,std::string> result;for(const auto& entry:messageTranslations)result[withoutPeriod(entry.first)]=entry.second;return result;}();
    if(const auto entry=exact.find(plain);entry!=exact.end())return entry->second;
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

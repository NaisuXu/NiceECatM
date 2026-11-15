# ESI descriptions and signal names

**English** | [简体中文](esi.zh-CN.md) | [User guide](../README.md)

NiceECatM combines local EtherCAT Slave Information XML with the PDO layout discovered online. ESI adds names and scalar type information; process values come from the mapped SOEM image.

## Use

1. Place manufacturer XML files in `esi/` beside the executable.
2. Restart the application so its ESI catalog loads the current files.
3. Scan the intended adapter and select a slave in the topology.
4. Inspect BufferPDO. Signal details show the matched ESI filename.

Identity matching uses Vendor ID, Product Code and Revision. The package includes manufacturer XML files for the ASIX AX58100 and Microchip LAN9252 test devices in `esi/`.

Removing a file removes that ESI description on the next application start. Names may still be available independently from the device's SII strings or CoE SDO Info. A visible signal name by itself does not establish that an ESI file remains loaded.

## Layout matching

CoE devices are inspected through their current PDO assignments and mapping entries. SDO Info can supply names and types when it is declared and implemented. Other supported layouts use SII EEPROM PDO entries. Neither support for CoE nor a device model name proves that SDO Info is available.

ESI entries must match direction, PDO/object identity, subindex, bit length and compatible SyncManager metadata. Online order determines bit offsets. Equal total byte lengths alone are insufficient. Wrong revisions, conflicting descriptions or layout mismatches retain their diagnostic result rather than forcing a guessed layout.

Supported scalar fields include BOOL, signed/unsigned 1–64-bit integers, REAL32 and REAL64. IEC aliases such as INT, UDINT and LREAL and BITARR8/16/32 are normalized. Padding is read-only; unknown or unsupported string, array and structure regions remain raw byte/bit fields. Values use EtherCAT little-endian bit order.

## Language and scope

Localized names are selected when supplied. An English-only description stays English in the Chinese UI; there is no required alias or automatic translation. Original device strings and identifiers are retained.

ESI provides display metadata. It does not implement ESI-driven slave initialization, startup-command execution, PDO remapping or modular-slot configuration. A register or object write that invalidates mapping requires a new scan before typed PDO access and cyclic exchange.

Discovery is bounded and runs during scanning, outside the cyclic loop. CLI scan JSON includes the ESI directory/counts, per-slave match result and discovered `pdoEntries`. XML parsing is embedded and needs no additional XML runtime.

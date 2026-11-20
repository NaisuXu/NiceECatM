# ESI descriptions, signal names and object dictionaries

**English** | [简体中文](esi.zh-CN.md) | [User guide](../README.md)

NiceECatM combines local EtherCAT Slave Information XML with the PDO layout discovered online. ESI adds names and scalar type information; process values come from the mapped SOEM image.

The CoE · SDO page also displays the matched ESI's object dictionary when `Profile/Dictionary/Objects` is present. This dictionary is independent of the PDO layout: configuration and diagnostic objects do not need a PDO mapping to appear. Scalar objects, record subitems and common one-dimensional arrays include their index/subindex, names, type, bit length and declared access rights. Unsupported array layouts remain explicitly partial; no SDO addresses are inferred from PDO entries alone.

ESI metadata is an offline description. Its presence does not confirm that the connected firmware implements an object or CoE access, and default values in XML are never displayed as current readings. Online SDO Information can supply live object descriptions; object values still require an explicit SDO read. Offline descriptions supplement online metadata and are cleared when their matching file is absent at the next application start.

## Use

1. Place manufacturer XML files in `esi/` beside the executable.
2. Restart the application so its ESI catalog loads the current files.
3. Scan the intended adapter and select a slave in the topology.
4. Inspect BufferPDO. Signal details show the matched ESI filename.
5. For CoE slaves, inspect CoE · SDO for dictionary descriptions. Select an entry to fill the manual parameter editor. Online discovery requires SDO Information support and PRE-OP with cyclic exchange stopped; running cyclic I/O uses previously loaded descriptions.

Identity matching uses Vendor ID, Product Code and Revision. The package includes manufacturer XML files for the ASIX AX58100 and Microchip LAN9252 test devices in `esi/`.

The included AX58100 and LAN9252 XML files describe PDOs only and contain no SDO object dictionary. Use an ESI with a dictionary or a slave that supports online SDO Information to populate the dictionary list. Supporting ordinary SDO transfers alone does not guarantee enumeration support.

Removing a file removes that ESI description on the next application start. Names may still be available independently from the device's SII strings or CoE SDO Info. A visible signal name by itself does not establish that an ESI file remains loaded.

## Layout matching

CoE devices are inspected through their current PDO assignments and mapping entries. SDO Info can supply names and types when it is declared and implemented. Other supported layouts use SII EEPROM PDO entries. Neither support for CoE nor a device model name proves that SDO Info is available.

ESI entries must match direction, PDO/object identity, subindex, bit length and compatible SyncManager metadata. Online order determines bit offsets. Equal total byte lengths alone are insufficient. Wrong revisions, conflicting descriptions or layout mismatches retain their diagnostic result rather than forcing a guessed layout.

Supported scalar fields include BOOL, signed/unsigned 1–64-bit integers, REAL32 and REAL64. IEC aliases such as INT, UDINT and LREAL and BITARR8/16/32 are normalized. Padding is read-only; unknown or unsupported string, array and structure regions remain raw byte/bit fields. Values use EtherCAT little-endian bit order.

## EEPROM images

An ESI XML file is a description, not an EEPROM image. The **EEPROM** page is available only in **Programming** mode. It independently builds a binary Slave Information Interface (SII) image from a selected ESI Device, or imports an existing SII BIN. Loading, inspecting and exporting an image require no connection. Reading or programming a physical EEPROM requires a programming scan and confirmed INIT. EEPROM access uses the ESC EEPROM interface, not CoE/SDO or FoE.

Programming may intentionally change the Vendor ID or Product Code. Differences are shown in the existing write confirmation and do not block a write or require a separate unlock. The target is checked against its own scan snapshot; image validation and declared-capacity checks still apply. Matching identifiers alone do not establish hardware or firmware compatibility. This programming rule is separate from normal workbench ESI identity matching: [Microchip AN1910 §4.2](https://ww1.microchip.com/downloads/en/AppNotes/AN1910-Application-Note-DS00001920B.pdf) describes editing EEPROM identity before programming, and [Beckhoff ET9300 §14.2](https://download.beckhoff.com/download/Document/io/ethercat-development-products/an_et9300_v1i10.pdf) describes programming a selected slave from an ESI.

The native builder supports the fixed-layout features used by the bundled AX58100/LAN9252 files: ESC ConfigData, identity, bootstrap/standard mailbox configuration, Strings, General, FMMU, SyncManager, fixed PDO entries and the supported DC timing fields. BITARR types keep their EtherCAT IDs. It checks ranges, category/string references and configuration CRC, and fills unused image capacity with `0xFF`. Image strings use English when available, then an unqualified or first available name; switching the UI language never changes the bytes.

Modular layouts, custom PDO data types, Eeprom/Data or custom ESI categories, unsupported DC timing options and unknown image-affecting fields are rejected explicitly. Passive catalog support does not imply EEPROM-generation support. For descriptions outside this subset, import a manufacturer-provided or SSC/TwinCAT-exported BIN instead. BIN import preserves bounded unknown categories; a short image ending at a valid SII terminator is explicitly padded to its declared capacity. A valid structure or successful readback alone does not prove device-specific firmware compatibility or that a new configuration has taken effect.

## Language and scope

Localized names are selected when supplied. An English-only description stays English in the Chinese UI; there is no required alias or automatic translation. Original device strings and identifiers are retained.

The workbench ESI catalog provides display metadata. It does not implement ESI-driven slave initialization, startup-command execution, PDO remapping or modular-slot configuration. EEPROM image preparation is a separate explicit operation. A register or object write that invalidates mapping requires a new scan before typed PDO access and cyclic exchange.

PDO discovery runs during scanning. Online object dictionary discovery is a separate bounded operation in PRE-OP with cyclic exchange stopped; neither runs inside the cyclic loop. CLI scan JSON includes the ESI directory/counts, per-slave match result and discovered `pdoEntries`. XML parsing is embedded and needs no additional XML runtime.

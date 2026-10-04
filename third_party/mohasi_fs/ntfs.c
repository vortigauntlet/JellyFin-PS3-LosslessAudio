// ntfs.c - NTFS reader for the PS3 file manager. See ntfs.h.
//
// Sibling of exfat.c: same lv2 storage I/O layer, same memory discipline (all
// scratch is static or stack, no libc, no malloc), same VFS backend wiring, the
// same single non-recursive backend mutex. Hand-written from the NTFS on-disk
// spec in docs/ntfs; the read-path parsing is cross-checked against libfsntfs.
//
// NTFS is little-endian on disk and the PPU is big-endian, so every multi-byte
// on-disk field is read through readLe16/readLe32/readLe64 - a packed on-disk
// struct is never overlaid on a C struct and read by field.
//
// Build stages (see docs/ntfs/NTFS-BUILD-LOG.md): S0 scaffold + primitives,
// S1 boot sector / probe are implemented; S2-S7 (MFT, attributes, runlists,
// file read, directory traversal, path resolution) are stubbed and wired.

#include "ntfs.h"
#include "jf_port.h"           // jf-port: storage, timing and string helpers (replaces vfs/storage-device/syscall/thread/string-utilities/rtc)

#define STORAGE_BUSY       0x80010002u   // lv2 "device not ready" (settling / ejected)
#define SYSIO_RETRY        8
#define SYSIO_RETRY_US     50000
#define SYSIO_SETTLE_US    62500         // settle gap after open, before the first read

#define STORAGE_ALIGN      32            // lv2 storage DMA buffer alignment
#define NTFS_MAX_SECTOR    4096          // largest sector we support
#define NTFS_MAX_RECORD    4096          // largest FILE/index record we buffer (>= mftRecordSize)
#define NTFS_READ_BOUNCE   16384         // sector-aligned bounce for non-resident file reads (16 KB/call)
#define MFT_REF_MASK       0x0000FFFFFFFFFFFFull   // low 48 bits of an 8-byte file reference = MFT index

// Partition-table constants (mirror exfat.c; MBR + GPT layout is filesystem-agnostic).
#define MBR_PART_TABLE     446
#define MBR_PART_ENTRIES   4
#define MBR_PART_SIZE      16
#define MBR_TYPE_GPT       0xEE
#define GPT_HEADER_LBA     1
#define GPT_MAX_ENTRIES    128
#define GPT_ENTRY_MIN_SIZE 128
// Finite ceiling for partition-table LBAs when the device size is unknown, so a hostile partition
// entry can't steer a scan read to an arbitrary 64-bit sector (see exfat.c for the rationale).
#define NTFS_SCAN_LBA_CAP  (1ull << 36)

// ===========================================================================
// NTFS on-disk offsets, transcribed from the spec (docs/ntfs). All fields are
// little-endian. Offsets cited so a reader can check them against the spec.
// ===========================================================================

// $Boot / BPB (volume header), 512 bytes. Spec: "The volume header".
#define BOOT_OEM_OFFSET            3    // 8 bytes, "NTFS    "
#define BOOT_BYTES_PER_SECTOR      11   // uint16
#define BOOT_SECTORS_PER_CLUSTER   13   // uint8, signed power-of-two encoding (see decodeClusterFactor)
#define BOOT_TOTAL_SECTORS         40   // uint64
#define BOOT_MFT_LCN               48   // uint64
#define BOOT_MFTMIRR_LCN           56   // uint64
#define BOOT_MFT_RECORD_FACTOR     64   // int8, signed power-of-two encoding (see decodeRecordBytes)
#define BOOT_INDEX_RECORD_FACTOR   68   // int8, same encoding
#define BOOT_VOLUME_SERIAL         72   // uint64
#define BOOT_SIGNATURE             510  // 0x55, 0xAA

// MFT FILE record header. Spec: "MFT entry header". Used from S2 on.
#define FILE_SIGNATURE_OFFSET      0    // "FILE" (0x46494C45 as bytes)
#define FILE_USA_OFFSET            4    // uint16: offset to update sequence array
#define FILE_USA_COUNT             6    // uint16: number of 16-bit words in the USA (placeholder + fixups)
#define FILE_SEQUENCE_NUMBER       16   // uint16
#define FILE_HARD_LINK_COUNT       18   // uint16
#define FILE_FIRST_ATTR_OFFSET     20   // uint16
#define FILE_FLAGS                 22   // uint16: 0x01 in-use, 0x02 directory
#define FILE_USED_SIZE             24   // uint32
#define FILE_LSN_OFFSET            8    // uint64: $LogFile sequence number (LSN) of the MFT entry (F034)
#define FILE_NEXT_ATTR_ID_OFFSET   40   // uint16: first available (next) attribute identifier (F042, v3.0)
#define FILE_HDR_UNKNOWN_42_OFFSET 42   // uint16: unknown (wfixupPattern) (F043 v3.0 / F045 v3.1)
#define FILE_HDR_UNKNOWN_44_OFFSET 44   // uint32: unknown (v3.0) / MFT entry index (v3.1) (F044)
#define FILE_HDR_V3_MIN            48   // through the @44 field (a v3.0+ MFT entry header)
#define FILE_ALLOCATED_SIZE        28   // uint32
#define FILE_BASE_REFERENCE        32   // uint64 (file reference)
#define FILE_FLAG_IN_USE           0x0001
#define FILE_FLAG_DIRECTORY        0x0002

// System file MFT record numbers (fixed by the spec).
#define MFT_RECORD_MFT             0
#define MFT_RECORD_MFTMIRR         1
#define MFT_RECORD_LOGFILE         2
#define MFT_RECORD_VOLUME          3
#define MFT_RECORD_ROOT            5
#define MFT_RECORD_BITMAP          6
#define MFT_RECORD_BOOT            7
#define MFT_RECORD_SECURE          9    // $Secure: central security descriptor store ($SDS/$SII/$SDH)
#define MFT_RECORD_ATTRDEF         4    // $AttrDef: attribute definitions (C005)
#define MFT_RECORD_BADCLUS         8    // $BadClus: bad clusters (C009)
#define MFT_RECORD_UPCASE          10
#define MFT_RECORD_EXTEND          11   // $Extend: directory of optional metadata files ($ObjId/$Quota/$Reparse/$UsnJrnl)

// Attribute record layout. Spec: "MFT attribute header". Common header is 16 bytes; resident and
// non-resident variants add their own fields after it. Used from S3 on.
#define ATTR_TYPE_OFFSET           0    // uint32: attribute type id (below)
#define ATTR_LENGTH_OFFSET         4    // uint32: length of this whole attribute (header + value)
#define ATTR_NON_RESIDENT          8    // uint8: 0 = resident, 1 = non-resident
#define ATTR_NAME_LENGTH           9    // uint8: attribute-name length in UTF-16 units (0 = unnamed)
#define ATTR_NAME_OFFSET           10   // uint16: offset to the name within the attribute
#define ATTR_FLAGS_OFFSET          12   // uint16: 0x0001 compressed, 0x4000 encrypted, 0x8000 sparse
#define ATTR_ID_OFFSET             14   // uint16: attribute id (unique within the record)
#define ATTR_RES_VALUE_LENGTH      16   // uint32 (resident): value length in bytes
#define ATTR_RES_VALUE_OFFSET      20   // uint16 (resident): offset to the value within the attribute
#define ATTR_NR_START_VCN          16   // uint64 (non-resident): first VCN this attribute maps
#define ATTR_NR_LAST_VCN           24   // uint64 (non-resident): last VCN this attribute maps
#define ATTR_NR_RUNLIST_OFFSET     32   // uint16 (non-resident): offset to the runlist (mapping pairs)
#define ATTR_NR_COMPRESSION_UNIT   34   // uint16 (non-resident): $LZNT1 compression-unit size (power of 2)
#define ATTR_NR_ALLOC_SIZE         40   // uint64 (non-resident): allocated size in bytes
#define ATTR_NR_REAL_SIZE          48   // uint64 (non-resident): real (data) size in bytes
#define ATTR_NR_VALID_SIZE         56   // uint64 (non-resident): valid/initialized size in bytes
#define ATTR_NR_HEADER_MIN         64   // smallest non-resident header (no compression-unit field)
#define ATTR_FLAG_COMPRESSED       0x0001
#define ATTR_FLAG_ENCRYPTED        0x4000
#define ATTR_FLAG_SPARSE           0x8000

// $FILE_NAME attribute (also the key inside a directory index entry). Spec: "The file name attribute".
#define FN_PARENT_REF       0     // uint64: parent directory file reference
#define FN_MODIFIED_TIME    16    // uint64 FILETIME: last data modification (used for mtime)
#define FN_ALLOC_SIZE       40    // uint64: allocated size (cluster-rounded; 0 for directories)
#define FN_REAL_SIZE        48    // uint64: real data size (accurate when read from a $I30 entry)
#define FN_FLAGS            56    // uint32: file attribute flags (0x10000000 = directory)
#define FN_NAME_LENGTH      64    // uint8: name length in UTF-16 units
#define FN_NAMESPACE        65    // uint8: 0 POSIX, 1 Win32, 2 DOS, 3 Win32+DOS
#define FN_NAME             66    // UTF-16LE name (FN_NAME_LENGTH units)
#define FN_MIN_SIZE         66    // bytes before the name
#define FN_FLAG_DIRECTORY   0x10000000u
#define FN_FLAG_REPARSE     0x00000400u   // FILE_ATTRIBUTE_REPARSE_POINT (W12b)
// DOS/Win32 FILE_ATTRIBUTE_* bits carried in the $FILE_NAME / $STANDARD_INFORMATION flags
// field (asciidoc L2624-L2650). Surfaced per-file via NtfsInfo.attributes / VfsStat.attributes.
#define FILE_ATTRIBUTE_READONLY            0x00000001u
#define FILE_ATTRIBUTE_HIDDEN              0x00000002u
#define FILE_ATTRIBUTE_SYSTEM              0x00000004u
#define FILE_ATTRIBUTE_ARCHIVE             0x00000020u
#define FILE_ATTRIBUTE_TEMPORARY           0x00000100u
#define FILE_ATTRIBUTE_SPARSE_FILE         0x00000200u
#define FILE_ATTRIBUTE_COMPRESSED          0x00000800u
#define FILE_ATTRIBUTE_OFFLINE             0x00001000u
#define FILE_ATTRIBUTE_NOT_CONTENT_INDEXED 0x00002000u
#define FILE_ATTRIBUTE_ENCRYPTED           0x00004000u
// File-attribute flags not used by NTFS proper (FAT/legacy/view bits, asciidoc L2627-L2647).
#define FILE_ATTRIBUTE_VOLUME_LABEL        0x00000008u   // E161
#define FILE_ATTRIBUTE_DIRECTORY_FAT       0x00000010u   // E162 (FAT directory bit; NTFS uses the MFT flag)
#define FILE_ATTRIBUTE_DEVICE              0x00000040u   // E164
#define FILE_ATTRIBUTE_NORMAL              0x00000080u   // E165
#define FILE_ATTRIBUTE_UNKNOWN_8000        0x00008000u   // E173 (seen on Windows 95 FAT)
#define FILE_ATTRIBUTE_VIRTUAL             0x00010000u   // E174
#define FILE_ATTRIBUTE_DIRECTORY_I30       0x10000000u   // E175 (has $I30 index)
#define FILE_ATTRIBUTE_INDEX_VIEW          0x20000000u   // E176 (view index)
// Volume flags (asciidoc L1158-L1169), beyond the dirty bit defined below.
#define VOLUME_FLAG_RESIZE_LOGFILE         0x0002u  // E040
#define VOLUME_FLAG_UPGRADE_ON_MOUNT       0x0004u  // E041
#define VOLUME_FLAG_MOUNTED_NT4            0x0008u  // E042
#define VOLUME_FLAG_DELETE_USN_UNDERWAY    0x0010u  // E043
#define VOLUME_FLAG_REPAIR_OBJECT_IDS      0x0020u  // E044
#define VOLUME_FLAG_UNKNOWN_0080           0x0080u  // E045
#define VOLUME_FLAG_CHKDSK_UNDERWAY        0x4000u  // E046
#define VOLUME_FLAG_MODIFIED_BY_CHKDSK     0x8000u  // E047
// Collation types (asciidoc L1458-L1467).
#define COLLATION_BINARY                   0x00000000u  // E057 (first byte most significant)
#define COLLATION_UNICODE_CASE_SENSITIVE   0x00000002u  // E059
#define COLLATION_ULONG                    0x00000010u  // E060 (u32 LE)
#define COLLATION_SID                      0x00000011u  // E061
#define COLLATION_SECURITY_HASH            0x00000012u  // E062 (hash, then SID)
#define COLLATION_ULONGS                   0x00000013u  // E063 (array of u32 LE)
// MFT entry flags (beyond in-use 0x01 / directory 0x02).
#define MFT_FLAG_UNKNOWN_0004              0x0004u  // E008
#define MFT_FLAG_IS_INDEX                  0x0008u  // E009 (entry is a (view) index)
// Symbolic-link reparse flags (asciidoc L... ).
#define SYMLINK_FLAG_RELATIVE              0x00000001u  // E128 (substitute name is relative)
// Media descriptor byte bit-fields (asciidoc L422-L434): high nibble is always 0xF.
#define MEDIA_DESC_SIDES_BIT               0x01u  // E001 bit0: 0 single-sided, 1 double-sided
#define MEDIA_DESC_TRACK_SIZE_BIT          0x02u  // E002 bit1
#define MEDIA_DESC_DENSITY_BIT             0x04u  // E003 bit2
#define MEDIA_DESC_TYPE_BIT                0x08u  // E004 bit3: 0 fixed, 1 removable
#define MEDIA_DESC_HIGH_NIBBLE             0xF0u  // E005 bits4-7 always set to 1
#define FN_NAMESPACE_DOS    2     // DOS short-name twin: skipped so a long name isn't duplicated

// $INDEX_ROOT value: a 16-byte index-root header, then an index node header, then index entries.
#define IDXROOT_NODE_HEADER 16    // the index node header starts 16 bytes into the $INDEX_ROOT value
// Index node header (relative to the node header start):
#define IDXNODE_ENTRIES_OFFSET 0  // uint32: offset to the first entry (from the node header start)
#define IDXNODE_USED_SIZE      4  // uint32: bytes used (from the node header start)
#define IDXNODE_ALLOC_SIZE     8  // uint32: bytes allocated for the node (from the node header start)
#define IDXNODE_FLAGS_OFFSET   12 // uint32: node flags (bit 0 = has child nodes / large index)
// $INDEX_ALLOCATION block ("INDX" record): 24-byte header (sig + USA + LSN + VCN), then a node header.
#define INDX_NODE_HEADER       24
// Index entry:
#define IDXENTRY_FILE_REF      0  // uint64: file reference of the indexed file (0 in the end marker)
#define IDXENTRY_LENGTH        8  // uint16: length of this entry
#define IDXENTRY_KEY_LENGTH    10 // uint16: length of the key ($FILE_NAME)
#define IDXENTRY_FLAGS         12 // uint16: 0x01 has subnode, 0x02 last entry (no key)
#define IDXENTRY_FLAG_NODE     0x01 // entry has a child sub-node: last 8 bytes of the entry are its VCN
#define IDXENTRY_FLAG_LAST     0x02
#define IDXENTRY_KEY           16 // the $FILE_NAME key begins here
#define INDX_VCN_OFFSET        16 // uint64: VCN of this INDX block within $INDEX_ALLOCATION
#define NTFS_MAX_INDEX_DEPTH   24 // B-tree descent/split depth guard (a sane index is far shallower)
#define NTFS_MAX_INDEX_ENTRY   600 // ALIGN8(16 hdr + 8 child + 66 + 255*2 name): worst-case entry size
// VIEW index entry (non-$FILE_NAME, e.g. $SII/$SDH/$O): @0-7 is data offset/length/reserved instead
// of a file reference; the value (data) lives at entry+IDXENTRY_DATA_OFFSET. Common @8-13 as above.
#define IDXENTRY_DATA_OFFSET   0  // uint16: offset of the value within the entry
#define IDXENTRY_DATA_LENGTH   2  // uint16: length of the value

// $Secure security stores. $SII index value (asciidoc L2080-L2086): id@0, hash@4, id@8, $SDS offset@12, $SDS size@20.
#define SII_VAL_ID_OFFSET       0
#define SII_VAL_HASH_OFFSET     4
#define SII_VAL_ID2_OFFSET      8
#define SII_VAL_SDS_OFFSET      12   // uint64: offset into $SDS
#define SII_VAL_SDS_SIZE        20   // uint32: size in $SDS
#define SII_VAL_MIN             24
// $SDH index ($Secure:$SDH) — keyed by {hash(4), security_id(4)} (collation NTOFS_SECURITY_HASH).
#define SDH_KEY_HASH_OFFSET     0    // within the index-entry key
#define SDH_KEY_ID_OFFSET       4
#define SDH_KEY_LEN             8
// $SDH index value (asciidoc L2061-L2068): hash@0, id@4, hash@8, id@12, $SDS offset@16, size@24, padding@28.
#define SDH_VAL_HASH_OFFSET     0
#define SDH_VAL_ID_OFFSET       4
#define SDH_VAL_HASH2_OFFSET    8
#define SDH_VAL_ID2_OFFSET      12
#define SDH_VAL_SDS_OFFSET      16
#define SDH_VAL_SDS_SIZE        24
#define SDH_VAL_PAD_OFFSET      28
#define SDH_VAL_MIN             28
// $SDS data-stream entry header (L2094-L2098): hash@0, id@4, $SDS offset@12, size@20, descriptor@24.
#define SDS_HDR_HASH_OFFSET     0
#define SDS_HDR_ID_OFFSET       4
#define SDS_HDR_SDS_OFFSET      12   // uint64: this entry's own offset within $SDS
#define SDS_HDR_SIZE_OFFSET     20   // uint32: total entry size (header + descriptor)
#define SDS_HDR_DESC_OFFSET     24   // the SECURITY_DESCRIPTOR begins here
#define SDS_HDR_MIN             24

#define NTFS_FIRST_USER_RECORD 16 // records 0..15 are reserved system files ($MFT, $Volume, ...): hidden from listings

// Attribute type ids (used from S3 on).
#define ATTR_STANDARD_INFORMATION  0x10
#define ATTR_ATTRIBUTE_LIST        0x20
// $ATTRIBUTE_LIST entry layout (W8): the value is a packed list of these, sorted by (type,name,VCN).
#define AL_TYPE                    0    // uint32: attribute type of the referenced instance
#define AL_LENGTH                  4    // uint16: length of this list entry (8-aligned)
#define AL_NAME_LENGTH             6    // uint8: name length in UTF-16 units (0 = unnamed)
#define AL_NAME_OFFSET             7    // uint8: offset to the name within the entry (usually 0x1A)
#define AL_START_VCN               8    // uint64: first VCN this instance maps (0 = resident/first frag)
#define AL_MFT_REF                 0x10 // uint64: file reference of the record housing the instance
#define AL_ATTR_ID                 0x18 // uint16: the instance's attribute id within its housing record
#define AL_NAME                    0x1A // UTF-16LE name begins here
#define AL_MIN_ENTRY               0x1A // bytes before the name
#define NTFS_MAX_EXTENTS           16   // fragments of one attribute we gather across records; refuse beyond
#define NTFS_MAX_FILE_RUNS         64   // runlist fragments we decode/re-encode for a record; refuse beyond
// W10a: a large $LZNT1 file's runlist has ~2 runs per compression unit (real clusters + a sparse hole),
// so a multi-MB file blows NTFS_MAX_FILE_RUNS. Compressed reads therefore map the runlist one compression
// unit at a time (mapVcnWindow) instead of decoding it whole — O(window) memory, any file size. A unit of
// cbClusters maps to at most cbClusters runs (each cluster its own run) plus clipping at each end.
#define NTFS_MAX_CB_CLUSTERS       64   // largest compression unit we map per read (real NTFS uses 16)
#define NTFS_CB_MAX_RUNS           (NTFS_MAX_CB_CLUSTERS + 2)
#define ATTR_FILE_NAME             0x30
#define ATTR_OBJECT_ID             0x40
#define ATTR_VOLUME_NAME           0x60
#define ATTR_VOLUME_INFORMATION    0x70
// $VOLUME_INFORMATION value: reserved(8) + major(1) + minor(1) + flags(2). Dirty bit forces chkdsk.
#define VOLINFO_MAJOR_OFFSET       8    // uint8: NTFS major version (1=NT, 2, 3=Win2000+)
#define VOLINFO_MINOR_OFFSET       9    // uint8: NTFS minor version (e.g. 3.1 -> major 3 minor 1)
#define VOLINFO_FLAGS_OFFSET       10
#define VOLUME_FLAG_DIRTY          0x0001
#define ATTR_DATA                  0x80
#define ATTR_INDEX_ROOT            0x90
#define ATTR_INDEX_ALLOCATION      0xA0
#define ATTR_BITMAP                0xB0
#define ATTR_REPARSE_POINT         0xC0
#define ATTR_EA_INFORMATION        0xD0   // (HPFS) extended attribute information (asciidoc L846)
#define ATTR_EA                    0xE0   // (HPFS) extended attribute (asciidoc L847)
#define ATTR_END                   0xFFFFFFFFu
// Attribute-type catalog stragglers (asciidoc L827-L853). Some are pre-v3.0 legacy types whose code was
// reused in v3.0 (0x40 was Volume version, now $OBJECT_ID; 0xC0 was Symbolic link, now $REPARSE_POINT).
#define ATTR_UNUSED                0x00       // E014: unused
#define ATTR_VOLUME_VERSION        0x40       // E018: volume version (removed in v3.0; code reused by $OBJECT_ID)
#define ATTR_SECURITY_DESCRIPTOR   0x50       // E020: (old per-file) security descriptor
#define ATTR_SYMBOLIC_LINK         0xC0       // E027: symbolic link (removed in v3.0; code reused by $REPARSE_POINT)
#define ATTR_PROPERTY_SET          0xF0       // E031: property set (removed in v3.0)
#define ATTR_LOGGED_UTILITY_STREAM 0x100      // E032: logged utility stream (introduced in v3.0; $TXF_DATA/$EFS)
#define ATTR_FIRST_USER_DEFINED    0x1000     // E033: first user-defined attribute type
#define ATTRIBUTE_FLAG_COMPRESSION_MASK 0x00FFu  // E011: compression-method mask in the attribute data flags
// $AttrDef record (asciidoc L1340-L1349): one 160-byte attribute-definition entry.
#define ATTRDEF_NAME_OFFSET        0    // UTF-16LE name (128 bytes, NUL-padded)
#define ATTRDEF_NAME_SIZE          128
#define ATTRDEF_TYPE_OFFSET        128  // uint32: attribute type code
#define ATTRDEF_UNKNOWN_OFFSET     132  // 8 bytes
#define ATTRDEF_FLAGS_OFFSET       140  // uint32: flags (seen 0x40/0x42/0x80)
#define ATTRDEF_MIN_SIZE_OFFSET    144  // uint64: minimum attribute size
#define ATTRDEF_MAX_SIZE_OFFSET    152  // uint64: maximum attribute size (-1 = no maximum)
#define ATTRDEF_ENTRY_SIZE         160
// One-off located fields read via ntfsReadField (offset within the relevant structure's value/record).
#define ATTR_RES_INDEXED_FLAG_OFFSET   22   // F060: resident attr indexed flag (abs; @6 relative)
#define ATTR_NR_TOTAL_ALLOC_OFFSET     64   // F070: non-resident total allocated size (compressed; abs, @48 rel)
#define FN_EXTENDED_DATA_OFFSET        60   // F103: $FILE_NAME extended data
#define VOLINFO_RESERVED_OFFSET        0    // F112: $VOLUME_INFORMATION reserved (unknown/empty)
#define UNITATTR_MODE_OFFSET           0    // F126: UNITATTR value (st_mode?)
#define IDXROOT_CLUSTER_BLOCKS_OFFSET  12   // F136: index-root header "number of cluster blocks"
#define INDX_LSN_OFFSET                8    // F140: INDX block header $LogFile LSN
#define RM_REPAIR_UNKNOWN0_OFFSET      0    // F240: resource-manager repair config unknown
#define RM_REPAIR_UNKNOWN4_OFFSET      4    // F241: resource-manager repair config unknown
// $STANDARD_INFORMATION v3.0+ extended fields (asciidoc L879-L897). The first 48 bytes (timestamps@0..31,
// flags@32, and the @36/@40/@44 fields) exist in v1.2; the @48.. fields were added in v3.0.
#define SI_MAX_VERSIONS_OFFSET     36   // uint32 (F081, meaning unknown)
#define SI_VERSION_OFFSET          40   // uint32 (F082, meaning unknown)
#define SI_CLASS_ID_OFFSET         44   // uint32 (F083, meaning unknown)
#define SI_OWNER_ID_OFFSET         48   // uint32 (F084, v3.0)
#define SI_SECURITY_ID_OFFSET      52   // uint32 ($Secure:$SII entry)
#define SI_QUOTA_CHARGED_OFFSET    56   // uint64 (F086)
#define SI_USN_OFFSET              64   // uint64 (F087)
#define SI_V3_MIN                  72   // through the USN field (a v3.0+ $STANDARD_INFORMATION)

// $REPARSE_POINT structure (asciidoc L1747-L1750): tag(4)@0, data size(2)@4, reserved(2)@6, data@8.
#define REPARSE_TAG_OFFSET         0
#define REPARSE_DATA_SIZE_OFFSET   4
#define REPARSE_RESERVED_OFFSET    6
#define REPARSE_DATA_OFFSET        8
// Reparse tag bit-fields (L1768-L1770) + flag bits (L1899-L1905): low 16 bits = type, high 4 bits = flags.
#define REPARSE_TAG_TYPE_MASK          0x0000FFFFu
#define REPARSE_TAG_FLAG_RESERVED      0x10000000u   // bit 28
#define REPARSE_TAG_FLAG_NAME_SURROGATE 0x20000000u  // bit 29 "is alias"
#define REPARSE_TAG_FLAG_HIGH_LATENCY  0x40000000u   // bit 30
#define REPARSE_TAG_FLAG_MICROSOFT     0x80000000u   // bit 31 Microsoft-defined tag
// Predefined Microsoft reparse tags we resolve a target for.
#define IO_REPARSE_TAG_MOUNT_POINT 0xA0000003u   // junction / volume mount point
#define IO_REPARSE_TAG_SYMLINK     0xA000000Cu
// Predefined reparse point tag catalog (asciidoc L1780-L1871). We do not resolve a target for these (only
// MOUNT_POINT/SYMLINK above are decoded); the catalog lets a consumer classify any tag it encounters.
#define IO_REPARSE_TAG_RESERVED_ZERO     0x00000000u
#define IO_REPARSE_TAG_RESERVED_ONE      0x00000001u
#define IO_REPARSE_TAG_RESERVED_TWO      0x00000002u
#define IO_REPARSE_TAG_DRIVE_EXTENDER    0x80000005u
#define IO_REPARSE_TAG_HSM2              0x80000006u
#define IO_REPARSE_TAG_SIS               0x80000007u
#define IO_REPARSE_TAG_WIM               0x80000008u
#define IO_REPARSE_TAG_CSV               0x80000009u
#define IO_REPARSE_TAG_DFS               0x8000000Au
#define IO_REPARSE_TAG_FILTER_MANAGER    0x8000000Bu
#define IO_REPARSE_TAG_DFSR              0x80000012u
#define IO_REPARSE_TAG_DEDUP             0x80000013u
#define IO_REPARSE_TAG_NFS               0x80000014u
#define IO_REPARSE_TAG_FILE_PLACEHOLDER  0x80000015u
#define IO_REPARSE_TAG_DFM               0x80000016u
#define IO_REPARSE_TAG_WOF               0x80000017u
#define IO_REPARSE_TAG_WCI               0x80000018u
#define IO_REPARSE_TAG_APPEXECLINK       0x8000001Bu
#define IO_REPARSE_TAG_STORAGE_SYNC      0x8000001Eu
#define IO_REPARSE_TAG_UNHANDLED         0x80000020u
#define IO_REPARSE_TAG_ONEDRIVE          0x80000021u
#define IO_REPARSE_TAG_AF_UNIX           0x80000023u
#define IO_REPARSE_TAG_LX_FIFO           0x80000024u
#define IO_REPARSE_TAG_LX_CHR            0x80000025u
#define IO_REPARSE_TAG_LX_BLK            0x80000036u
#define IO_REPARSE_TAG_PROJFS            0x9000001Cu
#define IO_REPARSE_TAG_WCI_1             0x90001018u
#define IO_REPARSE_TAG_CLOUD_1           0x9000101Au
#define IO_REPARSE_TAG_CLOUD_2           0x9000201Au
#define IO_REPARSE_TAG_CLOUD_3           0x9000301Au
#define IO_REPARSE_TAG_CLOUD_4           0x9000401Au
#define IO_REPARSE_TAG_CLOUD_5           0x9000501Au
#define IO_REPARSE_TAG_CLOUD_6           0x9000601Au
#define IO_REPARSE_TAG_CLOUD_7           0x9000701Au
#define IO_REPARSE_TAG_CLOUD_8           0x9000801Au
#define IO_REPARSE_TAG_CLOUD_9           0x9000901Au
#define IO_REPARSE_TAG_CLOUD_A           0x9000A01Au
#define IO_REPARSE_TAG_CLOUD_B           0x9000B01Au
#define IO_REPARSE_TAG_CLOUD_C           0x9000C01Au
#define IO_REPARSE_TAG_CLOUD_D           0x9000D01Au
#define IO_REPARSE_TAG_CLOUD_E           0x9000E01Au
#define IO_REPARSE_TAG_CLOUD_F           0x9000F01Au
#define IO_REPARSE_TAG_IIS_CACHE         0xA0000010u
#define IO_REPARSE_TAG_GLOBAL_REPARSE    0xA0000019u
#define IO_REPARSE_TAG_CLOUD             0xA000001Au
#define IO_REPARSE_TAG_LX_SYMLINK        0xA000001Du
#define IO_REPARSE_TAG_WCI_TOMBSTONE     0xA000001Fu
#define IO_REPARSE_TAG_PROJFS_TOMBSTONE  0xA0000022u
#define IO_REPARSE_TAG_WCI_LINK          0xA0000027u
#define IO_REPARSE_TAG_WCI_LINK_1        0xA0001027u
#define IO_REPARSE_TAG_HSM               0xC0000004u
#define IO_REPARSE_TAG_APPXSTRM          0xC0000014u
// symlink/junction reparse data header (relative to REPARSE_DATA_OFFSET):
#define RP_SUBST_NAME_OFFSET   0   // uint16: substitute-name offset, relative to the name buffer
#define RP_SUBST_NAME_SIZE     2   // uint16: substitute-name size in bytes (no end-of-string)
#define RP_PRINT_NAME_OFFSET   4   // uint16: print-name offset
#define RP_PRINT_NAME_SIZE     6   // uint16: print-name size in bytes
#define RP_JUNCTION_NAME_BASE  8   // junction: name buffer begins 8 bytes into the reparse data
#define RP_SYMLINK_FLAGS_OFFSET 8  // symlink: 4-byte flags field
#define RP_SYMLINK_NAME_BASE  12   // symlink: name buffer begins after the flags
#define REPARSE_MAX_READ      4096 // bounded read for a non-resident $REPARSE_POINT (covers any path target)
// $OBJECT_ID attribute (asciidoc L1098-L1104): four 16-byte GUIDs (CDomainRelativeObjId).
#define OBJID_DROID_FILE_OFFSET    0    // the object's own object_id GUID
#define OBJID_BIRTH_VOLUME_OFFSET  16
#define OBJID_BIRTH_FILE_OFFSET    32
#define OBJID_BIRTH_DOMAIN_OFFSET  48
#define GUID_SIZE                  16
#define OBJID_MAX                  64   // all four GUIDs (the attribute may carry just the first 16)
// $EA_INFORMATION attribute (asciidoc L1250-L1253): 8-byte header.
#define EAINFO_ENTRY_SIZE_OFFSET   0    // uint16: size of (the packed) extended attribute entry
#define EAINFO_NEED_EA_COUNT_OFFSET 2   // uint16: number of EAs that have NEED_EA set
#define EAINFO_DATA_SIZE_OFFSET    4    // uint32: size of the $EA data
#define EAINFO_SIZE                8
// $EA attribute (asciidoc L1271-L1281): a chain of variable-length entries.
#define EA_NEXT_OFFSET_OFFSET      0    // uint32: offset (from start of $EA data) to the next entry; 0 = last
#define EA_FLAGS_OFFSET            4    // uint8: flags
#define EA_NAME_LENGTH_OFFSET      5    // uint8: EA name length in ASCII chars (no NUL)
#define EA_VALUE_SIZE_OFFSET       6    // uint16: value-data size
#define EA_NAME_OFFSET             8    // ASCII name begins here; value follows the NUL after the name
#define EA_FLAG_NEED_EA            0x80 // NEED_EA: the EA must be understood for correct operation
#define EA_HEADER_MIN              8    // fixed header bytes before the name
#define EA_MAX_READ                8192 // bounded prefix scanned for a non-resident $EA chain
// USN change-journal metadata ($UsnJrnl:$Max, asciidoc L2267-L2273): four uint64 fields.
#define USNMAX_MAX_SIZE_OFFSET     0    // maximum journal size in bytes
#define USNMAX_ALLOC_DELTA_OFFSET  8    // allocation delta in bytes
#define USNMAX_JOURNAL_ID_OFFSET   16   // journal identifier (a FILETIME)
#define USNMAX_UNKNOWN_OFFSET      24   // unknown (empty)
#define USN_MAX_MIN                32
// USN change-journal entry / USN_RECORD_V2 ($UsnJrnl:$J, asciidoc L2297-L2320).
#define USN_REC_LENGTH_OFFSET      0    // uint32: entry (record) size
#define USN_REC_MAJOR_OFFSET       4    // uint16: major version (2)
#define USN_REC_MINOR_OFFSET       6    // uint16: minor version (0)
#define USN_REC_FILE_REF_OFFSET    8    // uint64: file reference
#define USN_REC_PARENT_REF_OFFSET  16   // uint64: parent file reference
#define USN_REC_USN_OFFSET         24   // uint64: USN (file offset of this entry)
#define USN_REC_TIMESTAMP_OFFSET   32   // uint64: update date/time (FILETIME)
#define USN_REC_REASON_OFFSET      40   // uint32: update reason flags
#define USN_REC_SOURCE_OFFSET      44   // uint32: update source flags
#define USN_REC_SECURITY_ID_OFFSET 48   // uint32: security-descriptor id ($Secure:$SII entry)
#define USN_REC_FILE_ATTRS_OFFSET  52   // uint32: file attribute flags
#define USN_REC_NAME_SIZE_OFFSET   56   // uint16: name byte size
#define USN_REC_NAME_OFFSET_OFFSET 58   // uint16: name offset (from start of the entry)
#define USN_REC_NAME_BASE          60   // the name buffer begins here in a V2 record
#define USN_REC_MIN                60   // smallest V2 record (no name)
// Update reason flags (asciidoc L2328-L2359).
#define USN_REASON_DATA_OVERWRITE        0x00000001u
#define USN_REASON_DATA_EXTEND           0x00000002u
#define USN_REASON_DATA_TRUNCATION       0x00000004u
#define USN_REASON_NAMED_DATA_OVERWRITE  0x00000010u
#define USN_REASON_NAMED_DATA_EXTEND     0x00000020u
#define USN_REASON_NAMED_DATA_TRUNCATION 0x00000040u
#define USN_REASON_FILE_CREATE           0x00000100u
#define USN_REASON_FILE_DELETE           0x00000200u
#define USN_REASON_EA_CHANGE             0x00000400u
#define USN_REASON_SECURITY_CHANGE       0x00000800u
#define USN_REASON_RENAME_OLD_NAME       0x00001000u
#define USN_REASON_RENAME_NEW_NAME       0x00002000u
#define USN_REASON_INDEXABLE_CHANGE      0x00004000u
#define USN_REASON_BASIC_INFO_CHANGE     0x00008000u
#define USN_REASON_HARD_LINK_CHANGE      0x00010000u
#define USN_REASON_COMPRESSION_CHANGE    0x00020000u
#define USN_REASON_ENCRYPTION_CHANGE     0x00040000u
#define USN_REASON_OBJECT_ID_CHANGE      0x00080000u
#define USN_REASON_REPARSE_POINT_CHANGE  0x00100000u
#define USN_REASON_STREAM_CHANGE         0x00200000u
#define USN_REASON_TRANSACTED_CHANGE     0x00400000u
#define USN_REASON_CLOSE                 0x80000000u
// Update source flags (asciidoc L2367-L2369).
#define USN_SOURCE_DATA_MANAGEMENT       0x00000001u
#define USN_SOURCE_AUXILIARY_DATA        0x00000002u
#define USN_SOURCE_REPLICATION_MANAGEMENT 0x00000004u
// $LogFile restart page header / RESTART_PAGE_HEADER (asciidoc L2160-L2178). Begins with a
// MULTI_SECTOR_HEADER (signature + USA), like FILE/INDX records.
#define LOG_RSTR_SIG_OFFSET            0    // 4-byte signature: "RSTR" / "RCRD" / "CHKD"
#define LOG_RSTR_USA_OFFSET_OFFSET     4    // uint16: update-sequence-array offset
#define LOG_RSTR_USA_COUNT_OFFSET      6    // uint16: number of fix-up values
#define LOG_RSTR_CHKDSK_LSN_OFFSET     8    // uint64: checkdisk last LSN
#define LOG_RSTR_SYS_PAGE_SIZE_OFFSET  16   // uint32: system page size
#define LOG_RSTR_LOG_PAGE_SIZE_OFFSET  20   // uint32: log page size
#define LOG_RSTR_RESTART_OFFSET_OFFSET 24   // uint16: restart offset
#define LOG_RSTR_MINOR_VER_OFFSET      26   // uint16: minor format version
#define LOG_RSTR_MAJOR_VER_OFFSET      28   // uint16: major format version (-1 beta / 0 transition / 1 USA support)
#define LOG_RSTR_MIN                   30   // through the major-version field
// $LogFile record header / LFS_RECORD_HEADER LSN triplet (asciidoc L2191-L2195).
#define LFS_REC_THIS_LSN_OFFSET        0    // uint64: this record's LSN
#define LFS_REC_PREV_LSN_OFFSET        8    // uint64: previous LSN
#define LFS_REC_UNDO_NEXT_LSN_OFFSET   16   // uint64: undo-next LSN
#define LFS_REC_MIN                    24
// $ObjID:$O index (asciidoc L2113-L2122): key = object_id GUID@0(16); value = file ref@4(8),
// birth volume id@12(16), birth file id@28(16), birth domain id@44(16).
#define OBJO_GUID_SIZE                 16
#define OBJO_VAL_FILE_REF_OFFSET       4
#define OBJO_VAL_BIRTH_VOLUME_OFFSET   12
#define OBJO_VAL_BIRTH_FILE_OFFSET     28
#define OBJO_VAL_BIRTH_DOMAIN_OFFSET   44
#define OBJO_VAL_MIN                   60   // through the birth-domain GUID
// TxF Old Page Stream (TOPS) metadata ($Tops unnamed $DATA, asciidoc L2479-L2492): 100 bytes.
#define TOPS_UNKNOWN0_OFFSET           0    // uint16, observed 0x000a
#define TOPS_SIZE_OFFSET               2    // uint16: size of TOPS metadata (0x0064 = 100)
#define TOPS_RM_COUNT_OFFSET           4    // uint32, observed 0x0001 (resource managers/streams?)
#define TOPS_RM_GUID_OFFSET            8    // resource-manager identifier (GUID, 16)
#define TOPS_UNKNOWN24_OFFSET          24   // uint64 (empty)
#define TOPS_BASE_LSN_OFFSET           32   // uint64: base/log-start LSN of the TxFLog stream
#define TOPS_UNKNOWN40_OFFSET          40   // uint64
#define TOPS_LAST_FLUSHED_LSN_OFFSET   48   // uint64: last flushed LSN of the TxFLog stream
#define TOPS_UNKNOWN56_OFFSET          56   // uint64
#define TOPS_UNKNOWN64_OFFSET          64   // uint64 (empty)
#define TOPS_RESTART_LSN_OFFSET        72   // uint64: restart LSN?
#define TOPS_UNKNOWN80_OFFSET          80   // 20 bytes
#define TOPS_META_SIZE                 100  // 0x0064
// $TXF_DATA logged utility stream attribute (asciidoc L2597-L2609).
#define TXF_REMNANT_OFFSET             0    // 6 bytes, remnant data
#define TXF_RM_ROOT_REF_OFFSET         6    // uint64: resource-manager root file reference (an MFT ref)
#define TXF_USN_INDEX_OFFSET           14   // uint64: USN index?
#define TXF_TXID_OFFSET                22   // uint64: TxF file identifier (TxID)
#define TXF_DATA_LSN_OFFSET            30   // uint64: data LSN (CLFS LSN of file-data tx records)
#define TXF_METADATA_LSN_OFFSET        38   // uint64: metadata LSN
#define TXF_DIR_INDEX_LSN_OFFSET       46   // uint64: directory-index LSN
#define TXF_FLAGS_OFFSET               54   // uint16: flags (seen 0x0000 / 0x0002)
#define TXF_DATA_MIN                   56   // through the flags field
// Windows Overlay Filter (WOF) reparse data (asciidoc L1985-L1989): four uint32 fields, 16 bytes.
#define WOF_VERSION_OFFSET             0    // WOF version (observed 1)
#define WOF_PROVIDER_OFFSET            4    // WOF provider (observed 2)
#define WOF_FILEINFO_VERSION_OFFSET    8    // file-information version (observed 1)
#define WOF_COMPRESSION_METHOD_OFFSET  12   // compression method (enum below)
#define WOF_REPARSE_MIN                16
// WOF compression method values (asciidoc L1998-L2001).
#define WOF_COMPRESSION_LZXPRESS_4K    0    // LZXPRESS Huffman, 4k window
#define WOF_COMPRESSION_LZX_32K        1    // LZX, 32k window
#define WOF_COMPRESSION_LZXPRESS_8K    2    // LZXPRESS Huffman, 8k window
#define WOF_COMPRESSION_LZXPRESS_16K   3    // LZXPRESS Huffman, 16k window
// WOF compressed data: an array of 32- or 64-bit chunk offsets (relative to the data-chunk region), then
// the chunks themselves (asciidoc, F155-F156). The offset array begins at the start of the compressed data.
#define WOF_CHUNK_OFFSETS_OFFSET       0
// Windows Container Isolation (WCI) reparse data (asciidoc L2-series, F181-F185).
#define WCI_VERSION_OFFSET             0    // uint32: version
#define WCI_RESERVED_OFFSET            4    // uint32: reserved
#define WCI_LOOKUP_GUID_OFFSET         8    // look-up identifier (GUID, 16)
#define WCI_NAME_SIZE_OFFSET           24   // uint16: name size in bytes
#define WCI_NAME_OFFSET                26   // UTF-16LE name (no terminator)
#define WCI_REPARSE_MIN                26   // through the name-size field (name may be empty)

// Reads `count` sectors at `lba` into a 32-byte-aligned buffer, retrying while the device reports
// "not ready" (hotplug settling). Returns 0 on success, -1 on a hard error.
static int readSectors(int storageHandle, uint64_t lba, uint32_t count, void *aligned)
{
   for (int attempt = 0; attempt < SYSIO_RETRY; attempt++) {
      uint32_t got = 0;
      int rc = readStorageRaw(storageHandle, lba, count, aligned, &got);
      if (rc == 0 && got == count) return 0;
      if ((uint32_t)rc != STORAGE_BUSY) return -1;
      sys_timer_usleep(SYSIO_RETRY_US);
   }
   return -1;
}

// True if `p` meets the lv2 storage DMA alignment, so whole sectors can be read/written in place
// without bouncing through an aligned scratch buffer (mirror of exfat.c).
static int isDmaAligned(const void *p) { return ((uintptr_t)p & (STORAGE_ALIGN - 1)) == 0; }

// ===========================================================================
// Little-endian readers (NTFS is LE on disk, the PPU is big-endian). Never read
// a multi-byte on-disk field any other way.
// ===========================================================================
static uint16_t readLe16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t readLe32(const uint8_t *p)
{
   return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t readLe64(const uint8_t *p) { return (uint64_t)readLe32(p) | ((uint64_t)readLe32(p + 4) << 32); }


// ===========================================================================
// Shared, 32-byte-aligned scratch. Only touched while the (single) caller holds
// ntfsLock, so these are safe to share across volumes/handles. Per-mount epoch
// keys any future cache the way exfat.c does (lv2 recycles storageHandle).
// ===========================================================================
static uint32_t mountEpoch;   // last epoch handed out (first mount gets 1; wraps skip 0)

static uint8_t  bootSector[NTFS_MAX_SECTOR] __attribute__((aligned(STORAGE_ALIGN)));
// Two one-sector scratch buffers for the partition scan: scanScratch reads GPT header/entry array,
// vbrScratch reads partition candidates (kept separate so the entry array isn't clobbered mid-walk).
static uint8_t  scanScratch[NTFS_MAX_SECTOR] __attribute__((aligned(STORAGE_ALIGN)));
static uint8_t  vbrScratch[NTFS_MAX_SECTOR]  __attribute__((aligned(STORAGE_ALIGN)));
// Working buffer for one FILE record (bootstrap MFT reads, attribute walks). Shared under ntfsLock.
static uint8_t  mftRecord[NTFS_MAX_RECORD]   __attribute__((aligned(STORAGE_ALIGN)));
// Sector-aligned bounce for non-resident file data reads (read whole sectors, copy the slice out).
static uint8_t  fileBounce[NTFS_READ_BOUNCE] __attribute__((aligned(STORAGE_ALIGN)));
// Dedicated sector buffer for cluster-$Bitmap I/O, so allocation never reuses mftRecord (which would
// force re-reading the file's and $Bitmap's MFT records every block). Shared under ntfsLock.
static uint8_t  bitmapScratch[NTFS_MAX_SECTOR] __attribute__((aligned(STORAGE_ALIGN)));
// One $INDEX_ALLOCATION block ("INDX" record) buffer for directory enumeration. Shared under ntfsLock.
static uint8_t  indexBuffer[NTFS_MAX_RECORD]  __attribute__((aligned(STORAGE_ALIGN)));
// W6 (index B-tree growth) scratch: the directory FILE record kept stable across cluster/bitmap ops
// (which clobber mftRecord), plus two INDX node buffers so a parent + child + new-right sibling can be
// held during a split (the third sibling reuses indexBuffer).
// W8 ($ATTRIBUTE_LIST): a second FILE-record buffer so an extension record can be read while the base
// record stays in another buffer; merged runlist scratch when one attribute spans several records.
static uint8_t  extRecord[NTFS_MAX_RECORD]   __attribute__((aligned(STORAGE_ALIGN)));
// W10a ($LZNT1): one compressed sub-block (header + data) and one decompressed sub-block. Decoding a
// sub-block at a time keeps these tiny (~8 KB) vs buffering a whole 64 KB compression unit.
static uint8_t  lzComp[4096 + 8]             __attribute__((aligned(STORAGE_ALIGN)));
static uint8_t  lzPlain[4096];
// The directory index name "$I30" (UTF-16), used to select the $INDEX_ROOT / $INDEX_ALLOCATION attrs.
static const uint16_t indexNameI30[4] = { '$', 'I', '3', '0' };

// ===========================================================================
// Boot-sector recognition and geometry validation.
// ===========================================================================

// true if buffer holds an NTFS boot sector ("NTFS    " at offset 3, sig 0xAA55). The OEM tag is
// distinct from exFAT's "EXFAT   " and FAT32's "FAT32", so this never false-positives on them.
static int hasNtfsBoot(const uint8_t *boot)
{
   static const char tag[8] = { 'N', 'T', 'F', 'S', ' ', ' ', ' ', ' ' };
   for (int i = 0; i < 8; i++) {
      if (boot[BOOT_OEM_OFFSET + i] != (uint8_t)tag[i]) return 0;
   }
   return boot[BOOT_SIGNATURE] == 0x55 && boot[BOOT_SIGNATURE + 1] == 0xAA;
}

// true if a sector begins with the GPT header signature "EFI PART".
static int hasGptHeader(const uint8_t *sector)
{
   static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
   for (int i = 0; i < 8; i++) {
      if (sector[i] != (uint8_t)sig[i]) return 0;
   }
   return 1;
}

// Decodes the spec's power-of-two factor used by SectorsPerCluster and the MFT/index record size
// fields: byte values up to `literalMax` are a literal count of the smaller unit; larger values mean
// 2^(256-value) bytes. The two fields differ only at value 128: SectorsPerCluster treats 0..128 as a
// literal sector count (literalMax 128), so a 64 KB cluster on 512-byte sectors (byte 0x80 == 128
// sectors) parses; the record-size fields treat 128..255 as the byte form (literalMax 127). Returns
// the count (caller multiplies literal values by the unit). Example: factor 0xF6 -> 2^10 == 1024;
// factor 0x04 -> 4 units; SectorsPerCluster 128 -> 128.
static uint32_t decodeSignedFactor(uint8_t factor, uint32_t unitBytes, uint8_t literalMax, uint32_t *outBytes)
{
   if (factor <= literalMax) {
      if (unitBytes == 0) { *outBytes = 0; return 0; }   // caller rejects a 0 size
      *outBytes = (uint32_t)factor * unitBytes;
      return factor;
   }
   uint32_t shift = 256u - factor;                        // 2^(256-value) bytes
   if (shift >= 32) { *outBytes = 0; return 0; }   // absurd; caller rejects a 0 size
   *outBytes = 1u << shift;
   return *outBytes / (unitBytes ? unitBytes : 1);
}

// Validates the (untrusted, removable-media) NTFS boot geometry before any of it is used to compute
// LBAs. Returns 1 if usable, 0 to reject the volume. `deviceSectors` of 0 means the device size is
// unknown, so the device-size bound is skipped. Fills the parsed geometry into *vol on success.
static int parseNtfsBoot(const uint8_t *boot, uint32_t deviceSectorSize, uint64_t deviceSectors,
                         uint64_t volStart, NtfsVolume *vol)
{
   // bytes per sector: power of two in [256, 4096], and must match the device
   uint32_t bytesPerSector = readLe16(boot + BOOT_BYTES_PER_SECTOR);
   if (bytesPerSector < 256 || bytesPerSector > NTFS_MAX_SECTOR) return 0;
   if (bytesPerSector & (bytesPerSector - 1)) return 0;            // not a power of two
   if (bytesPerSector != deviceSectorSize) return 0;

   // sectors per cluster (signed power-of-two factor; unit is 1 sector, so the decoded "count" of
   // sectors is what we want whether the field was positive or the negative 2^(-value) form)
   uint32_t clusterSectorBytes = 0;
   uint32_t sectorsPerCluster = decodeSignedFactor(boot[BOOT_SECTORS_PER_CLUSTER], 1, 128, &clusterSectorBytes);
   if (sectorsPerCluster == 0 || (sectorsPerCluster & (sectorsPerCluster - 1))) return 0;
   uint64_t bytesPerCluster = (uint64_t)bytesPerSector * sectorsPerCluster;
   if (bytesPerCluster > (32u * 1024u * 1024u)) return 0;          // cluster <= 32 MB (sanity)

   // total sectors and the MFT location
   uint64_t totalSectors = readLe64(boot + BOOT_TOTAL_SECTORS);
   uint64_t mftLcn       = readLe64(boot + BOOT_MFT_LCN);
   uint64_t mftMirrLcn   = readLe64(boot + BOOT_MFTMIRR_LCN);
   if (totalSectors == 0) return 0;
   uint64_t clusterCount = totalSectors / sectorsPerCluster;
   if (mftLcn == 0 || mftLcn >= clusterCount) return 0;            // $MFT must live inside the volume
   if (mftMirrLcn == 0 || mftMirrLcn >= clusterCount) return 0;

   // MFT record size and index record size (signed power-of-two factor, in clusters or bytes)
   uint32_t mftRecordSize = 0, indexRecordSize = 0;
   decodeSignedFactor(boot[BOOT_MFT_RECORD_FACTOR], (uint32_t)bytesPerCluster, 127, &mftRecordSize);
   decodeSignedFactor(boot[BOOT_INDEX_RECORD_FACTOR], (uint32_t)bytesPerCluster, 127, &indexRecordSize);
   if (mftRecordSize < bytesPerSector || mftRecordSize > NTFS_MAX_RECORD) return 0;
   if (mftRecordSize & (mftRecordSize - 1)) return 0;             // record size must be a power of two
   if (indexRecordSize == 0 || (indexRecordSize & (indexRecordSize - 1))) return 0;
   if (indexRecordSize < bytesPerSector || indexRecordSize > NTFS_MAX_RECORD) return 0;   // >= a sector so
   // every INDX walker's `indexRecordSize - INDX_NODE_HEADER` (and the USA/INDX header) cannot underflow;
   // must also fit indexBuffer (we read whole blocks into it).

   // the volume (heap) must fit the device when its size is known
   if (deviceSectors != 0 && volStart + totalSectors > deviceSectors) return 0;

   // commit the validated geometry
   vol->bytesPerSector    = bytesPerSector;
   vol->sectorsPerCluster = sectorsPerCluster;
   vol->bytesPerCluster   = (uint32_t)bytesPerCluster;
   vol->totalSectors      = totalSectors;
   vol->mftLcn            = mftLcn;
   vol->mftMirrLcn        = mftMirrLcn;
   vol->mftRecordSize     = mftRecordSize;
   vol->indexRecordSize   = indexRecordSize;
   vol->volumeSerial      = readLe64(boot + BOOT_VOLUME_SERIAL);
   return 1;
}

// If sector `lba` holds an NTFS boot sector, copies it into `boot`, records the start in *volStart
// and returns 1; otherwise 0. Reads through the caller's aligned `vbr` scratch. Rejects an
// out-of-range LBA (attacker-controlled partition field) before reading it.
static int tryNtfsVbr(int storageHandle, uint64_t lba, uint32_t sectorBytes, uint64_t deviceSectors,
                      uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   uint64_t sectorBound = deviceSectors ? deviceSectors : NTFS_SCAN_LBA_CAP;
   if (lba == 0 || lba >= sectorBound) return 0;
   if (readSectors(storageHandle, lba, 1, vbr) != 0 || !hasNtfsBoot(vbr)) return 0;
   memCopy(boot, vbr, (int)sectorBytes);
   *volStart = lba;
   return 1;
}

// Walks a GPT partition table and returns the first partition whose first sector is an NTFS VBR.
static int locateNtfsInGpt(int storageHandle, uint32_t sectorBytes, uint64_t deviceSectors,
                           uint8_t *scratch, uint8_t *vbr, uint8_t *boot, uint64_t *volStart)
{
   if (readSectors(storageHandle, GPT_HEADER_LBA, 1, scratch) != 0 || !hasGptHeader(scratch)) return 0;
   uint64_t entriesLba = readLe64(scratch + 72);   // PartitionEntryLBA
   uint32_t entryCount = readLe32(scratch + 80);   // NumberOfPartitionEntries
   uint32_t entrySize  = readLe32(scratch + 84);   // SizeOfPartitionEntry
   if (entrySize < GPT_ENTRY_MIN_SIZE || entrySize > sectorBytes) return 0;
   uint64_t sectorBound = deviceSectors ? deviceSectors : NTFS_SCAN_LBA_CAP;
   if (entriesLba >= sectorBound) return 0;
   if (entryCount > GPT_MAX_ENTRIES) entryCount = GPT_MAX_ENTRIES;
   uint32_t perSector = sectorBytes / entrySize;

   for (uint32_t i = 0; i < entryCount; i++) {
      if (i % perSector == 0) {
         uint64_t entrySectorLba = entriesLba + i / perSector;   // re-bound each read: a near-cap entriesLba could otherwise overflow into a wild LBA
         if (entrySectorLba >= sectorBound) return 0;
         if (readSectors(storageHandle, entrySectorLba, 1, scratch) != 0) return 0;
      }
      const uint8_t *entry = scratch + (i % perSector) * entrySize;
      int used = 0;
      for (int k = 0; k < 16; k++) if (entry[k]) { used = 1; break; }   // non-zero PartitionTypeGUID
      if (used && tryNtfsVbr(storageHandle, readLe64(entry + 32), sectorBytes, deviceSectors, vbr, boot, volStart))
         return 1;
   }
   return 0;
}

// Locates the NTFS volume reachable through `storageHandle`: a superfloppy at LBA 0, or a partition
// listed in an MBR or GPT table. On success `boot` holds the volume's VBR and *volStart its start LBA.
static int locateNtfsVolume(int storageHandle, uint8_t *boot, uint8_t *scratch, uint8_t *vbr,
                            uint32_t sectorBytes, uint64_t deviceSectors, uint64_t *volStart)
{
   if (hasNtfsBoot(boot)) { *volStart = 0; return 1; }              // superfloppy (volume at LBA 0)
   if (boot[BOOT_SIGNATURE] != 0x55 || boot[BOOT_SIGNATURE + 1] != 0xAA) return 0;   // neither NTFS nor partitioned

   for (int i = 0; i < MBR_PART_ENTRIES; i++) {
      const uint8_t *part = boot + MBR_PART_TABLE + i * MBR_PART_SIZE;
      uint8_t type = part[4];
      if (type == 0) continue;
      if (type == MBR_TYPE_GPT) {
         if (locateNtfsInGpt(storageHandle, sectorBytes, deviceSectors, scratch, vbr, boot, volStart)) return 1;
         continue;
      }
      if (tryNtfsVbr(storageHandle, readLe32(part + 8), sectorBytes, deviceSectors, vbr, boot, volStart)) return 1;
   }
   return 0;
}

// ===========================================================================
// MFT record reading + Update Sequence Array (fixup).
// ===========================================================================

// Applies the NTFS Update Sequence Array fixup to a multi-sector metadata record (FILE or, later,
// INDX). At write time each sector's last 2 bytes are replaced on disk by a single update sequence
// number (USN); the real values are stashed in the USA. We verify every sector still carries the
// USN - if one doesn't, the multi-sector write was torn (or the record is corrupt), which would
// otherwise be silent corruption - then restore the saved words. `recordSize` is a multiple of
// `sectorSize` (both validated at mount). Returns 0 on success, -1 on any mismatch/out-of-bounds.
static int applyUsaFixup(uint8_t *record, uint32_t recordSize, uint32_t sectorSize)
{
   uint16_t usaOffset = readLe16(record + FILE_USA_OFFSET);
   uint16_t usaCount  = readLe16(record + FILE_USA_COUNT);   // 1 USN word + one fixup word per sector

   // the USA must describe exactly one fixup per sector, and must physically fit in the record
   uint32_t blocks = recordSize / sectorSize;
   if (usaCount == 0 || (uint32_t)(usaCount - 1) != blocks) return -1;
   if (usaOffset < FILE_FIRST_ATTR_OFFSET) return -1;                      // USA can't sit in the fixed header fields
   if ((uint32_t)usaOffset + (uint32_t)usaCount * 2 > recordSize) return -1;

   // verify each sector tail still holds the USN, then restore the saved word
   const uint8_t *usa = record + usaOffset;
   uint16_t usn = readLe16(usa);
   for (uint32_t i = 0; i < blocks; i++) {
      uint8_t *tail = record + (i + 1) * sectorSize - 2;   // last 2 bytes of sector i
      if (readLe16(tail) != usn) return -1;                // torn write / corruption
      const uint8_t *saved = usa + 2 + i * 2;
      tail[0] = saved[0];
      tail[1] = saved[1];
   }
   return 0;
}

// Validates a FILE record header (after fixup): "FILE" signature, in-use, and the first-attribute
// offset and used size within the record. Returns 1 if usable, 0 otherwise.
static int isValidFileRecord(const uint8_t *record, uint32_t recordSize)
{
   if (record[0] != 'F' || record[1] != 'I' || record[2] != 'L' || record[3] != 'E') return 0;
   if (!(readLe16(record + FILE_FLAGS) & FILE_FLAG_IN_USE)) return 0;
   uint16_t firstAttr = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   uint32_t usedSize  = readLe32(record + FILE_USED_SIZE);
   if (firstAttr < FILE_FIRST_ATTR_OFFSET || firstAttr >= recordSize) return 0;
   if (usedSize < firstAttr || usedSize > recordSize) return 0;
   return 1;
}

// Reads FILE record `number` directly from the start of $MFT and fixes it up. Bootstrap path: the
// first MFT clusters are contiguous from mftLcn, so low-numbered system records ($MFT, $MFTMirr,
// root, $Bitmap, $UpCase) are reachable without the $MFT runlist - which is itself parsed out of
// record 0. Arbitrary (possibly fragmented) records need that runlist and arrive in a later stage.
// Returns 0 on success with the fixed-up, validated record in `out` (>= vol->mftRecordSize bytes).
static int readMftRecordBootstrap(const NtfsVolume *vol, uint64_t number, uint8_t *out)
{
   uint32_t recordSize = vol->mftRecordSize;
   uint64_t byteOffset = vol->mftLcn * vol->bytesPerCluster + number * recordSize;   // within contiguous MFT head
   uint64_t lba        = vol->partitionOffset + byteOffset / vol->bytesPerSector;    // sector-aligned (record/cluster are sector multiples)
   uint32_t sectors    = recordSize / vol->bytesPerSector;
   if (readSectors(vol->storageHandle, lba, sectors, out) != 0) return -1;
   if (applyUsaFixup(out, recordSize, vol->bytesPerSector) != 0) return -1;
   if (!isValidFileRecord(out, recordSize)) return -1;
   return 0;
}

static int64_t mapVcnToLcn(const NtfsRunEntry *runs, int runCount, uint64_t vcn);   // defined in the runlist section
static int decodeRuns(const uint8_t *runlist, uint32_t length, NtfsRunEntry *runs, int max, int *count);  // runlist section

// ===========================================================================
// Attribute parsing. Walks the attribute chain inside one FILE record, parsing
// the common header and the resident vs non-resident variants. ($ATTRIBUTE_LIST
// spanning multiple records is followed in S4, once arbitrary records are
// readable via the $MFT runlist.)
// ===========================================================================

// One attribute's parsed location within a FILE record buffer. For resident attributes `value`
// points at the data inside the record; for non-resident ones `attr` points at the attribute
// header so the runlist (at runlistOffset) can be decoded later.
typedef struct {
   uint32_t type;
   uint16_t attributeId;
   int      resident;
   int      compressed;        // ATTR_FLAG_COMPRESSED or ATTR_FLAG_SPARSE set
   uint32_t compUnitClusters;  // $LZNT1 compression-unit size in clusters (1<<compression_unit), 0 if none
   const uint8_t *value;       // resident: pointer to the value within the record
   uint32_t valueLength;       // resident: value length in bytes
   const uint8_t *attr;        // non-resident: pointer to the attribute header
   uint32_t attrLength;        // non-resident: whole-attribute length
   uint16_t runlistOffset;     // non-resident: offset of the runlist within the attribute
   uint64_t startVcn;          // non-resident: first VCN mapped
   uint64_t lastVcn;           // non-resident: last VCN mapped
   uint64_t allocatedSize;     // non-resident: allocated bytes
   uint64_t realSize;          // non-resident: real (data) bytes
   uint64_t validSize;         // non-resident: valid/initialized bytes (reads past it are zero)
} NtfsAttr;

static int findDataAnywhere(NtfsVolume *vol, uint64_t baseRef, uint8_t *buf, NtfsAttr *out, uint64_t *housingRef);
static int countNtfsFreeClusters(NtfsVolume *vol, uint64_t *out);   // seeds vol->freeClusters at mount

// Parses one attribute (header already located at `attr`, total length `attrLength`) into *out,
// bounds-checking the resident value / non-resident fields against the attribute. Returns 1 on
// success, 0 if a field doesn't fit the attribute.
static int parseAttribute(const uint8_t *attr, uint32_t attrLength, NtfsAttr *out)
{
   memSet(out, 0, (int)sizeof(*out));
   out->type        = readLe32(attr + ATTR_TYPE_OFFSET);
   out->attributeId = readLe16(attr + ATTR_ID_OFFSET);
   out->resident    = (attr[ATTR_NON_RESIDENT] == 0);
   out->attr        = attr;          // record location of the attribute header (both forms)
   out->attrLength  = attrLength;
   out->compressed  = (readLe16(attr + ATTR_FLAGS_OFFSET) & (ATTR_FLAG_COMPRESSED | ATTR_FLAG_SPARSE)) != 0;

   if (out->resident) {
      if (attrLength < 24) return 0;   // resident header is 24 bytes: common 16 + value length/offset/flags
      uint32_t valueLength = readLe32(attr + ATTR_RES_VALUE_LENGTH);
      uint16_t valueOffset = readLe16(attr + ATTR_RES_VALUE_OFFSET);
      if ((uint32_t)valueOffset + valueLength > attrLength) return 0;   // value must fit the attribute
      out->value       = attr + valueOffset;
      out->valueLength = valueLength;
      return 1;
   }

   // non-resident
   if (attrLength < ATTR_NR_HEADER_MIN) return 0;
   out->runlistOffset = readLe16(attr + ATTR_NR_RUNLIST_OFFSET);
   if (out->runlistOffset < ATTR_NR_HEADER_MIN || out->runlistOffset > attrLength) return 0;
   out->startVcn      = readLe64(attr + ATTR_NR_START_VCN);
   out->lastVcn       = readLe64(attr + ATTR_NR_LAST_VCN);
   out->allocatedSize = readLe64(attr + ATTR_NR_ALLOC_SIZE);
   out->realSize      = readLe64(attr + ATTR_NR_REAL_SIZE);
   out->validSize     = readLe64(attr + ATTR_NR_VALID_SIZE);
   // jf-port: the compression unit applies only to a COMPRESSED attribute.  A sparse-only one
   // (flag 0x8000, which Windows and ntfs-3g also give a unit of 16 clusters) stores its allocated
   // clusters as plain data, so it must read through the normal runlist path, holes as zeros.
   // Upstream decoded those clusters as LZNT1 and failed any sparse file with a written extent.
   { uint16_t cu = readLe16(attr + ATTR_NR_COMPRESSION_UNIT);   // W10a: $LZNT1 compression-unit (power of 2)
     out->compUnitClusters = ((readLe16(attr + ATTR_FLAGS_OFFSET) & ATTR_FLAG_COMPRESSED) && cu > 0 && cu < 24)
                                ? (1u << cu) : 0; }
   return 1;
}

// Finds the first attribute of `type` (matching `name` of `nameLen` UTF-16 units when nameLen > 0;
// pass NULL/0 for the common unnamed case, e.g. the unnamed $DATA) by walking the attribute chain
// with bounds + a forward-progress/iteration guard against a cyclic or overlong chain. Returns 1
// found (fills *out), 0 not present, -1 malformed.
static int findAttribute(const uint8_t *record, uint32_t recordSize, uint32_t type,
                         const uint16_t *name, uint8_t nameLen, NtfsAttr *out)
{
   uint32_t usedSize = readLe32(record + FILE_USED_SIZE);
   uint32_t limit    = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset   = readLe16(record + FILE_FIRST_ATTR_OFFSET);

   for (int guard = 0; guard < 256; guard++) {   // a record can't hold anywhere near 256 attributes
      // read the type (the end marker is just this 4-byte word), then validate a full common header
      if (offset + 4 > limit) return -1;
      uint32_t attrType = readLe32(record + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) return 0;
      if (offset + 16 > limit) return -1;
      uint32_t attrLength = readLe32(record + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) return -1;   // offset <= limit, so no overflow

      // on a type hit, optionally match the UTF-16 attribute name
      if (attrType == type) {
         int nameOk = (record[offset + ATTR_NAME_LENGTH] == nameLen);
         if (nameOk && nameLen > 0) {
            uint16_t nameOffset = readLe16(record + offset + ATTR_NAME_OFFSET);
            if ((uint32_t)nameOffset + (uint32_t)nameLen * 2 > attrLength) return -1;
            for (uint8_t i = 0; i < nameLen && nameOk; i++)
               if (readLe16(record + offset + nameOffset + i * 2) != name[i]) nameOk = 0;
         }
         if (nameOk) return parseAttribute(record + offset, attrLength, out) ? 1 : -1;
      }
      offset += attrLength;
   }
   return -1;   // no end marker within the bound -> malformed
}

// ===========================================================================
// Runlist (data run) decoding. A non-resident attribute's content is a list of
// runs: each run's header byte packs the byte-width of the run length (low
// nibble) and of the LCN offset (high nibble). The LCN offset is a *signed*
// delta from the previous run's LCN (sign-extended from its width); a zero
// offset width is a sparse run (a hole that reads as zeros). A 0x00 header byte
// terminates the list.
// ===========================================================================

// A cursor over an encoded runlist. Tracks the accumulated absolute LCN (updated only by real
// runs, so a sparse run does not perturb the next run's delta base) and the running VCN.
typedef struct {
   const uint8_t *runlist;
   uint32_t length;       // bytes available (bound)
   uint32_t offset;       // current parse position
   int64_t  currentLcn;   // accumulated LCN of the last real run
   uint64_t currentVcn;   // VCN at the start of the next run
   int      done;         // hit the terminator
} NtfsRunlist;

static void openRunlist(NtfsRunlist *cursor, const uint8_t *runlist, uint32_t length)
{
   cursor->runlist = runlist;
   cursor->length  = length;
   cursor->offset  = 0;
   cursor->currentLcn = 0;
   cursor->currentVcn = 0;
   cursor->done = 0;
}

// Decodes the next run: sets *vcn/*lcn/*count and returns 1 (lcn < 0 for a sparse hole), returns
// 0 at the terminator, or -1 on a malformed runlist.
static int nextRun(NtfsRunlist *cursor, uint64_t *vcn, int64_t *lcn, uint64_t *count)
{
   if (cursor->done) return 0;
   if (cursor->offset >= cursor->length) return -1;        // ran off the end with no terminator

   // header: low nibble = length byte-width, high nibble = offset byte-width
   uint8_t header = cursor->runlist[cursor->offset++];
   if (header == 0) { cursor->done = 1; return 0; }        // terminator
   uint32_t lengthBytes = header & 0x0F;
   uint32_t offsetBytes = (header >> 4) & 0x0F;
   if (lengthBytes == 0 || lengthBytes > 8 || offsetBytes > 8) return -1;
   if (cursor->offset + lengthBytes + offsetBytes > cursor->length) return -1;

   // run length (unsigned, little-endian)
   uint64_t runLength = 0;
   for (uint32_t i = 0; i < lengthBytes; i++)
      runLength |= (uint64_t)cursor->runlist[cursor->offset + i] << (8 * i);
   cursor->offset += lengthBytes;
   if (runLength == 0) return -1;                           // a zero-length run is malformed

   *vcn = cursor->currentVcn;
   *count = runLength;

   if (offsetBytes == 0) {
      *lcn = -1;                                            // sparse: hole, currentLcn unchanged
   } else {
      // signed LCN delta, little-endian, sign-extended from its top byte
      uint64_t raw = 0;
      for (uint32_t i = 0; i < offsetBytes; i++)
         raw |= (uint64_t)cursor->runlist[cursor->offset + i] << (8 * i);
      uint32_t shift = 64 - 8 * offsetBytes;
      int64_t delta = (int64_t)(raw << shift) >> shift;     // arithmetic shift sign-extends the delta
      cursor->offset += offsetBytes;
      cursor->currentLcn += delta;
      if (cursor->currentLcn < 0) return -1;                // a real run can't map below LCN 0
      *lcn = cursor->currentLcn;
   }
   cursor->currentVcn += runLength;
   return 1;
}

// Decodes $MFT's own $DATA runlist into vol->mftRuns so any record can be located. Returns 0 on a
// clean terminator, -1 if malformed, sparse (the MFT never is), or too fragmented for the cache.
static int decodeMftRunlist(NtfsVolume *vol, const NtfsAttr *data)
{
   NtfsRunlist cursor;
   openRunlist(&cursor, data->attr + data->runlistOffset, data->attrLength - data->runlistOffset);
   vol->mftRunCount = 0;
   uint64_t vcn, count;
   int64_t  lcn;
   int rc;
   while ((rc = nextRun(&cursor, &vcn, &lcn, &count)) == 1) {
      if (lcn < 0) return -1;                               // $MFT is never sparse
      if (vol->mftRunCount >= NTFS_MFT_RUNS_MAX) return -1; // too fragmented for the bounded cache
      vol->mftRuns[vol->mftRunCount].vcn   = vcn;
      vol->mftRuns[vol->mftRunCount].lcn   = lcn;
      vol->mftRuns[vol->mftRunCount].count = count;
      vol->mftRunCount++;
   }
   return rc;   // 0 clean, -1 malformed
}

// Maps a VCN to its absolute LCN via a decoded runlist. Returns the LCN, or -1 if the VCN is in a
// sparse hole or maps outside the runlist.
static int64_t mapVcnToLcn(const NtfsRunEntry *runs, int runCount, uint64_t vcn)
{
   for (int i = 0; i < runCount; i++) {
      if (vcn >= runs[i].vcn && vcn < runs[i].vcn + runs[i].count) {
         if (runs[i].lcn < 0) return -1;                    // sparse
         return runs[i].lcn + (int64_t)(vcn - runs[i].vcn);
      }
   }
   return -1;
}

// Like mapVcnToLcn but also reports, in *contig, how many physically-contiguous clusters map from
// `vcn` onward (consecutive VCNs -> consecutive LCNs, absorbing adjacent runs) so a multi-cluster
// read/write can issue ONE storage call. *contig spans the sparse run on a hole. Returns -1 sparse,
// -2 if vcn is unmapped.
static int64_t mapRunsSpan(const NtfsRunEntry *runs, int runCount, uint64_t vcn, uint64_t *contig)
{
   for (int i = 0; i < runCount; i++) {
      if (vcn < runs[i].vcn || vcn >= runs[i].vcn + runs[i].count) continue;
      uint64_t offsetInRun = vcn - runs[i].vcn;
      if (runs[i].lcn < 0) { *contig = runs[i].count - offsetInRun; return -1; }   // sparse hole
      int64_t  startLcn = runs[i].lcn + (int64_t)offsetInRun;
      uint64_t span     = runs[i].count - offsetInRun;
      int64_t  nextLcn  = runs[i].lcn + (int64_t)runs[i].count;
      for (int j = i + 1; j < runCount && runs[j].lcn == nextLcn; j++) {           // fold physically-adjacent runs
         span    += runs[j].count;
         nextLcn += (int64_t)runs[j].count;
      }
      *contig = span;
      return startLcn;
   }
   return -2;
}

// Reads FILE record `number` via the decoded $MFT runlist (so it works for any record, fragmented
// or not), fixes it up and validates it. Handles records smaller than a cluster (several per
// cluster) and records spanning multiple clusters (read per cluster, so a run boundary mid-record
// is handled). Returns 0 on success with the record in `out`.
// Reads record `number` off disk via the $MFT runlist and applies the USA fixup, with no validity
// check. A not-in-use (freed or freshly grown) record reads back fine - the in-use/structural checks
// are layered on top by readMftRecord. Returns 0 on a clean read.
static int readMftRecordBytes(const NtfsVolume *vol, uint64_t number, uint8_t *out)
{
   uint32_t recordSize     = vol->mftRecordSize;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t byteOffset     = number * recordSize;

   if (recordSize <= bytesPerCluster) {
      // one cluster holds the whole record (possibly several records per cluster)
      int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster);
      if (lcn < 0) return -1;
      uint32_t inCluster = (uint32_t)(byteOffset % bytesPerCluster);
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + inCluster / vol->bytesPerSector;
      if (readSectors(vol->storageHandle, lba, recordSize / vol->bytesPerSector, out) != 0) return -1;
   } else {
      // record spans several clusters: read each one (handles a run boundary inside the record)
      uint32_t clusters = recordSize / bytesPerCluster;
      for (uint32_t c = 0; c < clusters; c++) {
         int64_t lcn = mapVcnToLcn(vol->mftRuns, vol->mftRunCount, byteOffset / bytesPerCluster + c);
         if (lcn < 0) return -1;
         uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster;
         if (readSectors(vol->storageHandle, lba, bytesPerCluster / vol->bytesPerSector, out + c * bytesPerCluster) != 0)
            return -1;
      }
   }
   if (applyUsaFixup(out, recordSize, vol->bytesPerSector) != 0) return -1;
   return 0;
}

static int readMftRecord(const NtfsVolume *vol, uint64_t number, uint8_t *out)
{
   if (readMftRecordBytes(vol, number, out) != 0) return -1;
   if (!isValidFileRecord(out, vol->mftRecordSize)) return -1;
   return 0;
}

// Reads an MFT record named by an 8-byte FILE REFERENCE: low 48 bits = entry index, high 16
// = sequence number. Validates the sequence (F048): when an MFT record is deleted and reused
// its sequence is bumped (see freeMftRecord), so a stale reference (right index, old sequence)
// must be refused rather than silently resolved to the reused record. The threat case is
// references read from untrusted on-disk metadata ($ATTRIBUTE_LIST, directory index entries,
// parent pointers); route every reference-based record read through this helper.
//
// wantSeq == 0 is the DELIBERATE NTFS CONVENTION for "unchecked": a bare record NUMBER (a
// system file such as $MFT/$Volume/$Bitmap, or an internally-generated/just-allocated record)
// has no sequence, so callers that legitimately read by index pass it with the high 16 bits
// zero and the sequence check is skipped. Real on-disk file references always carry a nonzero
// sequence, so this never weakens validation of attacker-controlled refs.
static int readMftRecordByRef(const NtfsVolume *vol, uint64_t fullRef, uint8_t *out)
{
   if (readMftRecord(vol, fullRef & MFT_REF_MASK, out) != 0) return -1;
   uint16_t wantSeq = (uint16_t)(fullRef >> 48);
   if (wantSeq != 0 && readLe16(out + FILE_SEQUENCE_NUMBER) != wantSeq) return -1;
   return 0;
}

// ===========================================================================
// File data read (resident, non-resident via runlist, sparse holes, ValidDataLength).
// ===========================================================================

// Like mapRunlistVcn but also reports, in *contig, how many physically-contiguous clusters map from
// `vcn` onward (absorbing adjacent runs) so a multi-cluster read/write can issue ONE storage call.
// *contig spans the sparse run on a hole. Returns -1 sparse, -2 malformed / vcn beyond the runlist.
static int64_t mapRunlistSpan(const uint8_t *runlist, uint32_t length, uint64_t vcn, uint64_t *contig)
{
   NtfsRunlist cursor;
   openRunlist(&cursor, runlist, length);
   uint64_t runVcn, runCount;
   int64_t  runLcn;
   int64_t  startLcn = -2, nextLcn = 0;
   uint64_t span = 0;
   int rc;
   while ((rc = nextRun(&cursor, &runVcn, &runLcn, &runCount)) == 1) {
      if (startLcn == -2) {                                 // still locating the run that holds vcn
         if (vcn < runVcn || vcn >= runVcn + runCount) continue;
         uint64_t offsetInRun = vcn - runVcn;
         if (runLcn < 0) { *contig = runCount - offsetInRun; return -1; }   // sparse hole
         startLcn = runLcn + (int64_t)offsetInRun;
         span     = runCount - offsetInRun;
         nextLcn  = runLcn + (int64_t)runCount;
      } else if (runLcn == nextLcn) {                       // physically-adjacent following run: extend the span
         span    += runCount;
         nextLcn += (int64_t)runCount;
      } else break;
   }
   if (startLcn == -2) return -2;
   *contig = span;
   return startLcn;
}

// ===========================================================================
// W10a: $LZNT1 decompression (read). A compressed $DATA stores each compression unit (cb) as a few
// real "compressed" clusters followed by sparse padding; each cb decompresses to cb_size bytes made of
// 4 KiB sub-blocks. We decode ONE sub-block at a time (LZNT1 back-references never cross a sub-block
// boundary) to keep the static footprint tiny instead of buffering a whole 64 KiB cb. Faithful port of
// ntfs-3g compress.c ntfs_decompress; spec is gold standard.
// ===========================================================================
#define NTFS_SB_SIZE        4096      // $LZNT1 sub-block plaintext size
#define NTFS_SB_SIZE_MASK   0x0fff
#define NTFS_SB_IS_COMPRESSED 0x8000

// Decompresses one $LZNT1 sub-block (`sb` = its 2-byte header + data, total `sbTotal` bytes) into
// plain[NTFS_SB_SIZE]. Returns the plaintext length (always NTFS_SB_SIZE; trailing bytes zero-filled),
// or -1 on malformed input. All compressed-stream accesses are bounds-checked (refuse, never over-read).
static int lznt1DecompressSb(const uint8_t *sb, uint32_t sbTotal, uint8_t *plain)
{
   if (sbTotal < 2) return -1;
   uint16_t hdr = readLe16(sb);
   const uint8_t *comp = sb + 2;
   const uint8_t *compEnd = sb + sbTotal;
   if (!(hdr & NTFS_SB_IS_COMPRESSED)) {                       // verbatim sub-block: exactly 4096 bytes
      if (sbTotal - 2 != NTFS_SB_SIZE) return -1;
      memCopy(plain, comp, NTFS_SB_SIZE);
      return NTFS_SB_SIZE;
   }
   uint32_t dpos = 0;                                          // position within the plaintext sub-block
   while (comp < compEnd && dpos < NTFS_SB_SIZE) {
      uint8_t tag = *comp++;
      for (int token = 0; token < 8 && comp < compEnd && dpos < NTFS_SB_SIZE; token++, tag >>= 1) {
         if ((tag & 1) == 0) { plain[dpos++] = *comp++; continue; }   // literal byte
         if (dpos == 0) return -1;                             // a phrase can't be the first token
         if (comp + 2 > compEnd) return -1;
         uint16_t pt = readLe16(comp); comp += 2;
         uint32_t lg = 0;
         for (uint32_t i = dpos - 1; i >= 0x10; i >>= 1) lg++; // log2 split point for this position
         uint32_t back   = (uint32_t)(pt >> (12 - lg)) + 1;    // bytes to go back
         uint32_t length = (uint32_t)(pt & (0xfff >> lg)) + 3; // bytes to copy
         if (back > dpos || dpos + length > NTFS_SB_SIZE) return -1;
         uint32_t src = dpos - back;
         for (uint32_t k = 0; k < length; k++) plain[dpos + k] = plain[src + k];   // byte-wise (overlap-safe)
         dpos += length;
      }
   }
   if (dpos < NTFS_SB_SIZE) memSet(plain + dpos, 0, NTFS_SB_SIZE - dpos);   // pad an incomplete sub-block
   return NTFS_SB_SIZE;
}

// Reads `chunk` real (non-sparse) bytes, already clamped to the contiguous span and ValidDataLength,
// starting at byte offset `inCluster` within cluster `lcn` into `out`. Whole aligned sectors go straight
// to the caller's buffer in one storage call (spanning clusters); a partial/unaligned edge bounces
// through the scratch. `spanBytes` is the contiguous real bytes available from here. Returns bytes
// read (may be < chunk when only whole sectors were taken), or -1. Shared by both non-resident readers.
static int64_t readSpanData(const NtfsVolume *vol, int64_t lcn, uint32_t inCluster, uint64_t spanBytes,
                            uint8_t *out, uint64_t chunk)
{
   uint32_t sectorSize   = vol->bytesPerSector;
   uint32_t offsetInSec  = inCluster % sectorSize;
   uint32_t alignedStart = inCluster - offsetInSec;
   uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + alignedStart / sectorSize;

   // fast path: sector-aligned start + aligned destination -> one storage call into the caller's buffer
   if (offsetInSec == 0 && chunk >= sectorSize && isDmaAligned(out)) {
      uint32_t sectors = (uint32_t)(chunk / sectorSize);
      if (readSectors(vol->storageHandle, lba, sectors, out) != 0) return -1;
      return (int64_t)sectors * sectorSize;            // partial trailing sector handled on the next iteration
   }

   // bounce path: partial leading sector or unaligned destination. Read whole sectors from the aligned
   // start (capped at the bounce) through the scratch, then copy out the requested slice.
   uint64_t windowBytes = spanBytes + offsetInSec;     // whole-sector bytes from alignedStart
   if (windowBytes > NTFS_READ_BOUNCE) windowBytes = NTFS_READ_BOUNCE;
   if (readSectors(vol->storageHandle, lba, (uint32_t)windowBytes / sectorSize, fileBounce) != 0) return -1;
   uint64_t avail = windowBytes - offsetInSec;
   if (chunk > avail) chunk = avail;
   memCopy(out, fileBounce + offsetInSec, (int)chunk);
   return (int64_t)chunk;
}

// Reads `want` bytes at file offset `pos` from a non-resident $DATA: maps each contiguous span via the
// runlist, zero-fills sparse holes and anything at/after ValidDataLength, and reads real data via
// readSpanData. Returns bytes read (== want on success) or -1 on error.
static int64_t readNonResident(const NtfsVolume *vol, const NtfsAttr *data, uint64_t validSize,
                               uint64_t pos, uint8_t *out, uint64_t want)
{
   const uint8_t *runlist = data->attr + data->runlistOffset;
   uint32_t runlistLength = data->attrLength - data->runlistOffset;
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t done = 0;

   while (done < want) {
      uint64_t fileOffset = pos + done;
      uint64_t vcn        = fileOffset / bytesPerCluster;
      uint32_t inCluster  = (uint32_t)(fileOffset % bytesPerCluster);

      // bytes past ValidDataLength read as zeros (initialized-size semantics); clamp each window to VDL
      uint64_t chunk = want - done;
      if (fileOffset >= validSize) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }
      if (fileOffset + chunk > validSize) chunk = validSize - fileOffset;

      uint64_t contig = 0;
      int64_t  lcn = mapRunlistSpan(runlist, runlistLength, vcn, &contig);
      if (lcn == -2) return -1;
      uint64_t spanBytes = contig * bytesPerCluster - inCluster;       // contiguous real bytes from here
      if (chunk > spanBytes) chunk = spanBytes;
      if (lcn == -1) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }   // sparse hole

      int64_t n = readSpanData(vol, lcn, inCluster, spanBytes, out + done, chunk);
      if (n < 0) return -1;
      done += (uint64_t)n;
   }
   return (int64_t)done;
}

// ===========================================================================
// W8a read path: $ATTRIBUTE_LIST. When a file/dir spreads across several MFT records, the base
// record carries an $ATTRIBUTE_LIST mapping each attribute instance (type+name+startVCN) to the
// record that houses it. The helpers below follow that list and merge a non-resident attribute's
// runlist fragments (held in different records) into one decoded runlist, so the existing readers
// work unchanged. A single-record file (no $ATTRIBUTE_LIST) takes the fast path untouched.
// ===========================================================================

// Compares an attribute name (UTF-16LE) in a record against `name`/`nameLen`. Returns 1 on match.
static int attrNameMatches(const uint8_t *attr, uint32_t attrLength, const uint16_t *name, uint8_t nameLen)
{
   if (attr[ATTR_NAME_LENGTH] != nameLen) return 0;
   if (nameLen == 0) return 1;
   uint16_t nameOff = readLe16(attr + ATTR_NAME_OFFSET);
   if ((uint32_t)nameOff + (uint32_t)nameLen * 2 > attrLength) return 0;
   for (uint8_t i = 0; i < nameLen; i++)
      if (readLe16(attr + nameOff + i * 2) != name[i]) return 0;
   return 1;
}

// Finds the attribute instance in `record` matching (type, name, attribute-id). Like findAttribute
// but also matches the id, so a specific fragment named by an $ATTRIBUTE_LIST entry is selected even
// when a record holds several instances of the same type. Returns 1 (fills *out), 0 absent, -1 bad.
static int findAttributeInstance(const uint8_t *record, uint32_t recordSize, uint32_t type,
                                 const uint16_t *name, uint8_t nameLen, uint16_t attrId, NtfsAttr *out)
{
   uint32_t usedSize = readLe32(record + FILE_USED_SIZE);
   uint32_t limit    = usedSize <= recordSize ? usedSize : recordSize;
   uint32_t offset   = readLe16(record + FILE_FIRST_ATTR_OFFSET);
   for (int guard = 0; guard < 256; guard++) {
      if (offset + 4 > limit) return -1;
      uint32_t attrType = readLe32(record + offset + ATTR_TYPE_OFFSET);
      if (attrType == ATTR_END) return 0;
      if (offset + 16 > limit) return -1;
      uint32_t attrLength = readLe32(record + offset + ATTR_LENGTH_OFFSET);
      if (attrLength < 16 || attrLength > limit - offset) return -1;
      if (attrType == type && readLe16(record + offset + ATTR_ID_OFFSET) == attrId &&
          attrNameMatches(record + offset, attrLength, name, nameLen))
         return parseAttribute(record + offset, attrLength, out) ? 1 : -1;
      offset += attrLength;
   }
   return -1;
}

// Reads `want` bytes at `pos` from a decoded (possibly multi-fragment) runlist — the merged-runlist
// twin of readNonResident. Zero-fills sparse holes and bytes at/after validSize. Returns bytes read
// or -1. (Mirrors readNonResident exactly but maps VCN via the decoded runs[] instead of raw pairs.)
static int64_t readRuns(const NtfsVolume *vol, const NtfsRunEntry *runs, int runCount, uint64_t validSize,
                        uint64_t pos, uint8_t *out, uint64_t want)
{
   uint32_t bytesPerCluster = vol->bytesPerCluster;
   uint64_t done = 0;
   while (done < want) {
      uint64_t fileOffset = pos + done;
      uint64_t vcn        = fileOffset / bytesPerCluster;
      uint32_t inCluster  = (uint32_t)(fileOffset % bytesPerCluster);

      uint64_t chunk = want - done;
      if (fileOffset >= validSize) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }
      if (fileOffset + chunk > validSize) chunk = validSize - fileOffset;

      uint64_t contig = 0;
      int64_t  lcn = mapRunsSpan(runs, runCount, vcn, &contig);
      if (lcn == -2) return -1;
      uint64_t spanBytes = contig * bytesPerCluster - inCluster;
      if (chunk > spanBytes) chunk = spanBytes;
      if (lcn == -1) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }   // sparse / hole

      int64_t n = readSpanData(vol, lcn, inCluster, spanBytes, out + done, chunk);
      if (n < 0) return -1;
      done += (uint64_t)n;
   }
   return (int64_t)done;
}

// Merges every fragment of attribute (type,name) in a file into one decoded runlist, following the
// base record's $ATTRIBUTE_LIST. `baseBuf` holds the base record (number `baseRef`); extension
// records load into extRecord. Returns 0 (fills runs/count + sizes from the startVCN==0 fragment),
// 1 if the attribute is resident in a single record (caller reads its value directly), or -1 on
// error/refuse (non-resident $ATTRIBUTE_LIST, too many fragments/runs, malformed, unreadable). The
// fast path (no $ATTRIBUTE_LIST) decodes the base instance directly.
static int gatherRuns(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                      uint32_t type, const uint16_t *name, uint8_t nameLen,
                      NtfsRunEntry *runs, int maxRuns, int *runCount,
                      uint64_t *realSize, uint64_t *validSize, uint64_t *allocSize)
{
   uint32_t recSize = vol->mftRecordSize;
   *runCount = 0; *realSize = *validSize = *allocSize = 0;

   NtfsAttr listAttr;
   if (findAttribute(baseBuf, recSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1) {
      NtfsAttr a;                                            // no list: the whole attribute is in the base
      if (findAttribute(baseBuf, recSize, type, name, nameLen, &a) != 1) return -1;
      if (a.resident) return 1;
      if (a.startVcn != 0) return -1;
      *realSize = a.realSize; *validSize = a.validSize; *allocSize = a.allocatedSize;
      return decodeRuns(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, runs, maxRuns, runCount) == 0 ? 0 : -1;
   }
   if (!listAttr.resident) return -1;                        // non-resident $ATTRIBUTE_LIST: deferred (very rare)

   const uint8_t *L = listAttr.value;
   uint32_t Llen = listAttr.valueLength, off = 0;
   int frags = 0, sawResident = 0, sawFirst = 0;
   uint64_t expectVcn = 0;                                   // next fragment must start exactly here (tile, in order)
   for (int guard = 0; guard < 8192; guard++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;   // end / malformed: stop
      // does this list entry name our (type, name)?
      int match = (readLe32(L + off + AL_TYPE) == type && L[off + AL_NAME_LENGTH] == nameLen);
      if (match && nameLen > 0) {
         uint8_t no = L[off + AL_NAME_OFFSET];
         if ((uint32_t)no + (uint32_t)nameLen * 2 > elen) match = 0;
         for (uint8_t i = 0; i < nameLen && match; i++)
            if (readLe16(L + off + no + i * 2) != name[i]) match = 0;
      }
      if (match) {
         {
            uint64_t evcn = readLe64(L + off + AL_START_VCN);
            uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
            uint64_t eref = erefFull & MFT_REF_MASK;
            uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
            const uint8_t *rec = baseBuf;
            if (eref != baseRef) { if (readMftRecordByRef(vol, erefFull, extRecord) != 0) return -1; rec = extRecord; }
            NtfsAttr a;
            if (findAttributeInstance(rec, recSize, type, name, nameLen, eid, &a) != 1) return -1;
            if (a.resident) { sawResident = 1; break; }       // resident single instance: caller handles
            // Fragments must tile the VCN space contiguously and in order: the first names VCN 0 (and
            // carries the authoritative sizes), each next starts exactly where the previous ended. A
            // malformed list with a gap, overlap or out-of-order fragment is refused, never silently
            // zero-filled (gap) or shadowed (overlap).
            if (evcn != expectVcn) return -1;
            if (frags >= NTFS_MAX_EXTENTS) return -1;         // refuse before appending past the fragment cap
            frags++;
            if (evcn == 0) { sawFirst = 1; *realSize = a.realSize; *validSize = a.validSize; *allocSize = a.allocatedSize; }
            NtfsRunEntry frag[NTFS_MAX_FILE_RUNS]; int fc = 0;
            if (decodeRuns(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, frag, NTFS_MAX_FILE_RUNS, &fc) != 0) return -1;
            for (int i = 0; i < fc; i++) {
               if (*runCount >= maxRuns) return -1;
               runs[*runCount] = frag[i];
               runs[*runCount].vcn += evcn;                   // decoded VCNs are fragment-relative; make absolute
               expectVcn += frag[i].count;                    // advance the expected next-fragment VCN
               (*runCount)++;
            }
         }
      }
      off += elen;
   }
   if (sawResident) return 1;
   return (sawFirst && *runCount > 0) ? 0 : -1;               // require the startVCN==0 fragment (sizes set)
}

// Streams one fragment's mapping pairs (VCNs relative to fragBaseVcn) and appends to out[] the runs
// overlapping [winStart, winEnd), each clipped to the window (LCN advanced into the clip). Never stores
// more than the window's worth of runs, so a fragment with thousands of runs costs O(1) memory. Returns
// 0, or -1 on a malformed runlist or if the (bounded) window output overflows.
static int streamFragWindow(const uint8_t *rl, uint32_t rlLen, uint64_t fragBaseVcn,
                            uint64_t winStart, uint64_t winEnd, NtfsRunEntry *out, int maxOut, int *outCount)
{
   NtfsRunlist cur; openRunlist(&cur, rl, rlLen);
   uint64_t rvcn, rcount; int64_t rlcn; int rc;
   while ((rc = nextRun(&cur, &rvcn, &rlcn, &rcount)) == 1) {
      uint64_t a = fragBaseVcn + rvcn, b = a + rcount;
      if (b <= winStart) continue;                             // entirely before the window
      if (a >= winEnd) break;                                  // runs are VCN-ascending: nothing later overlaps
      uint64_t lo = a > winStart ? a : winStart;
      uint64_t hi = b < winEnd ? b : winEnd;
      if (*outCount >= maxOut) return -1;
      out[*outCount].vcn   = lo;
      out[*outCount].count = hi - lo;
      out[*outCount].lcn   = (rlcn < 0) ? -1 : (rlcn + (int64_t)(lo - a));   // advance the LCN into the clipped start
      (*outCount)++;
   }
   return (rc < 0) ? -1 : 0;
}

// Maps [vcnStart, vcnStart+vcnCount) of a (possibly $ATTRIBUTE_LIST-spanned) non-resident attribute to
// runs, WITHOUT materializing the whole runlist: it streams the mapping pairs of each covering fragment
// and keeps only the runs intersecting the window. This is what lets the driver read a large $LZNT1 file
// whose full runlist (~2 runs per compression unit) far exceeds NTFS_MAX_FILE_RUNS — each read maps one
// compression unit (<= cbClusters clusters) at a time. Housing extension records are read into extRecord;
// baseBuf must remain valid (the $ATTRIBUTE_LIST is read from it). Returns 0 (sets *outCount) or -1.
static int mapVcnWindow(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                        uint32_t type, const uint16_t *name, uint8_t nameLen,
                        uint64_t vcnStart, uint64_t vcnCount, NtfsRunEntry *out, int maxOut, int *outCount)
{
   uint32_t recSize = vol->mftRecordSize;
   uint64_t winEnd = vcnStart + vcnCount;
   *outCount = 0;

   NtfsAttr listAttr;
   if (findAttribute(baseBuf, recSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1) {
      NtfsAttr a;                                              // no list: one runlist in the base
      if (findAttribute(baseBuf, recSize, type, name, nameLen, &a) != 1 || a.resident) return -1;
      return streamFragWindow(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, 0,
                              vcnStart, winEnd, out, maxOut, outCount);
   }
   if (!listAttr.resident) return -1;                          // non-resident $ATTRIBUTE_LIST: unsupported

   const uint8_t *L = listAttr.value; uint32_t Llen = listAttr.valueLength, off = 0;
   for (int guard = 0; guard < 8192; guard++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;
      int match = (readLe32(L + off + AL_TYPE) == type && L[off + AL_NAME_LENGTH] == nameLen);
      if (match && nameLen > 0) {
         uint8_t no = L[off + AL_NAME_OFFSET];
         if ((uint32_t)no + (uint32_t)nameLen * 2 > elen) match = 0;
         for (uint8_t i = 0; i < nameLen && match; i++)
            if (readLe16(L + off + no + i * 2) != name[i]) match = 0;
      }
      if (match) {
         uint64_t evcn = readLe64(L + off + AL_START_VCN);
         if (evcn < winEnd) {                                  // this fragment may overlap the window
            uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
            uint64_t eref = erefFull & MFT_REF_MASK;
            uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
            const uint8_t *rec = baseBuf;
            if (eref != baseRef) { if (readMftRecordByRef(vol, erefFull, extRecord) != 0) return -1; rec = extRecord; }
            NtfsAttr a;
            if (findAttributeInstance(rec, recSize, type, name, nameLen, eid, &a) != 1) return -1;
            if (!a.resident &&
                streamFragWindow(a.attr + a.runlistOffset, a.attrLength - a.runlistOffset, evcn,
                                 vcnStart, winEnd, out, maxOut, outCount) != 0) return -1;
         }
      }
      off += elen;
   }
   return 0;
}

// W10a: reads `want` bytes at `pos` from an $LZNT1-compressed non-resident $DATA. The runlist is mapped
// one compression unit at a time via mapVcnWindow (baseBuf + baseRef locate it, following $ATTRIBUTE_LIST),
// so this works for files of any size/fragmentation. Each compression unit (cbClusters clusters) is sparse
// (leading hole -> zeros), uncompressed (no holes -> raw), or compressed (leading real clusters + trailing
// holes -> decode its 4 KiB sub-blocks). Reads past validSize are zeros. Returns bytes read (== want) / -1.
static int64_t readCompressed(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                              const uint16_t *name, uint8_t nameLen,
                              uint32_t cbClusters, uint64_t validSize, uint64_t pos, uint8_t *out, uint64_t want)
{
   uint32_t clusterBytes = vol->bytesPerCluster;
   uint64_t cbSize = (uint64_t)cbClusters * clusterBytes;
   if (cbClusters == 0 || cbSize == 0 || cbClusters > NTFS_MAX_CB_CLUSTERS) return -1;
   if (cbSize > 0x40000000u) return -1;                     // keep cbSize (and the uint32 offsets below) 31-bit-safe
   uint64_t done = 0;
   while (done < want) {
      uint64_t fileOff = pos + done;
      if (fileOff >= validSize) { memSet(out + done, 0, (int)(want - done)); done = want; break; }
      uint64_t unitVcn = (fileOff / cbSize) * cbClusters;       // first VCN of this compression unit
      uint32_t offInUnit = (uint32_t)(fileOff % cbSize);
      uint64_t chunk = cbSize - offInUnit;
      if (chunk > want - done) chunk = want - done;
      if (fileOff + chunk > validSize) chunk = validSize - fileOff;

      // map just this unit's clusters (bounded), then classify by counting leading real clusters
      NtfsRunEntry runs[NTFS_CB_MAX_RUNS]; int runCount = 0;
      if (mapVcnWindow(vol, baseBuf, baseRef, ATTR_DATA, name, nameLen, unitVcn, cbClusters, runs, NTFS_CB_MAX_RUNS, &runCount) != 0) return -1;

      int hasHole = 0; uint32_t realClusters = 0;
      for (uint32_t c = 0; c < cbClusters; c++) {
         int64_t l = mapVcnToLcn(runs, runCount, unitVcn + c);
         if (l < 0) { hasHole = 1; break; }
         realClusters++;
      }
      if (realClusters == 0) { memSet(out + done, 0, (int)chunk); done += chunk; continue; }   // sparse unit

      if (!hasHole) {                                           // uncompressed unit: read raw
         if (readRuns(vol, runs, runCount, validSize, fileOff, out + done, chunk) != (int64_t)chunk) return -1;
         done += chunk; continue;
      }

      // compressed unit: walk its sub-blocks from the unit start; decode those overlapping the request.
      uint64_t unitByte = unitVcn * clusterBytes;
      uint64_t realBytes = (uint64_t)realClusters * clusterBytes;
      uint32_t compOff = 0;                                     // byte offset of the current sub-block in the unit
      uint32_t outUnitPos = 0;                                  // plaintext byte offset within the unit
      for (int sbGuard = 0; sbGuard < (int)(cbSize / NTFS_SB_SIZE) + 1 && outUnitPos < cbSize; sbGuard++) {
         if (compOff + 2 > realBytes) break;                    // no more sub-blocks
         uint32_t avail = (uint32_t)(realBytes - compOff);
         uint32_t rd = avail < (uint32_t)sizeof lzComp ? avail : (uint32_t)sizeof lzComp;
         if (readRuns(vol, runs, runCount, unitByte + realBytes, unitByte + compOff, lzComp, rd) != (int64_t)rd) return -1;
         uint16_t sbHdr = readLe16(lzComp);
         if (sbHdr == 0) break;                                 // end of compressed data
         uint32_t sbTotal = (uint32_t)(sbHdr & NTFS_SB_SIZE_MASK) + 3;
         if (sbTotal > rd) return -1;                           // sub-block runs past the real data
         if (lznt1DecompressSb(lzComp, sbTotal, lzPlain) != NTFS_SB_SIZE) return -1;
         // copy the part of this sub-block (plaintext [outUnitPos, outUnitPos+NTFS_SB_SIZE)) that
         // overlaps the requested [offInUnit, offInUnit+chunk)
         uint32_t sbStart = outUnitPos, sbEnd = outUnitPos + NTFS_SB_SIZE;
         uint32_t reqStart = offInUnit, reqEnd = offInUnit + (uint32_t)chunk;
         uint32_t lo = sbStart > reqStart ? sbStart : reqStart;
         uint32_t hi = sbEnd < reqEnd ? sbEnd : reqEnd;
         if (lo < hi) memCopy(out + done + (lo - reqStart), lzPlain + (lo - sbStart), (int)(hi - lo));
         compOff += sbTotal;
         outUnitPos += NTFS_SB_SIZE;
      }
      // any plaintext the unit didn't produce within the request reads as zero
      if (outUnitPos < offInUnit + (uint32_t)chunk) {
         uint32_t z0 = outUnitPos > offInUnit ? outUnitPos : offInUnit;
         uint32_t z1 = offInUnit + (uint32_t)chunk;
         if (z0 < z1) memSet(out + done + (z0 - offInUnit), 0, (int)(z1 - z0));
      }
      done += chunk;
   }
   return (int64_t)done;
}

// Parses the authoritative (lowest_vcn == 0) fragment of a (possibly $ATTRIBUTE_LIST-spanned) attribute
// into *out, WITHOUT decoding its runlist — so openFileByRef can learn a file's size and whether it is
// $LZNT1-compressed even when the runlist is far too large to decode whole. out->attr points into baseBuf
// or extRecord (valid until the next record read). Returns 1 resident, 0 non-resident, -1 not found/error.
static int firstFragmentInfo(NtfsVolume *vol, const uint8_t *baseBuf, uint64_t baseRef,
                             uint32_t type, const uint16_t *name, uint8_t nameLen, NtfsAttr *out)
{
   uint32_t recSize = vol->mftRecordSize;
   NtfsAttr listAttr;
   if (findAttribute(baseBuf, recSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1) {
      if (findAttribute(baseBuf, recSize, type, name, nameLen, out) != 1) return -1;
      return out->resident ? 1 : 0;
   }
   if (!listAttr.resident) return -1;
   const uint8_t *L = listAttr.value; uint32_t Llen = listAttr.valueLength, off = 0;
   for (int guard = 0; guard < 8192; guard++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;
      int match = (readLe32(L + off + AL_TYPE) == type && L[off + AL_NAME_LENGTH] == nameLen);
      if (match && nameLen > 0) {
         uint8_t no = L[off + AL_NAME_OFFSET];
         if ((uint32_t)no + (uint32_t)nameLen * 2 > elen) match = 0;
         for (uint8_t i = 0; i < nameLen && match; i++)
            if (readLe16(L + off + no + i * 2) != name[i]) match = 0;
      }
      // Only the lowest_vcn==0 extent's header carries the authoritative real/valid sizes, COMPRESSED/SPARSE
      // flags and compression_unit (later extents have compression_unit==0); that's the one we parse here.
      if (match && readLe64(L + off + AL_START_VCN) == 0) {
         uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
         uint64_t eref = erefFull & MFT_REF_MASK;
         uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
         const uint8_t *rec = baseBuf;
         if (eref != baseRef) { if (readMftRecordByRef(vol, erefFull, extRecord) != 0) return -1; rec = extRecord; }
         if (findAttributeInstance(rec, recSize, type, name, nameLen, eid, out) != 1) return -1;
         return out->resident ? 1 : 0;
      }
      off += elen;
   }
   return -1;
}

// Sets up an open file from its MFT reference and a $DATA stream name (W12a). `name`/`nameLen` select a
// named stream (file:stream); nameLen == 0 selects the unnamed main stream (the common case). Reads the
// record, finds that $DATA, and fills size / ValidDataLength / resident flag. Returns 0, -1 on error /
// no such stream (e.g. a directory, or a missing named stream), or -2 if the stream is encrypted.
static int openStreamByRef(NtfsFile *file, NtfsVolume *vol, uint64_t mftReference,
                           const uint16_t *name, uint8_t nameLen)
{
   memSet(file, 0, (int)sizeof(*file));
   file->vol          = vol;
   file->mftReference = mftReference;
   if (nameLen > 0) {                                         // remember the stream name so readNtfs re-finds it
      if (nameLen > 32) return -1;
      for (uint8_t i = 0; i < nameLen; i++) file->dataName[i] = name[i];
      file->dataNameLen = nameLen;
   }
   if (readMftRecordByRef(vol, mftReference, mftRecord) != 0) return -1;   // F048: masks + validates sequence

   NtfsAttr data, listAttr;
   int found = findAttribute(mftRecord, vol->mftRecordSize, ATTR_DATA, name, nameLen, &data);
   int haveList = (findAttribute(mftRecord, vol->mftRecordSize, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) == 1);
   // Self-contained base $DATA (the overwhelming common case): the base record holds the unnamed
   // $DATA, its first fragment (startVcn 0), and no $ATTRIBUTE_LIST relocating it. Fast path unchanged.
   if (found == 1 && !haveList) {
      file->resident   = data.resident;
      file->dataAttrId = data.attributeId;
      if (data.resident) {                                   // resident $DATA is never compressed/sparse
         file->size = data.valueLength; file->validSize = data.valueLength; file->compressed = 0;
         return 0;
      }
      if (data.startVcn != 0) return -1;
      file->size = data.realSize; file->validSize = data.validSize;
      if (readLe16(data.attr + ATTR_FLAGS_OFFSET) & ATTR_FLAG_ENCRYPTED) {   // EFS: opened-but-unsupported
         file->compressed = 1; return -2;
      }
      // W10a: an $LZNT1-compressed $DATA is readable via readCompressed; a sparse-only $DATA reads
      // through the normal reader (holes -> zeros). Both stay write-refused (file->compressed) until
      // W10b/W12. A plain file is fully writable.
      file->compUnitClusters = data.compUnitClusters;
      file->compressed = (data.compUnitClusters > 0 || data.compressed) ? 1 : 0;
      return 0;
   }

   // $DATA is relocated by an $ATTRIBUTE_LIST (resident-in-a-list-file, or spread across records). Peek the
   // authoritative (lowest_vcn 0) fragment first — without decoding the runlist — so a large $LZNT1 file
   // (whose runlist dwarfs NTFS_MAX_FILE_RUNS) is recognized as compressed and read by streaming.
   NtfsAttr first;
   int ff = firstFragmentInfo(vol, mftRecord, mftReference & MFT_REF_MASK, ATTR_DATA, name, nameLen, &first);
   if (ff < 0) return -1;                                   // no $DATA (directory) or malformed
   if (ff == 1) {                                           // resident $DATA (small; value lives in the base)
      if (found != 1) return -1;
      file->resident = 1; file->compressed = data.compressed; file->dataAttrId = data.attributeId;
      file->size = data.valueLength; file->validSize = data.valueLength;
      return file->compressed ? -2 : 0;
   }
   // non-resident, spanned. `first` holds the authoritative sizes + flags (attr ptr still valid).
   file->size = first.realSize; file->validSize = first.validSize; file->spanned = 1;
   if (readLe16(first.attr + ATTR_FLAGS_OFFSET) & ATTR_FLAG_ENCRYPTED) { file->compressed = 1; return -2; }
   if (first.compUnitClusters > 0) {                        // spanned + $LZNT1: read via readCompressed/mapVcnWindow
      if (first.compUnitClusters > NTFS_MAX_CB_CLUSTERS) { file->compressed = 1; return -2; }   // absurd unit: refuse
      file->compUnitClusters = first.compUnitClusters; file->compressed = 1;
      return 0;
   }
   // spanned, uncompressed: validate the merged runlist now (gatherRuns; reads use it too). A sparse-but-
   // not-LZNT1 $DATA also lands here (holes -> zeros via readRuns).
   NtfsRunEntry r[NTFS_MAX_FILE_RUNS]; int rc = 0; uint64_t rs, vs, as;
   int g = gatherRuns(vol, mftRecord, mftReference & MFT_REF_MASK, ATTR_DATA, name, nameLen, r, NTFS_MAX_FILE_RUNS, &rc, &rs, &vs, &as);
   if (g != 0) return -1;                                   // refused (e.g. too fragmented) or malformed
   file->resident = 0; file->compressed = first.compressed; file->size = rs; file->validSize = vs;
   return 0;
}

// Unnamed (main) $DATA stream — the overwhelming common case and every internal caller. Wraps
// openStreamByRef with no stream name.
static int openFileByRef(NtfsFile *file, NtfsVolume *vol, uint64_t mftReference)
{
   return openStreamByRef(file, vol, mftReference, 0, 0);
}

// ===========================================================================
// Mount / unmount.
// ===========================================================================
int mountNtfs(NtfsVolume *vol, int drive)
{
   memSet(vol, 0, (int)sizeof(*vol));

   // device geometry
   uint64_t deviceId = getUsbDeviceId(drive);
   StorageDeviceInfo info;
   int      haveInfo         = (getStorageInfo(deviceId, &info) == 0);
   uint32_t deviceSectorSize = haveInfo ? info.sectorSize  : 512;
   uint64_t deviceSectors    = haveInfo ? info.sectorCount : 0;   // 0 = unknown (skip the device-size bound)
   if (deviceSectorSize == 0 || deviceSectorSize > NTFS_MAX_SECTOR) return NTFS_MOUNT_NOT_READY;

   // open + settle (lv2 faults if the first read lands too soon after open)
   int storageHandle;
   if (openStorage(deviceId, &storageHandle) < 0) return NTFS_MOUNT_NOT_READY;
   sys_timer_usleep(SYSIO_SETTLE_US);

   // read LBA 0 and locate the NTFS volume (superfloppy, MBR or GPT partition)
   if (readSectors(storageHandle, 0, 1, bootSector) != 0) {
      closeStorage(storageHandle);
      return NTFS_MOUNT_NOT_READY;
   }
   uint64_t volStart = 0;
   if (!locateNtfsVolume(storageHandle, bootSector, scanScratch, vbrScratch,
                         deviceSectorSize, deviceSectors, &volStart)) {
      closeStorage(storageHandle);
      return NTFS_MOUNT_NOT_NTFS;
   }

   // validate + parse the (untrusted) geometry
   if (!parseNtfsBoot(bootSector, deviceSectorSize, deviceSectors, volStart, vol)) {
      closeStorage(storageHandle);
      return NTFS_MOUNT_NOT_NTFS;
   }

   // commit volume state
   vol->storageHandle    = storageHandle;
   vol->cacheEpoch       = (mountEpoch + 1) ? ++mountEpoch : (mountEpoch = 1);   // fresh cache key; skip 0
   vol->drive            = (uint8_t)drive;
   vol->deviceSectorSize = deviceSectorSize;
   vol->partitionOffset  = volStart;
   vol->allocHint        = 0;
   vol->mounted          = 1;

   // bootstrap check: $MFT's own record (0) must read, fix up and validate as a FILE record. A
   // volume whose $MFT entry is unreadable or garbage is not usable NTFS; rejecting here also
   // exercises the S2 record-read + USA-fixup path at every real mount.
   if (readMftRecordBootstrap(vol, MFT_RECORD_MFT, mftRecord) != 0) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }

   // S3: $MFT's own unnamed $DATA must be present and non-resident (the MFT data always is). This
   // exercises the attribute walk on the real volume and records the MFT data size (record count)
   // for the S4 runlist decode.
   NtfsAttr mftData;
   if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_DATA, 0, 0, &mftData) != 1 || mftData.resident) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   vol->mftDataSize = mftData.realSize;

   // S4: decode $MFT's runlist so any record is reachable, then prove the pipeline by reading the
   // root directory record (#5) via that runlist and confirming it's an in-use directory FILE
   // record. (decode reads from mftRecord, so it must run before readMftRecord overwrites it.)
   if (decodeMftRunlist(vol, &mftData) != 0) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   if (readMftRecord(vol, MFT_RECORD_ROOT, mftRecord) != 0 ||   // F048 ok: root dir by fixed system record number (seq-0 convention)
       !(readLe16(mftRecord + FILE_FLAGS) & FILE_FLAG_DIRECTORY)) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }

   // S5: read the $Boot file's non-resident $DATA and confirm its first sector equals the VBR we
   // already loaded. This byte-diffs the file-read path against a known reference on the real
   // volume (so a broken read engine fails the mount rather than silently returning bad bytes).
   NtfsFile bootFile;
   uint8_t firstSector[512];
   if (openFileByRef(&bootFile, vol, MFT_RECORD_BOOT) != 0) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   if (readNtfs(&bootFile, firstSector, 512) != 512) {
      closeStorage(storageHandle);
      vol->mounted = 0;
      return NTFS_MOUNT_NOT_NTFS;
   }
   for (int i = 0; i < 512; i++) {
      if (firstSector[i] != bootSector[i]) {
         closeStorage(storageHandle);
         vol->mounted = 0;
         return NTFS_MOUNT_NOT_NTFS;
      }
   }

   // jf-port: a volume is never written.  The writability gate (dirty flag, $LogFile
   // state, $Bitmap cache) is gone with the write path; writable stays 0.
   vol->writable = 0;
   vol->volumeDirty = 0;
   vol->versionMajor = 0;
   vol->versionMinor = 0;
   vol->label[0] = '\0';                                       // $VOLUME_NAME label (empty -> chooseSegment falls back)
   if (readMftRecord(vol, MFT_RECORD_VOLUME, mftRecord) == 0) {   // F048 ok: $Volume by fixed system record number (seq-0 convention)
      NtfsAttr volumeInfo;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_VOLUME_INFORMATION, 0, 0, &volumeInfo) == 1 &&
          volumeInfo.resident && volumeInfo.valueLength >= VOLINFO_FLAGS_OFFSET + 2) {
         vol->versionMajor = volumeInfo.value[VOLINFO_MAJOR_OFFSET];   // NTFS version, e.g. 3.1
         vol->versionMinor = volumeInfo.value[VOLINFO_MINOR_OFFSET];
      }

      // $VOLUME_NAME (type 0x60, resident UTF-16LE) -> UTF-8 display label, used as the mount segment
      // (parity with exFAT). Absent/empty leaves vol->label empty so chooseSegment uses "ntfs<port>".
      NtfsAttr volumeName;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_VOLUME_NAME, 0, 0, &volumeName) == 1 && volumeName.resident) {
         uint16_t units[64];
         uint32_t count = volumeName.valueLength / 2;
         if (count > 63) count = 63;
         for (uint32_t i = 0; i < count; i++) units[i] = readLe16(volumeName.value + i * 2);
         units[count] = 0;
         utf16ToUtf8(units, vol->label, (int)sizeof(vol->label));
      }
   }

   // Cache $Bitmap's runlist so the free-cluster scan and cluster (de)allocation reach bitmap sectors
   // directly, instead of re-opening and re-reading $Bitmap's MFT record on every block. Cached on
   // read-only mounts too: countNtfsFreeClusters below uses it, and gating this on `writable` meant
   // any read-only volume paid the slow path. If it can't be cached (a pathologically fragmented
   // bitmap) writes are refused and the scan falls back to the file engine.
   vol->bitmapRunCount = 0;
   vol->bitmapDataSize = 0;
   if (readMftRecord(vol, MFT_RECORD_BITMAP, mftRecord) == 0) {
      NtfsRunEntry runs[NTFS_MFT_RUNS_MAX]; int runCount = 0; uint64_t realSize, validSize, allocSize;
      if (gatherRuns(vol, mftRecord, MFT_RECORD_BITMAP, ATTR_DATA, 0, 0, runs, NTFS_MFT_RUNS_MAX, &runCount,
                     &realSize, &validSize, &allocSize) == 0) {
         for (int i = 0; i < runCount; i++) vol->bitmapRuns[i] = runs[i];
         vol->bitmapRunCount = runCount;
         vol->bitmapDataSize = realSize;   // bounds the scan: never read past $Bitmap's own data
      }
   }

   if (countNtfsFreeClusters(vol, &vol->freeClusters) != 0)   // seed once; maintained on alloc/free thereafter
      vol->freeClusters = 0;

   return NTFS_MOUNT_OK;
}

void unmountNtfs(NtfsVolume *vol)
{
   if (!vol->mounted) return;
   closeStorage(vol->storageHandle);
   vol->mounted = 0;
}

// ===========================================================================
// Directory traversal ($INDEX_ROOT + $INDEX_ALLOCATION, the $I30 index). Entries
// are enumerated iteratively across the root node and every index block (no
// recursive B-tree descent): each file's key appears exactly once in the tree, so
// visiting every node and emitting every real entry lists each file once.
// ===========================================================================

// 100ns ticks between 1601-01-01 and 1970-01-01; FILETIME (UTC) -> unix seconds.
static uint64_t filetimeToUnix(uint64_t filetime)
{
   uint64_t epoch = 116444736000000000ull;
   return filetime > epoch ? (filetime - epoch) / 10000000ull : 0;
}

// Fills *info and the UTF-8 name from a $FILE_NAME key (caller bounds-checked the key length).
static void fillFileNameInfo(const uint8_t *key, uint64_t fileRef, char *name, int nameCap, NtfsInfo *info)
{
   uint32_t fnFlags   = readLe32(key + FN_FLAGS);
   info->isDir        = (fnFlags & FN_FLAG_DIRECTORY) != 0;
   info->isReparse    = (fnFlags & FN_FLAG_REPARSE) != 0;   // W12b: symlink/junction/placeholder marker
   info->reparseTag   = 0;                                  // the tag lives in $REPARSE_POINT; statNtfs fills it
   info->attributes   = fnFlags;                            // DOS/Win32 FILE_ATTRIBUTE_* flags (read-only/hidden/system/...)
   info->size         = readLe64(key + FN_REAL_SIZE);
   info->validSize    = info->size;
   info->mtime        = filetimeToUnix(readLe64(key + FN_MODIFIED_TIME));
   info->mftReference = fileRef;

   uint8_t nameLength = key[FN_NAME_LENGTH];
   uint16_t units[256];
   for (uint8_t i = 0; i < nameLength; i++) units[i] = readLe16(key + FN_NAME + (uint32_t)i * 2);   // LE -> host
   units[nameLength] = 0;
   utf16ToUtf8(units, name, nameCap);
}

// Scans one index node for the next listable entry, resuming at *offset (a byte offset from the
// node header start). Skips the DOS short-name twin and the reserved system files, stops at the
// node's last-entry marker. Returns 1 (fills name/info, advances *offset), 0 at end of node.
static int scanIndexNode(const uint8_t *node, const uint8_t *hardEnd, uint32_t *offset,
                         char *name, int nameCap, NtfsInfo *info)
{
   uint32_t entriesOffset = readLe32(node + IDXNODE_ENTRIES_OFFSET);
   uint32_t usedSize      = readLe32(node + IDXNODE_USED_SIZE);
   uint32_t bufCap        = (uint32_t)(hardEnd - node);   // usedSize is attacker-controlled: never trust it past
   if (usedSize > bufCap) usedSize = bufCap;              // the real buffer, or an entry's key reads out of bounds
   uint32_t pos = *offset ? *offset : entriesOffset;

   for (int guard = 0; guard < 4096; guard++) {
      const uint8_t *entry = node + pos;
      if (pos + 16 > usedSize || entry + 16 > hardEnd) return 0;             // no room for an entry header
      uint16_t entryLength = readLe16(entry + IDXENTRY_LENGTH);
      uint16_t entryFlags  = readLe16(entry + IDXENTRY_FLAGS);
      if (entryFlags & IDXENTRY_FLAG_LAST) return 0;                         // last entry: end of node
      if (entryLength < 16 || pos + entryLength > usedSize) return 0;        // malformed -> stop
      uint16_t keyLength = readLe16(entry + IDXENTRY_KEY_LENGTH);
      uint32_t nextPos = pos + entryLength;

      // emit only a real, listable $FILE_NAME (skip DOS twins and reserved system files)
      const uint8_t *key = entry + IDXENTRY_KEY;
      uint64_t fileRef = readLe64(entry + IDXENTRY_FILE_REF);
      if (keyLength >= FN_MIN_SIZE && (uint32_t)16 + keyLength <= entryLength) {
         uint8_t nameLength = key[FN_NAME_LENGTH];
         if ((uint32_t)FN_NAME + (uint32_t)nameLength * 2 <= keyLength &&
             key[FN_NAMESPACE] != FN_NAMESPACE_DOS &&
             (fileRef & MFT_REF_MASK) >= NTFS_FIRST_USER_RECORD) {
            fillFileNameInfo(key, fileRef, name, nameCap, info);
            *offset = nextPos;
            return 1;
         }
      }
      pos = nextPos;
   }
   return 0;
}

// ===========================================================================
// Public read API - stubbed until S2-S7 land. Each returns a clean error so the
// VFS surface is wired and a probed NTFS volume mounts without breaking the
// router; browsing/reading becomes functional as the parsing stages land.
// ===========================================================================
// Number of $INDEX_ALLOCATION blocks worth scanning. Blocks past the real allocation read back as
// zeros (not errors), so a corrupt realSize/allocatedSize would otherwise spin the scan loop ~2^52
// times under the backend lock. Clamp to the real allocation and hard-cap by the volume size, so any
// header corruption still terminates. On a healthy volume allocatedSize >= realSize, so this is realSize.
static uint64_t indexBlockCount(const NtfsVolume *vol, const NtfsAttr *alloc, uint32_t blockSize)
{
   if (!blockSize) return 0;
   uint64_t bytes  = alloc->realSize < alloc->allocatedSize ? alloc->realSize : alloc->allocatedSize;
   uint64_t blocks = bytes / blockSize;
   uint64_t cap    = (vol->totalSectors * vol->bytesPerSector) / blockSize;   // can't exceed the volume
   return blocks < cap ? blocks : cap;
}

void openNtfsDir(NtfsDir *dir, const NtfsVolume *vol, uint64_t dirReference)
{
   memSet(dir, 0, (int)sizeof(*dir));
   dir->vol = vol;
   dir->dirReference = dirReference;
   dir->inRoot = 1;   // start in the resident $INDEX_ROOT node, then move to $INDEX_ALLOCATION blocks
}

// Returns one directory entry per call. Re-reads the directory's record and index block each call
// (the shared buffers can be reused between calls), resuming from the iterator position. The walk
// is: the $INDEX_ROOT node first, then every $INDEX_ALLOCATION block in turn.
int readNtfsDir(NtfsDir *dir, char *name, int nameCap, NtfsInfo *info)
{
   NtfsVolume *vol = (NtfsVolume *)dir->vol;
   if (!vol || !vol->mounted) { dir->ioError = 1; return 0; }

   for (;;) {
      // re-read the directory's FILE record and locate its $I30 index attributes
      if (readMftRecordByRef(vol, dir->dirReference, mftRecord) != 0) { dir->ioError = 1; return 0; }

      // phase 1: the resident $INDEX_ROOT node
      if (dir->inRoot) {
         NtfsAttr root;
         if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_INDEX_ROOT, indexNameI30, 4, &root) != 1 || !root.resident)
            return 0;   // a directory without a readable $INDEX_ROOT: nothing to list
         const uint8_t *node = root.value + IDXROOT_NODE_HEADER;
         const uint8_t *hardEnd = root.value + root.valueLength;
         if (scanIndexNode(node, hardEnd, &dir->entryOffset, name, nameCap, info)) return 1;
         dir->inRoot = 0; dir->indexVcn = 0; dir->entryOffset = 0;   // root exhausted -> index blocks
         continue;
      }

      // phase 2: the $INDEX_ALLOCATION blocks (absent for a small, root-only directory)
      NtfsAttr alloc;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_INDEX_ALLOCATION, indexNameI30, 4, &alloc) != 1 ||
          alloc.resident)
         return 0;   // no large index -> done
      uint32_t blockSize = vol->indexRecordSize;
      uint64_t totalBlocks = indexBlockCount(vol, &alloc, blockSize);

      while (dir->indexVcn < totalBlocks) {
         int64_t got = readNonResident(vol, &alloc, alloc.validSize, dir->indexVcn * blockSize, indexBuffer, blockSize);
         if (got != (int64_t)blockSize) { dir->ioError = 1; return 0; }
         // a valid, in-use block is an "INDX" record that fixes up; skip anything else
         if (indexBuffer[0] == 'I' && indexBuffer[1] == 'N' && indexBuffer[2] == 'D' && indexBuffer[3] == 'X' &&
             applyUsaFixup(indexBuffer, blockSize, vol->bytesPerSector) == 0) {
            const uint8_t *node = indexBuffer + INDX_NODE_HEADER;
            const uint8_t *hardEnd = indexBuffer + blockSize;
            if (scanIndexNode(node, hardEnd, &dir->entryOffset, name, nameCap, info)) return 1;
         }
         dir->indexVcn++; dir->entryOffset = 0;
      }
      return 0;   // all blocks consumed
   }
}

void closeNtfsDir(NtfsDir *dir) { (void)dir; }

// Resolves an in-volume path ("/", "/a", "/a/b/c") to its entry metadata by walking it component
// by component from the root, matching each name case-insensitively (ASCII) against the parent's
// directory index. Iterative and bounded (no recursion). Returns 0 with *info filled, -1 if any
// component is missing or a non-directory is descended into. This lookup scan is ASCII case-fold only
// (a non-ASCII name differing only by case may not match - a benign miss, never corruption); the
// write-path B-tree collation in compareFileNameKeys does full $UpCase folding so inserts stay ordered.
static int resolvePath(const NtfsVolume *vol, const char *path, NtfsInfo *info)
{
   memSet(info, 0, (int)sizeof(*info));
   info->isDir = 1;
   info->mftReference = MFT_RECORD_ROOT;

   const char *cursor = path;
   while (*cursor) {
      while (*cursor == '/') cursor++;                  // skip separators
      if (!*cursor) break;
      char component[256];
      int length = 0;
      while (*cursor && *cursor != '/' && length < 255) component[length++] = *cursor++;
      component[length] = '\0';
      if (!info->isDir) return -1;                      // a path component under a non-directory

      // scan the current directory for a case-insensitive name match
      NtfsDir dir;
      openNtfsDir(&dir, vol, info->mftReference);
      char name[256];
      NtfsInfo entry;
      int found = 0;
      while (readNtfsDir(&dir, name, (int)sizeof(name), &entry) == 1) {
         if (strCmpICase(name, component) == 0) { *info = entry; found = 1; break; }
      }
      if (dir.ioError || !found) return -1;
   }
   return 0;
}

void seekNtfs(NtfsFile *file, uint64_t position);   // defined later; used by readNtfsSecurityDescriptor below

int statNtfs(const NtfsVolume *vol, const char *path, NtfsInfo *info)
{
   if (resolvePath(vol, path, info) != 0) return -1;
   // Read the target record once to refine `info` from the record's own attributes (resolvePath
   // only has the $FILE_NAME / $I30 index copy, which can be stale):
   //  - $STANDARD_INFORMATION @0x20 holds the AUTHORITATIVE DOS attribute flags. Windows updates SI
   //    on SetFileAttributes but does not eagerly push the change into the index key, so SI wins for
   //    read-only/hidden/system reporting. (Also fixes the synthetic root entry, which has no FN key.)
   //  - W12b: a reparse point's tag (first 4 bytes of $REPARSE_POINT) distinguishes symlink/junction/etc.
   // Best-effort: on any read/parse miss, info keeps the index-copy attributes and reparseTag stays 0.
   if (readMftRecordByRef(vol, info->mftReference, mftRecord) == 0) {
      NtfsAttr si;
      if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_STANDARD_INFORMATION, 0, 0, &si) == 1 &&
          si.resident && si.valueLength >= 0x24)
         info->attributes = readLe32(si.value + 0x20);   // canonical FILE_ATTRIBUTE_* flags
      if (info->isReparse) {
         NtfsAttr rp;
         if (findAttribute(mftRecord, vol->mftRecordSize, ATTR_REPARSE_POINT, 0, 0, &rp) == 1) {
            if (rp.resident && rp.valueLength >= 4) {
               info->reparseTag = readLe32(rp.value);
            } else if (!rp.resident) {                      // non-resident $REPARSE_POINT: read the 4-byte tag
               uint8_t tagbuf[8];
               if (readNonResident(vol, &rp, rp.validSize, 0, tagbuf, 4) == 4)
                  info->reparseTag = readLe32(tagbuf);
            }
         }
      }
   }

   // The index $FILE_NAME size copy lags the live $DATA (it is only resynced at close, syncFileNameSizes),
   // so a written-but-not-yet-resynced file lists with a stale/small size. Report the authoritative current
   // $DATA size instead. Directories have no $DATA -> keep the index value. Clobbers mftRecord.
   if (!info->isDir) {
      NtfsAttr data;
      uint64_t housingRef;
      if (findDataAnywhere((NtfsVolume *)vol, info->mftReference & MFT_REF_MASK, mftRecord, &data, &housingRef) == 0) {
         info->size      = data.resident ? data.valueLength : data.realSize;
         info->validSize = info->size;
      }
   }
   return 0;
}

int openNtfs(NtfsFile *file, NtfsVolume *vol, const char *path)
{
   // W12a: an optional "file:stream" suffix in the leaf selects a named $DATA stream. ':' is illegal in
   // NTFS names, so the first ':' after the last '/' is unambiguously the stream separator. A trailing
   // ":$DATA" attribute-type suffix is stripped; "file::$DATA" (empty stream) means the unnamed stream.
   char buf[512];
   int n = 0; while (path[n] && n < (int)sizeof(buf) - 1) { buf[n] = path[n]; n++; }
   buf[n] = '\0';
   int lastSlash = -1; for (int i = 0; i < n; i++) if (buf[i] == '/') lastSlash = i;
   int colon = -1; for (int i = lastSlash + 1; i < n; i++) if (buf[i] == ':') { colon = i; break; }

   uint16_t streamName[32]; uint8_t streamLen = 0;
   if (colon >= 0) {
      buf[colon] = '\0';                                  // terminate the file path at the stream separator
      const char *s = buf + colon + 1;
      int sl = 0; while (s[sl]) sl++;
      if (sl >= 6 && strCmpICase(s + sl - 6, ":$DATA") == 0) sl -= 6;   // strip the ":$DATA" type suffix
      if (sl > 32) return -1;                             // stream name too long for our bounded buffer
      for (int i = 0; i < sl; i++) streamName[i] = (uint8_t)s[i];        // ASCII -> UTF-16 (ADS names are ASCII)
      streamLen = (uint8_t)sl;
   }

   NtfsInfo info;
   if (resolvePath(vol, buf, &info) != 0) return -1;
   if (info.isReparse) return -1;                         // W12b: a reparse point is not an ordinary file
   if (info.isDir) return -1;                             // can't open a directory as a file
   return openStreamByRef(file, vol, info.mftReference, streamLen ? streamName : 0, streamLen);
}

int readNtfs(NtfsFile *file, void *buffer, int length)
{
   if (!file->vol || !file->vol->mounted) return -1;
   if (length <= 0) return 0;
   if (file->position >= file->size) return 0;  // at/after EOF

   uint64_t want = file->size - file->position;
   if (want > (uint64_t)length) want = (uint64_t)length;

   // re-read this file's record and locate its $DATA stream (the unnamed main stream, or a W12a named
   // stream if this handle was opened on one). The record buffer is shared under the lock.
   if (readMftRecordByRef(file->vol, file->mftReference, mftRecord) != 0) return -1;   // F048
   const uint16_t *sName = file->dataNameLen ? file->dataName : 0;
   uint8_t sNameLen = file->dataNameLen;
   NtfsAttr data;
   int haveData = (findAttribute(mftRecord, file->vol->mftRecordSize, ATTR_DATA, sName, sNameLen, &data) == 1);
   if (!haveData && !file->spanned) return -1;             // base must hold $DATA unless it spilled (W8)

   int64_t done;
   if (file->compUnitClusters > 0) {
      // W10a: $LZNT1-compressed $DATA. readCompressed maps the runlist one compression unit at a time
      // (mapVcnWindow, following $ATTRIBUTE_LIST), so it reads a file of any size/fragmentation in
      // O(unit) memory — the full runlist of a multi-MB compressed file far exceeds NTFS_MAX_FILE_RUNS.
      done = readCompressed(file->vol, mftRecord, file->mftReference & MFT_REF_MASK, sName, sNameLen,
                            file->compUnitClusters, file->validSize, file->position, (uint8_t *)buffer, want);
      if (done < 0) return -1;
   } else if (file->spanned) {
      // W8a: $DATA fragments live in several records — merge the runlist, then read from it.
      NtfsRunEntry r[NTFS_MAX_FILE_RUNS]; int rc; uint64_t rs, vs, as;
      if (gatherRuns(file->vol, mftRecord, file->mftReference & MFT_REF_MASK, ATTR_DATA, sName, sNameLen,
                     r, NTFS_MAX_FILE_RUNS, &rc, &rs, &vs, &as) != 0) return -1;
      done = readRuns(file->vol, r, rc, file->validSize, file->position, (uint8_t *)buffer, want);
      if (done < 0) return -1;
   } else if (data.resident) {
      memCopy((uint8_t *)buffer, data.value + file->position, (int)want);   // pos+want <= valueLength == size
      done = (int64_t)want;
   } else {
      done = readNonResident(file->vol, &data, file->validSize, file->position, (uint8_t *)buffer, want);
      if (done < 0) return -1;
   }
   file->position += (uint64_t)done;
   return (int)done;
}

// Decodes a runlist into runs[] (bounded). Returns 0 with *count set, or -1 (malformed / too many).
static int decodeRuns(const uint8_t *runlist, uint32_t length, NtfsRunEntry *runs, int max, int *count)
{
   NtfsRunlist cursor;
   openRunlist(&cursor, runlist, length);
   *count = 0;
   uint64_t vcn, runCount;
   int64_t lcn;
   int rc;
   while ((rc = nextRun(&cursor, &vcn, &lcn, &runCount)) == 1) {
      if (*count >= max) return -1;
      runs[*count].vcn = vcn; runs[*count].lcn = lcn; runs[*count].count = runCount;
      (*count)++;
   }
   return rc;   // 0 clean, -1 malformed
}

// Reads `count` bytes of the cluster $Bitmap at byte offset `pos` into `out` via the runlist cached at
// mount (vol->bitmapRuns) - so cluster (de)allocation never opens or re-reads $Bitmap's MFT record. A
// sub-sector slice is served by reading its covering sector into bitmapScratch. Returns 0 / -1.
static int readBitmapBytes(NtfsVolume *vol, uint64_t pos, uint8_t *out, uint32_t count)
{
   uint32_t sectorSize = vol->bytesPerSector, bytesPerCluster = vol->bytesPerCluster;
   uint32_t copied = 0;
   while (copied < count) {
      uint64_t at = pos + copied;
      uint64_t contiguous = 0;
      int64_t  lcn = mapRunsSpan(vol->bitmapRuns, vol->bitmapRunCount, at / bytesPerCluster, &contiguous);
      if (lcn < 0) return -1;
      uint32_t inCluster = (uint32_t)(at % bytesPerCluster), offsetInSec = inCluster % sectorSize;
      uint64_t lba = vol->partitionOffset + (uint64_t)lcn * vol->sectorsPerCluster + (inCluster - offsetInSec) / sectorSize;
      uint32_t remaining = count - copied;

      // Whole-sector fast path: take as many contiguous sectors as the run and the request allow in a
      // single read, straight into the caller's buffer. The free-cluster scan moves megabytes through
      // here and on the PS3 every read is a USB round-trip (~1.2ms measured), so reads-per-byte is what
      // decides mount time - one sector at a time cost 17s on a 120GB volume.
      uint64_t runBytes = contiguous * bytesPerCluster - inCluster;
      if (offsetInSec == 0 && remaining >= sectorSize && runBytes >= sectorSize && isDmaAligned(out + copied)) {
         uint32_t take = remaining < runBytes ? remaining : (uint32_t)runBytes;
         take -= take % sectorSize;
         if (readSectors(vol->storageHandle, lba, take / sectorSize, out + copied) != 0) return -1;
         copied += take;
         continue;
      }

      if (readSectors(vol->storageHandle, lba, 1, bitmapScratch) != 0) return -1;
      uint32_t take = sectorSize - offsetInSec;
      if (take > remaining) take = remaining;
      memCopy(out + copied, bitmapScratch + offsetInSec, (int)take);
      copied += take;
   }
   return 0;
}

// Locates a file's unnamed $DATA wherever it lives. Reads the base into `buf`; if $DATA is there,
// returns the base ref. Otherwise follows the base $ATTRIBUTE_LIST to the housing extent, reads it
// into `buf`, and returns the extent ref. Fills *out (pointing into buf) and *housingRef. Returns 0/-1.
static int findDataAnywhere(NtfsVolume *vol, uint64_t baseRef, uint8_t *buf, NtfsAttr *out, uint64_t *housingRef)
{
   uint32_t rs = vol->mftRecordSize;
   if (readMftRecord(vol, baseRef, buf) != 0) return -1;   // F048 ok: baseRef is the file's own record from the open handle (validated at open)
   if (findAttribute(buf, rs, ATTR_DATA, 0, 0, out) == 1) { *housingRef = baseRef; return 0; }
   NtfsAttr listAttr;
   if (findAttribute(buf, rs, ATTR_ATTRIBUTE_LIST, 0, 0, &listAttr) != 1 || !listAttr.resident) return -1;
   const uint8_t *L = listAttr.value; uint32_t Llen = listAttr.valueLength, off = 0;
   for (int g = 0; g < 8192; g++) {
      if (off + AL_MIN_ENTRY > Llen) break;
      uint16_t elen = readLe16(L + off + AL_LENGTH);
      if (elen < AL_MIN_ENTRY || off + elen > Llen) break;
      if (readLe32(L + off + AL_TYPE) == ATTR_DATA && L[off + AL_NAME_LENGTH] == 0 &&
          readLe64(L + off + AL_START_VCN) == 0) {
         uint64_t erefFull = readLe64(L + off + AL_MFT_REF);   // full ref: index + sequence (F048)
         uint64_t eref = erefFull & MFT_REF_MASK;
         uint16_t eid  = readLe16(L + off + AL_ATTR_ID);
         if (eref < NTFS_FIRST_USER_RECORD) return -1;         // a list must not point $DATA at a system record
         if (readMftRecordByRef(vol, erefFull, buf) != 0) return -1;
         if (findAttributeInstance(buf, rs, ATTR_DATA, 0, 0, eid, out) != 1) return -1;
         *housingRef = eref;
         return 0;
      }
      off += elen;
   }
   return -1;
}

void seekNtfs(NtfsFile *file, uint64_t position) { if (file) file->position = position; }

// jf-port: read-only.  Nothing is ever dirty, so closing a file has nothing to write back.
int closeNtfs(NtfsFile *file)
{
   (void)file;
   return 0;
}

// Free (zero) bits per nibble value, so the scan costs one lookup per nibble instead of eight
// shift-and-test steps over hundreds of millions of clusters.
static const uint8_t zeroBitsPerNibble[16] = { 4, 3, 3, 2, 3, 2, 2, 1, 3, 2, 2, 1, 2, 1, 1, 0 };

// Walks $Bitmap through the file engine. Correct but costs two device reads per 512 bytes (readNtfs
// re-reads $Bitmap's MFT record every call), so it is only the fallback for a $Bitmap too fragmented
// to cache a runlist for - a volume that is refused writes anyway.
static int countFreeClustersViaFileEngine(NtfsVolume *vol, uint64_t clusterCount, uint64_t *out)
{
   NtfsFile bitmap;
   if (openFileByRef(&bitmap, vol, MFT_RECORD_BITMAP) != 0) return -1;

   uint64_t freeClusters = 0;
   uint64_t bitIndex = 0;
   uint8_t chunk[512];
   int got = 0;
   while (bitIndex < clusterCount && (got = readNtfs(&bitmap, chunk, (int)sizeof(chunk))) > 0) {
      for (int i = 0; i < got && bitIndex < clusterCount; i++) {
         int bits = (clusterCount - bitIndex) >= 8 ? 8 : (int)(clusterCount - bitIndex);
         for (int bit = 0; bit < bits; bit++)
            if (!((chunk[i] >> bit) & 1)) freeClusters++;
         bitIndex += bits;
      }
   }
   if (got < 0) return -1;
   *out = freeClusters;
   return 0;
}

// Counts the zero (free) bits in $Bitmap's $DATA, exactly clusterCount bits (bit 0 of byte 0 = cluster
// 0, LSB first). Run once at mount to seed vol->freeClusters, (the volume is never
// written, so it stays as counted). Returns 0 with *out set, or -1.
//
// Reads through the cached $Bitmap runlist in fileBounce-sized blocks. Mount blocks on this, and the
// cost grows with the volume, so reads-per-byte is what decides how long a large drive takes to appear.
static int countNtfsFreeClusters(NtfsVolume *vol, uint64_t *out)
{
   uint64_t clusterCount = vol->totalSectors / vol->sectorsPerCluster;
   if (vol->bitmapRunCount == 0) return countFreeClustersViaFileEngine(vol, clusterCount, out);

   uint64_t wholeBytes = clusterCount / 8;
   uint32_t tailBits   = (uint32_t)(clusterCount % 8);
   if (vol->bitmapDataSize && wholeBytes > vol->bitmapDataSize) {   // $Bitmap shorter than the volume it
      wholeBytes = vol->bitmapDataSize;                             // describes: count what exists, no more
      tailBits   = 0;
   }

   // whole bytes, in the largest blocks the bounce buffer allows
   uint64_t freeClusters = 0;
   for (uint64_t at = 0; at < wholeBytes; ) {
      uint32_t want = NTFS_READ_BOUNCE;
      if (wholeBytes - at < want) want = (uint32_t)(wholeBytes - at);
      if (readBitmapBytes(vol, at, fileBounce, want) != 0) return -1;
      for (uint32_t i = 0; i < want; i++)
         freeClusters += zeroBitsPerNibble[fileBounce[i] & 0xF] + zeroBitsPerNibble[fileBounce[i] >> 4];
      at += want;
   }

   // trailing partial byte: bits past clusterCount are padding and must not be counted
   if (tailBits) {
      uint8_t last;
      if (readBitmapBytes(vol, wholeBytes, &last, 1) != 0) return -1;
      for (uint32_t bit = 0; bit < tailBits; bit++)
         if (!((last >> bit) & 1)) freeClusters++;
   }

   *out = freeClusters;
   return 0;
}

// O(1): the bitmap is counted once at mount and the count is kept current on every alloc/free, so the
// file manager can refresh free space on the UI thread without rescanning the whole $Bitmap (parity
// with exFAT). totalBytes is pure geometry.
int getNtfsFree(const NtfsVolume *vol, uint64_t *freeBytes, uint64_t *totalBytes)
{
   uint64_t clusterCount = vol->totalSectors / vol->sectorsPerCluster;
   if (totalBytes) *totalBytes = clusterCount * vol->bytesPerCluster;   // callers may pass NULL (free-space widget)
   if (freeBytes)  *freeBytes  = vol->freeClusters * vol->bytesPerCluster;
   return 0;
}

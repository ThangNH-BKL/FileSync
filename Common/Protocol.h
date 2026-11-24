#pragma once
// Protocol.h — Định nghĩa protocol byte-level cho FileSync.
// Theo ARCHITECT.md §6 + điều chỉnh đã chốt:
//   - Bỏ SYNC_ACCEPT; dùng SYNC_REQUEST_INIT + BLOCK_HASHES + SYNC_REQUEST_BLOCKS.
//   - FILE_EVENT có trường optional newRelPath khi action == Renamed.
//   - Mọi số nguyên little-endian; chuỗi UTF-8 với u16 length prefix (không null-term).
//
// Bảng byte-level của mỗi payload được ghi trong Protocol.cpp (phần marshal).

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace fsyn {

// ============================================================================
// Hằng số protocol
// ============================================================================
constexpr uint32_t kMagic        = 0x4E595346u; // 'FSYN' — F=0x46 S=0x53 Y=0x59 N=0x4E; LE u32 = 0x4E595346
constexpr uint16_t kVersion      = 6;
constexpr uint32_t kMaxPayload   = 1024u * 1024u;   // 1 MB (theo đề)
constexpr uint32_t kHeaderSize   = 20;               // header cố định
constexpr uint32_t kBlockSize    = 64u * 1024u;      // 64 KB block delta
constexpr uint32_t kMaxChunkData = 64u * 1024u;      // dataLen tối đa FILE_CHUNK

// Giới hạn chuỗi (byte, sau UTF-8 encode)
constexpr uint16_t kMaxHostnameLen = 256;
constexpr uint16_t kMaxIpLen       = 64;
constexpr uint16_t kMaxRelPathLen  = 1024;
constexpr uint16_t kMaxTokenLen    = 256;
constexpr uint16_t kMaxErrMsgLen   = 512;

// Timeout/heartbeat theo đề bài
constexpr uint32_t kHeartbeatIntervalMs = 5000;
constexpr uint32_t kHeartbeatTimeoutMs  = 60000;
constexpr uint32_t kConnectTimeoutMs    = 10000;
constexpr uint32_t kHandshakeTimeoutMs  = 10000;

// ============================================================================
// Enum
// ============================================================================
enum class MsgType : uint16_t {
    HELLO               = 1,
    HELLO_ACK           = 2,
    TREE_SNAPSHOT       = 3,
    TREE_END            = 4,
    FILE_EVENT          = 5,
    SYNC_REQUEST_INIT   = 6,   // {relPath, serverSize}
    BLOCK_HASHES        = 7,   // {relPath, blockCount, [(offset,crc32)]}
    SYNC_REQUEST_BLOCKS = 8,   // {relPath, serverSize, truncate, needOffsets[]}
    FILE_CHUNK          = 9,
    FILE_DONE           = 10,
    HEARTBEAT           = 11,
    HEARTBEAT_ACK       = 12,
    ERROR_MSG           = 13,
    RESYNC              = 14,
    FILE_COMMITTED      = 15,   // Server SHA256+atomic replace done; FileDoneMsg payload
    FILE_DEFERRED       = 16,   // Client source unstable/unavailable; ResyncMsg payload
    V5_SIGNATURE_PAGE   = 17,   // Server -> Client: base content fingerprints
    V5_BEGIN            = 18,   // Client -> Server: target SHA and expected base SHA
    V5_COPY             = 19,   // Client -> Server: copy source range on server disk
    V5_CHECK            = 20,   // Client -> Server: check prior staged literal SHA
    V5_NEED             = 21,   // Server -> Client: resend only missing literal
    V5_BYTES            = 22,   // Client -> Server: new literal bytes
    V5_FINISH           = 23,   // Client -> Server: validate complete stage and commit
    V5_UNCHANGED        = 24,   // Client -> Server: source SHA == committed base SHA
};

enum class FileAction : uint8_t {
    Created  = 1,
    Modified = 2,
    Renamed  = 3,
    Deleted  = 4,
};

enum class ErrorCode : uint8_t {
    Auth     = 1,
    Proto    = 2,
    Path     = 3,
    Size     = 4,
    Io       = 5,
    Internal = 6,
};

// ============================================================================
// Frame header — 20 byte, little-endian trên dây
//
//  offset  size  field
//  0       4     magic        = kMagic
//  4       2     version      = kVersion
//  6       2     type         = MsgType
//  8       4     payloadLen   0..kMaxPayload
//  12      4     seq          tăng dần mỗi frame gửi đi
//  16      4     reserved     bắt buộc 0; nếu != 0 → ERROR_MSG(Proto) + disconnect
// ============================================================================
#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t payloadLen;
    uint32_t seq;
    uint32_t reserved;
};
#pragma pack(pop)
static_assert(sizeof(FrameHeader) == kHeaderSize, "FrameHeader phải đúng 20 byte");

// ============================================================================
// Cấu trúc logic payload
// ============================================================================
struct HelloMsg {
    std::string hostname;
    std::string ip;
    std::string watchRoot;
    std::string token;
    std::string exeFolder; // folder containing FileClient.exe (protocol v2)
};

struct HelloAckMsg {
    uint8_t ok;           // 1 = OK
    int64_t serverTimeMs; // unix ms
};

struct TreeEntry {
    std::string relPath;
    uint64_t    size;
    int64_t     mtimeNs;  // unix ns
    std::string instanceId; // 40-char file identity, stable across ordinary edits
};

struct FileEventMsg {
    FileAction  action;
    std::string relPath;
    uint64_t    size;
    int64_t     mtimeNs;
    std::string newRelPath; // chỉ khi action == Renamed
    std::string instanceId; // 40-char file identity for current target
};

struct SyncRequestInitMsg {
    std::string relPath;
    uint64_t    serverSize;
};

struct BlockHashEntry {
    uint64_t offset;
    uint32_t crc32;
    uint32_t length;
};

struct BlockHashesMsg {
    std::string relPath;
    std::vector<BlockHashEntry> blocks;
};

struct SyncRequestBlocksMsg {
    std::string relPath;
    uint64_t    serverSize;
    uint8_t     truncate;              // 1 = SetEndOfFile sau khi ghi
    std::vector<uint64_t> needOffsets; // danh sách offset cần client gửi
};

struct FileChunkMsg {
    std::string relPath;
    uint64_t    offset;
    std::vector<uint8_t> data;         // size <= kMaxChunkData
};

struct FileDoneMsg {
    std::string relPath;
    uint64_t    finalSize;
    uint8_t     sha256[32];            // binary
};

struct ErrorMsg {
    ErrorCode   code;
    std::string message;
};

struct ResyncMsg {
    std::string relPath;
};

// ============================================================================
// Marshal / Unmarshal — cài đặt trong Protocol.cpp
// Trả false nếu payload không hợp lệ (độ dài sai, length vượt giới hạn, ...).
// ============================================================================
bool Marshal(const HelloMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, HelloMsg& out);

bool Marshal(const HelloAckMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, HelloAckMsg& out);

bool Marshal(const std::vector<TreeEntry>&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, std::vector<TreeEntry>& out);

bool Marshal(const FileEventMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, FileEventMsg& out);

bool Marshal(const SyncRequestInitMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, SyncRequestInitMsg& out);

bool Marshal(const BlockHashesMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, BlockHashesMsg& out);

bool Marshal(const SyncRequestBlocksMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, SyncRequestBlocksMsg& out);

bool Marshal(const FileChunkMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, FileChunkMsg& out);

bool Marshal(const FileDoneMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, FileDoneMsg& out);

bool Marshal(const ErrorMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, ErrorMsg& out);

bool Marshal(const ResyncMsg&, std::vector<uint8_t>& out);
bool Unmarshal(const uint8_t* p, uint32_t n, ResyncMsg& out);

} // namespace fsyn

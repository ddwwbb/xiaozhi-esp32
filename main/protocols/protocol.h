#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <cJSON.h>
#include <chrono>
#include <functional>
#include <string>
#include <vector>

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

// ===== WebSocket 二进制音频帧结构（跨端字节级契约）=====
// 字节序：网络字节序（大端）。发送端用 htons/htonl 编码，接收端用 ntohs/ntohl 解码。
// 后端 xiaozhi-dialogue WebSocketBinaryProtocol.java 必须与本结构逐字节对齐。
// 详见 docs/02-固件协议规范总览.md §2.3。
//
// 版本演进：
//   v1 = 裸 Opus payload，无头部（向后兼容最老固件）
//   v2 = 16 字节头，带 timestamp（服务端 AEC 需要，payload_size 为 32 位）
//   v3 = 4 字节紧凑头，默认版本（payload_size 为 16 位，单帧最大 65535）
struct BinaryProtocol2 {
    uint16_t version;
    uint16_t type;          // Message type (0: OPUS, 1: JSON)
    uint32_t reserved;      // Reserved for future use
    uint32_t timestamp;     // Timestamp in milliseconds (used for server-side AEC)
    uint32_t payload_size;  // Payload size in bytes（32 位，v2 独有）
    uint8_t payload[];      // Payload data
} __attribute__((packed));

struct BinaryProtocol3 {
    uint8_t type;           // 0: OPUS（v3 仅支持 Opus）
    uint8_t reserved;       // Reserved
    uint16_t payload_size;  // Payload size in bytes（16 位，故单帧 ≤ 65535）
    uint8_t payload[];
} __attribute__((packed));

enum AbortReason { kAbortReasonNone, kAbortReasonWakeWordDetected };

// 监听模式：决定 VAD 静音后是否自动结束本轮、是否支持实时全双工。
//   AutoStop    —— 检测到静音自动结束（默认，无需 AEC）
//   ManualStop  —— 用户显式停止才结束（按钮长按场景）
//   Realtime    —— 实时全双工，边说边听，必须 AEC 支持否则自激
enum ListeningMode {
    kListeningModeAutoStop,
    kListeningModeManualStop,
    kListeningModeRealtime  // 需要 AEC 支持
};

class Protocol {
public:
    virtual ~Protocol() = default;

    inline int server_sample_rate() const { return server_sample_rate_; }
    inline int server_frame_duration() const { return server_frame_duration_; }
    inline const std::string& session_id() const { return session_id_; }

    void OnIncomingAudio(std::function<void(std::unique_ptr<AudioStreamPacket> packet)> callback);
    void OnIncomingJson(std::function<void(const cJSON* root)> callback);
    void OnAudioChannelOpened(std::function<void()> callback);
    void OnAudioChannelClosed(std::function<void()> callback);
    void OnNetworkError(std::function<void(const std::string& message)> callback);
    void OnConnected(std::function<void()> callback);
    void OnDisconnected(std::function<void()> callback);

    virtual bool Start() = 0;
    virtual bool OpenAudioChannel() = 0;
    virtual void CloseAudioChannel(bool send_goodbye = true) = 0;
    virtual bool IsAudioChannelOpened() const = 0;
    virtual bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) = 0;
    virtual void SendWakeWordDetected(const std::string& wake_word);
    virtual void SendStartListening(ListeningMode mode);
    virtual void SendStopListening();
    virtual void SendAbortSpeaking(AbortReason reason);
    virtual void SendMcpMessage(const std::string& message);

protected:
    std::function<void(const cJSON* root)> on_incoming_json_;
    std::function<void(std::unique_ptr<AudioStreamPacket> packet)> on_incoming_audio_;
    std::function<void()> on_audio_channel_opened_;
    std::function<void()> on_audio_channel_closed_;
    std::function<void(const std::string& message)> on_network_error_;
    std::function<void()> on_connected_;
    std::function<void()> on_disconnected_;

    int server_sample_rate_ = 24000;
    int server_frame_duration_ = 60;
    bool error_occurred_ = false;
    std::string session_id_;
    std::chrono::time_point<std::chrono::steady_clock> last_incoming_time_;

    virtual bool SendText(const std::string& text) = 0;
    virtual void SetError(const std::string& message);
    virtual bool IsTimeout() const;
    static void AddTextFontCapabilities(cJSON* root);
};

#endif  // PROTOCOL_H

#pragma once

namespace esphome {
namespace remote_webview {
namespace cfg {

// ── Task config ──────────────────────────────────────────────────────────────
inline constexpr int    decode_task_stack       = 48 * 1024;  // tăng 32→48KB: Chrome H.264 frame lớn hơn Chromium
inline constexpr int    decode_task_prio        = 6;
inline constexpr int    ws_task_stack           = 12 * 1024;
inline constexpr int    ws_task_prio            = 5;
inline constexpr int    decode_queue_depth      = 3;          // giảm 4→3: mỗi slot 32KB, tiết kiệm PSRAM

// ── WebSocket ────────────────────────────────────────────────────────────────
inline constexpr size_t ws_max_message_bytes    = 32 * 1024;  // giữ nguyên 32KB
inline constexpr size_t ws_buffer_size          = 32 * 1024;  // giữ nguyên, khớp với max_message
inline constexpr size_t ws_keepalive_interval_us = 30 * 1000 * 1000;  // giữ 30s

// ── Touch ────────────────────────────────────────────────────────────────────
inline constexpr bool     coalesce_moves        = true;
inline constexpr uint32_t move_rate_hz          = 30;         // giảm 60→30Hz: giảm WS traffic khi kéo scroll

} // namespace cfg
} // namespace remote_webview
} // namespace esphome
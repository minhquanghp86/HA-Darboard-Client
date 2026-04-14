#pragma once
#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/touchscreen/touchscreen.h"
#include "protocol.h"
#include "remote_webview_config.h"
#include <atomic>

#include "esp_event.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#if __has_include("esp_jpeg_dec.h")
  #include "esp_jpeg_dec.h"
  #define REMOTE_WEBVIEW_HAS_ESP_JPEG 1
#else
  #error "esp_new_jpeg component is required. Add to your ESPHome config: framework.components: [{name: 'espressif/esp_new_jpeg'}]"
  #define REMOTE_WEBVIEW_HAS_ESP_JPEG 0
#endif

namespace esphome {
namespace remote_webview {

class RemoteWebView : public Component {
 public:
  // ── Destructor ──────────────────────────────────────────────────────────────
  ~RemoteWebView() {
    if (jpeg_decode_buffer_) {
      free(jpeg_decode_buffer_);
      jpeg_decode_buffer_ = nullptr;
    }
    if (jpeg_dec_) {
      jpeg_dec_close(jpeg_dec_);
      jpeg_dec_ = nullptr;
    }
  }

  // ── Setters (từ __init__.py) ─────────────────────────────────────────────
  void set_display(display::Display *d)             { display_ = d; }
  void set_touchscreen(touchscreen::Touchscreen *t) { touch_ = t; }
  void set_device_id(const std::string &s)          { device_id_ = s; }
  void set_url(const std::string &s)                { url_ = s; }
  void set_server(const std::string &s);
  void set_tile_size(int v)                         { tile_size_ = v; }
  void set_full_frame_tile_count(int v)             { full_frame_tile_count_ = v; }
  void set_full_frame_area_threshold(float v)       { full_frame_area_threshold_ = v; }
  void set_full_frame_every(int v)                  { full_frame_every_ = v; }
  void set_every_nth_frame(int v)                   { every_nth_frame_ = v; }
  void set_min_frame_interval(int v)                { min_frame_interval_ = v; }
  void set_jpeg_quality(int v)                      { jpeg_quality_ = v; }
  void set_max_bytes_per_msg(int v)                 { max_bytes_per_msg_ = v; }
  void set_big_endian(bool v)                       { rgb565_big_endian_ = v; }
  void set_rotation(int v)                          { rotation_ = v; }
  // Có xóa màn hình về màu đen khi pause() không (mặc định: true)
  void set_clear_on_pause(bool v)                   { clear_on_pause_ = v; }

  // ── Getters / Actions ────────────────────────────────────────────────────
  const std::string &get_url() const                { return url_; }
  void disable_touch(bool disable);
  bool open_url(const std::string &s);
  void pause();
  void resume();

  bool     is_receiving_frames(uint32_t timeout_ms = 3000) const;
  uint64_t get_last_frame_age_ms() const;

  // WebSocket helpers (dùng từ HA automation / lambda)
  bool is_ws_connected() const {
    return ws_client_ && esp_websocket_client_is_connected(ws_client_);
  }
  void send_ws_text(const std::string &text) {
    if (!is_ws_connected()) return;
    esp_websocket_client_send_text(ws_client_, text.c_str(), (int)text.length(), portMAX_DELAY);
  }

  // ── ESPHome Component API ────────────────────────────────────────────────
  void setup() override;
  void reconnect_ws();
  void loop() override {}
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 private:
  // ── Nội bộ ──────────────────────────────────────────────────────────────
  struct WsMsg {
    uint8_t *buf{nullptr};
    size_t   len{0};
    void    *client{nullptr};
  };
  struct WsReasm {
    uint8_t *buf{nullptr};
    size_t   total{0}, filled{0};
  };

  static constexpr bool     kCoalesceMoves  = cfg::coalesce_moves;
  static constexpr uint32_t kMoveRateHz     = cfg::move_rate_hz;
  static constexpr uint32_t kMoveIntervalUs = (kMoveRateHz ? (1000000u / kMoveRateHz) : 0);

  static RemoteWebView *self_;

  display::Display          *display_{nullptr};
  touchscreen::Touchscreen  *touch_{nullptr};
  class RemoteWebViewTouchListener *touch_listener_{nullptr};

  int         display_width_{0};
  int         display_height_{0};
  std::string url_;
  std::string server_host_;
  std::string device_id_;
  int         server_port_{0};

  // Tham số cấu hình (−1 = chưa set → server dùng default)
  int   tile_size_{-1};
  int   full_frame_tile_count_{-1};
  float full_frame_area_threshold_{-1.0f};
  int   full_frame_every_{-1};
  int   every_nth_frame_{-1};
  int   min_frame_interval_{-1};
  int   jpeg_quality_{-1};
  int   max_bytes_per_msg_{-1};
  bool  rgb565_big_endian_{true};
  int   rotation_{0};
  bool  touch_disabled_{false};
  bool  clear_on_pause_{true};          // ← xóa màn hình khi pause
  std::atomic<bool> paused_{false};
  std::atomic<bool> url_pending_{false};  // ← thêm dòng này

  // JPEG decoder
  jpeg_dec_handle_t  jpeg_dec_{nullptr};
  uint8_t           *jpeg_decode_buffer_{nullptr};
  size_t             jpeg_decode_buffer_size_{0};

  // Timing
  uint64_t last_move_us_{0};
  uint64_t last_keepalive_us_{0};
  uint64_t last_frame_us_{0};

  // Frame state (chỉ truy cập từ decode task)
  uint64_t frame_start_us_{0};
  uint32_t frame_id_{0xffffffffu};
  uint16_t frame_tiles_{0};
  size_t   frame_bytes_{0};
  uint32_t frame_stats_time_{0};
  uint32_t frame_stats_count_{0};
  size_t   frame_stats_bytes_{0};

  // FreeRTOS handles
  QueueHandle_t     q_decode_{nullptr};
  SemaphoreHandle_t ws_send_mtx_{nullptr};
  SemaphoreHandle_t display_mtx_{nullptr};  // ← bảo vệ draw_pixels_at từ decode task (Core 1)
  TaskHandle_t      t_ws_{nullptr};
  TaskHandle_t      t_decode_{nullptr};

  esp_websocket_client_handle_t ws_client_{nullptr};

  // Task / event handler
  void start_ws_task_();
  void start_decode_task_();
  static void ws_task_tramp_(void *arg);
  static void decode_task_tramp_(void *arg);
  static void ws_event_handler_(void *handler_arg, esp_event_base_t base, int32_t event_id, void *event_data);
  static void reasm_reset_(WsReasm &r);

  // Packet processing
  void process_packet_(void *client, const uint8_t *data, size_t len);
  void process_frame_packet_(const uint8_t *data, size_t len);
  void process_frame_stats_packet_(const uint8_t *data, size_t len);
  bool decode_jpeg_tile_to_lcd_(int16_t dst_x, int16_t dst_y, const uint8_t *data, size_t len);

  // Display helpers
  void clear_display_();   // ← xóa màn hình về đen, thread-safe với display_mtx_

  // WebSocket send helpers
  bool ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid);
  bool ws_send_keepalive_();
  bool ws_send_open_url_(const char *url, uint16_t flags);

  // URI builder
  std::string resolve_device_id_() const;
  std::string build_ws_uri_() const;

  friend class RemoteWebViewTouchListener;
};

// ── Touch listener ──────────────────────────────────────────────────────────
class RemoteWebViewTouchListener : public touchscreen::TouchListener {
 public:
  explicit RemoteWebViewTouchListener(RemoteWebView *p) : parent_(p) {}
  void touch(touchscreen::TouchPoint tp) override;
  void update(const touchscreen::TouchPoints_t &pts) override;
  void release() override;
 private:
  RemoteWebView *parent_{nullptr};
};


template<typename... Ts>
class RemoteWebViewPauseAction : public Action<Ts...> {
 public:
  void set_parent(RemoteWebView *p) { parent_ = p; }
  void play(Ts... x) override { parent_->pause(); }
 private:
  RemoteWebView *parent_{nullptr};
};

template<typename... Ts>
class RemoteWebViewResumeAction : public Action<Ts...> {
 public:
  void set_parent(RemoteWebView *p) { parent_ = p; }
  void play(Ts... x) override { parent_->resume(); }
 private:
  RemoteWebView *parent_{nullptr};
};


} // namespace remote_webview
} // namespace esphome

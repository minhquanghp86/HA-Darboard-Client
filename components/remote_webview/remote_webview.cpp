#include "remote_webview.h"
#include "remote_webview_config.h"
#include "esphome/core/log.h"

#include "esp_idf_version.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_websocket_client.h"
#include "esp_efuse.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace remote_webview {

static const char *const TAG = "Remote_WebView";
RemoteWebView *RemoteWebView::self_ = nullptr;

// ── Helpers ──────────────────────────────────────────────────────────────────

static inline void websocket_force_reconnect(esp_websocket_client_handle_t client) {
  if (!client) return;
  esp_websocket_client_stop(client);
  esp_websocket_client_start(client);
}

// ── Setup / Config ───────────────────────────────────────────────────────────

void RemoteWebView::setup() {
  self_ = this;

  if (!display_) {
    ESP_LOGE(TAG, "no display");
    return;
  }

  display_width_  = display_->get_width();
  display_height_ = display_->get_height();

  q_decode_    = xQueueCreate(cfg::decode_queue_depth, sizeof(WsMsg));
  ws_send_mtx_ = xSemaphoreCreateMutex();
  display_mtx_ = xSemaphoreCreateMutex();  // bảo vệ draw_pixels_at từ decode task

  // Khởi tạo JPEG decoder
  jpeg_dec_config_t jpeg_cfg = {
    .output_type  = rgb565_big_endian_ ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE,
    .scale        = {.width = 0, .height = 0},
    .clipper      = {.width = 0, .height = 0},
    .rotate       = JPEG_ROTATE_0D,
    .block_enable = false,
  };

  jpeg_error_t ret = jpeg_dec_open(&jpeg_cfg, &jpeg_dec_);
  if (ret != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "Failed to open JPEG decoder: %d", ret);
    jpeg_dec_ = nullptr;
  } else {
    jpeg_decode_buffer_size_ = (size_t)display_width_ * (size_t)display_height_ * 2u;

    // Ưu tiên PSRAM (aligned 16 byte cho DMA)
    jpeg_decode_buffer_ = (uint8_t *)heap_caps_aligned_alloc(16, jpeg_decode_buffer_size_,
                                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg_decode_buffer_) {
      jpeg_decode_buffer_ = (uint8_t *)heap_caps_aligned_alloc(16, jpeg_decode_buffer_size_,
                                                                MALLOC_CAP_8BIT);
    }
    if (!jpeg_decode_buffer_) {
      ESP_LOGE(TAG, "Failed to allocate JPEG decode buffer: %u bytes", (unsigned)jpeg_decode_buffer_size_);
      jpeg_dec_close(jpeg_dec_);
      jpeg_dec_ = nullptr;
    } else {
      ESP_LOGD(TAG, "JPEG decoder initialized, buffer: %u bytes", (unsigned)jpeg_decode_buffer_size_);
    }
  }

  start_decode_task_();
  start_ws_task_();

  if (touch_) {
    touch_listener_ = new RemoteWebViewTouchListener(this);
    touch_->register_listener(touch_listener_);
    ESP_LOGD(TAG, "touch listener registered");
  }
}

void RemoteWebView::dump_config() {
  ESP_LOGCONFIG(TAG, "remote_webview:");

  const std::string id = device_id_.empty() ? resolve_device_id_() : device_id_;
  ESP_LOGCONFIG(TAG, "  id: %s", id.c_str());

  if (display_) {
    ESP_LOGCONFIG(TAG, "  display: %dx%d", display_->get_width(), display_->get_height());
  }

  ESP_LOGCONFIG(TAG, "  server: %s:%d", server_host_.c_str(), server_port_);
  ESP_LOGCONFIG(TAG, "  url: %s", url_.c_str());

  auto log_int   = [&](const char *name, int v)   { if (v >= 0)    ESP_LOGCONFIG(TAG, "  %s: %d",    name, v); };
  auto log_float = [&](const char *name, float v) { if (v >= 0.0f) ESP_LOGCONFIG(TAG, "  %s: %.2f", name, (double)v); };

  log_int  ("tile_size",                 tile_size_);
  log_int  ("full_frame_tile_count",     full_frame_tile_count_);
  log_float("full_frame_area_threshold", full_frame_area_threshold_);
  log_int  ("full_frame_every",          full_frame_every_);
  log_int  ("every_nth_frame",           every_nth_frame_);
  log_int  ("min_frame_interval",        min_frame_interval_);
  log_int  ("jpeg_quality",              jpeg_quality_);
  log_int  ("max_bytes_per_msg",         max_bytes_per_msg_);
  log_int  ("big_endian",                (int)rgb565_big_endian_);
  log_int  ("rotation",                  rotation_);
  log_int  ("clear_on_pause",            (int)clear_on_pause_);
}

// ── WebSocket reconnect ──────────────────────────────────────────────────────

void RemoteWebView::reconnect_ws() {
  ESP_LOGI(TAG, "Forcing websocket reconnect...");

  // 1. Dừng và xóa ws task cũ trước (tránh task leak)
  if (t_ws_) {
    vTaskDelete(t_ws_);
    t_ws_ = nullptr;
  }

  // 2. Đóng và destroy ws client
  if (ws_client_) {
    if (esp_websocket_client_is_connected(ws_client_)) {
      ESP_LOGI(TAG, "Closing active websocket...");
      esp_websocket_client_close(ws_client_, pdMS_TO_TICKS(3000));
    }
    ESP_LOGI(TAG, "Destroying websocket client...");
    esp_websocket_client_stop(ws_client_);
    esp_websocket_client_destroy(ws_client_);
    ws_client_ = nullptr;
  }

  // 3. Reset frame state
  frame_id_ = 0xffffffffu;

  // 4. Tạo lại ws task
  ESP_LOGI(TAG, "Starting websocket task again...");
  start_ws_task_();
}

// ── WebSocket task ───────────────────────────────────────────────────────────

void RemoteWebView::start_ws_task_() {
  xTaskCreatePinnedToCore(&RemoteWebView::ws_task_tramp_, "rwv_ws",
                          cfg::ws_task_stack, this, cfg::ws_task_prio, &t_ws_, 0);
}

void RemoteWebView::ws_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView *>(arg);

  std::string uri_str = self->build_ws_uri_();
  esp_websocket_client_config_t cfg_ws = {};
  cfg_ws.uri                    = uri_str.c_str();
  cfg_ws.reconnect_timeout_ms   = 2000;
  cfg_ws.network_timeout_ms     = 10000;
  cfg_ws.task_stack             = cfg::ws_task_stack;
  cfg_ws.task_prio              = cfg::ws_task_prio;
  cfg_ws.buffer_size            = cfg::ws_buffer_size;
  cfg_ws.disable_auto_reconnect = false;

  WsReasm reasm{};
  esp_websocket_client_handle_t client = esp_websocket_client_init(&cfg_ws);
  ESP_ERROR_CHECK(esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, ws_event_handler_, &reasm));
  ESP_ERROR_CHECK(esp_websocket_client_start(client));

  for (;;) {
      vTaskDelay(pdMS_TO_TICKS(2000));

      if (!esp_websocket_client_is_connected(client)) {
          websocket_force_reconnect(client);
          continue;
      }

      if (!self) continue;

      const uint64_t now = esp_timer_get_time();

      // Gửi open_url nếu pending (an toàn vì đây không phải event handler)
      if (self->url_pending_.load(std::memory_order_relaxed) && !self->url_.empty()) {
          if (self->ws_send_open_url_(self->url_.c_str(), 0)) {
              self->url_pending_ = false;
              ESP_LOGI(TAG, "[ws] open_url sent");
          }
          continue;
      }

      // Keepalive
      if (self->ws_client_ && esp_websocket_client_is_connected(self->ws_client_))   {
          if (now - self->last_keepalive_us_ >= cfg::ws_keepalive_interval_us) {
              if (self->ws_send_keepalive_()) {
                  self->last_keepalive_us_ = now;
                  ESP_LOGV(TAG, "[ws] keepalive sent");
              }
          }
      }
   }
}

// ── WebSocket event handler ──────────────────────────────────────────────────

void RemoteWebView::reasm_reset_(WsReasm &r) {
  if (r.buf) free(r.buf);
  r.buf = nullptr; r.total = 0; r.filled = 0;
}

void RemoteWebView::ws_event_handler_(void *handler_arg, esp_event_base_t, int32_t event_id, void *event_data) {
  auto *r = reinterpret_cast<WsReasm *>(handler_arg);
  auto *e = reinterpret_cast<const esp_websocket_event_data_t *>(event_data);

  switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
      if (self_) {
        self_->ws_client_         = e->client;
        self_->last_keepalive_us_ = esp_timer_get_time();
        self_->last_frame_us_     = 0;     // reset để monitoring biết chưa có frame
        self_->url_pending_       = true;  // ← dùng flag thay vì gọi send trực tiếp
      }
      ESP_LOGI(TAG, "[ws] connected");
      break;


    case WEBSOCKET_EVENT_DISCONNECTED:
      if (self_) {
        self_->ws_client_         = nullptr;
        self_->last_keepalive_us_ = 0;
        self_->last_frame_us_     = 0;
      }
      ESP_LOGI(TAG, "[ws] disconnected");
      reasm_reset_(*r);
      websocket_force_reconnect(e->client);
      break;

#ifdef WEBSOCKET_EVENT_CLOSED
    case WEBSOCKET_EVENT_CLOSED:
      if (self_) {
        self_->ws_client_         = nullptr;
        self_->last_keepalive_us_ = 0;
        self_->last_frame_us_     = 0;
      }
      ESP_LOGI(TAG, "[ws] closed");
      reasm_reset_(*r);
      websocket_force_reconnect(e->client);
      break;
#endif

    case WEBSOCKET_EVENT_DATA: {
      if (!self_) break;

      const uint8_t *frag     = (const uint8_t *)e->data_ptr;
      const size_t   frag_len = (size_t)e->data_len;

      if (e->op_code != WS_TRANSPORT_OPCODES_BINARY) break;

      if (e->payload_offset == 0) {
        reasm_reset_(*r);
        const size_t max_allowed = (self_->max_bytes_per_msg_ > 0)
                                   ? (size_t)self_->max_bytes_per_msg_
                                   : cfg::ws_max_message_bytes;
        if ((size_t)e->payload_len > max_allowed) {
          ESP_LOGE(TAG, "WS message too large: %u > %u",
                   (unsigned)e->payload_len, (unsigned)max_allowed);
          reasm_reset_(*r);
          break;
        }
        r->total = (size_t)e->payload_len;
        r->buf   = (uint8_t *)heap_caps_malloc(r->total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!r->buf) r->buf = (uint8_t *)heap_caps_malloc(r->total, MALLOC_CAP_8BIT);
        if (!r->buf) {
          ESP_LOGE(TAG, "malloc %u failed", (unsigned)r->total);
          r->total = 0;
          break;
        }
      }

      if (!r->buf || r->total == 0) break;

      if ((size_t)e->payload_offset + frag_len > r->total) {
        ESP_LOGE(TAG, "bad fragment bounds");
        reasm_reset_(*r);
        break;
      }

      memcpy(r->buf + e->payload_offset, frag, frag_len);
      const size_t new_filled = (size_t)e->payload_offset + frag_len;
      if (new_filled > r->filled) r->filled = new_filled;

      if (r->filled == r->total) {
        WsMsg m{r->buf, r->total, e->client};
        r->buf = nullptr; r->total = 0; r->filled = 0;
        if (!self_->q_decode_ || xQueueSend(self_->q_decode_, &m, 0) != pdTRUE) {
          ESP_LOGW(TAG, "decode queue full, dropping packet");
          free(m.buf);
        }
      }
      break;
    }

    case WEBSOCKET_EVENT_ERROR:
      ESP_LOGE(TAG, "[ws] error: type=%d tls_err=%d tls_stack=%d",
               e->error_handle.error_type,
               e->error_handle.esp_tls_last_esp_err,
               e->error_handle.esp_tls_stack_err);
      break;

    default: break;
  }
}

// ── Decode task ──────────────────────────────────────────────────────────────

void RemoteWebView::start_decode_task_() {
  xTaskCreatePinnedToCore(&RemoteWebView::decode_task_tramp_, "rwv_decode",
                          cfg::decode_task_stack, this, cfg::decode_task_prio, &t_decode_, 1);
}

void RemoteWebView::decode_task_tramp_(void *arg) {
  auto *self = reinterpret_cast<RemoteWebView *>(arg);
  WsMsg m;
  for (;;) {
    if (xQueueReceive(self->q_decode_, &m, portMAX_DELAY) == pdTRUE) {
      if (!self->paused_.load(std::memory_order_relaxed))
        self->process_packet_(m.client, m.buf, m.len);
      free(m.buf);
    }
  }
}

// ── Packet processing ────────────────────────────────────────────────────────

void RemoteWebView::process_packet_(void * /*client*/, const uint8_t *data, size_t len) {
  if (!data || len == 0) return;

  switch ((proto::MsgType)data[0]) {
    case proto::MsgType::Frame:      process_frame_packet_(data, len);       break;
    case proto::MsgType::FrameStats: process_frame_stats_packet_(data, len); break;
    default:
      ESP_LOGW(TAG, "unknown packet type: %d", (int)data[0]);
      break;
  }
}

void RemoteWebView::process_frame_packet_(const uint8_t *data, size_t len) {
  if (!data || len < sizeof(proto::FrameHeader)) return;

  proto::FrameInfo fi{};
  size_t off = 0;
  if (!proto::parse_frame_header(data, len, fi, off)) return;

  if (fi.frame_id != frame_id_) {
    frame_id_        = fi.frame_id;
    frame_tiles_     = 0;
    frame_bytes_     = 0;
    frame_start_us_  = esp_timer_get_time();
  }
  frame_bytes_ += len;
  frame_tiles_ += fi.tile_count;

  for (uint16_t i = 0; i < fi.tile_count; i++) {
    // ── FIX: thoát sớm nếu bị pause giữa chừng — tránh ghost tile ──────────
    if (paused_.load(std::memory_order_relaxed)) return;

    proto::TileHeader th{};
    if (!proto::parse_tile_header(data, len, th, off)) return;
    if (off + th.dlen > len) return;

    if (th.w > 0 && th.h > 0 && th.w <= (uint16_t)display_width_ && th.h <= (uint16_t)display_height_) {
      if (fi.enc == proto::Encoding::JPEG && th.dlen) {
        decode_jpeg_tile_to_lcd_((int16_t)th.x, (int16_t)th.y, data + off, th.dlen);
      }
    }
    off += th.dlen;
  }

  if (fi.flags & proto::kFlafLastOfFrame) {
    last_frame_us_ = esp_timer_get_time();

    const uint32_t time_ms = (uint32_t)((last_frame_us_ - frame_start_us_) / 1000ULL);
    frame_stats_bytes_ += frame_bytes_;
    frame_stats_time_  += time_ms;
    frame_stats_count_++;
    ESP_LOGD(TAG, "frame %lu: tiles %u (%u bytes) - %lu ms",
             (unsigned long)frame_id_, frame_tiles_, (unsigned)frame_bytes_, (unsigned long)time_ms);
  }
}

void RemoteWebView::process_frame_stats_packet_(const uint8_t *data, size_t len) {
  const uint32_t avg_render_time = (frame_stats_count_ > 0)
                                   ? (frame_stats_time_ / frame_stats_count_)
                                   : 0u;

  ESP_LOGD(TAG, "sending frame stats: avg_time=%u ms, bytes=%u",
           (unsigned)avg_render_time, (unsigned)frame_stats_bytes_);

  uint8_t pkt[sizeof(proto::FrameStatsPacket)];
  const size_t n = proto::build_frame_stats_packet(avg_render_time, (uint32_t)frame_stats_bytes_, pkt);

  frame_stats_time_  = 0;
  frame_stats_count_ = 0;
  frame_stats_bytes_ = 0;

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, to) != pdTRUE) return;
  esp_websocket_client_send_bin(ws_client_, (const char *)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
}

// ── JPEG decode → LCD ────────────────────────────────────────────────────────

bool RemoteWebView::decode_jpeg_tile_to_lcd_(int16_t dst_x, int16_t dst_y,
                                              const uint8_t *data, size_t len) {
  if (!data || !len || !jpeg_dec_ || !jpeg_decode_buffer_) return false;

  jpeg_dec_io_t io = {
    .inbuf        = (uint8_t *)data,
    .inbuf_len    = (int)len,
    .inbuf_remain = (int)len,
    .outbuf       = jpeg_decode_buffer_,
    .out_size     = 0,
  };

  jpeg_dec_header_info_t hdr;
  if (jpeg_dec_parse_header(jpeg_dec_, &io, &hdr) != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "jpeg_dec_parse_header failed");
    return false;
  }

  const size_t required = (size_t)hdr.width * (size_t)hdr.height * 2u;
  if (required > jpeg_decode_buffer_size_) {
    ESP_LOGE(TAG, "Tile too large: %u > %u bytes",
             (unsigned)required, (unsigned)jpeg_decode_buffer_size_);
    return false;
  }

  if (jpeg_dec_process(jpeg_dec_, &io) != JPEG_ERR_OK) {
    ESP_LOGE(TAG, "jpeg_dec_process failed");
    return false;
  }

  // Bảo vệ draw_pixels_at: decode task chạy trên Core 1,
  // display driver có thể không thread-safe với Core 0 main loop.
  if (display_mtx_ && xSemaphoreTake(display_mtx_, pdMS_TO_TICKS(100)) == pdTRUE) {
    display_->draw_pixels_at(dst_x, dst_y, (int)hdr.width, (int)hdr.height,
                             jpeg_decode_buffer_,
                             esphome::display::COLOR_ORDER_RGB,
                             esphome::display::COLOR_BITNESS_565,
                             rgb565_big_endian_);
    xSemaphoreGive(display_mtx_);
  } else {
    ESP_LOGW(TAG, "display_mtx_ timeout, skipping tile draw");
    return false;
  }

  return true;
}

// ── Display clear helper ─────────────────────────────────────────────────────

void RemoteWebView::clear_display_() {
  // Không dùng jpeg_decode_buffer_ để tránh race với decode task.
  // display_->fill() là API độc lập, an toàn để gọi sau khi lấy display_mtx_.
  if (!display_ || !display_mtx_) return;

  if (xSemaphoreTake(display_mtx_, pdMS_TO_TICKS(500)) == pdTRUE) {
    display_->fill(esphome::Color::BLACK);
    xSemaphoreGive(display_mtx_);
    ESP_LOGD(TAG, "display cleared");
  } else {
    ESP_LOGW(TAG, "clear_display_: display_mtx_ timeout");
  }
}

// ── WebSocket send helpers ───────────────────────────────────────────────────

bool RemoteWebView::ws_send_touch_event_(proto::TouchType type, int x, int y, uint8_t pid) {
  if (touch_disabled_) return false;
  if (!ws_client_ || !ws_send_mtx_ || !esp_websocket_client_is_connected(ws_client_)) return false;

  // Clamp
  if (x < 0) x = 0; else if (x > 65535) x = 65535;
  if (y < 0) y = 0; else if (y > 65535) y = 65535;

  uint8_t pkt[sizeof(proto::TouchPacket)];
  const size_t n = proto::build_touch_packet(type, pid, (uint16_t)x, (uint16_t)y, pkt);

  if (xSemaphoreTake(ws_send_mtx_, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  const int r = esp_websocket_client_send_bin(ws_client_, (const char *)pkt, (int)n, pdMS_TO_TICKS(50));
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)n;
}

bool RemoteWebView::ws_send_open_url_(const char *url, uint16_t flags) {
  if (!ws_client_ || !ws_send_mtx_ || !url || !esp_websocket_client_is_connected(ws_client_)) return false;

  const size_t url_len = strlen(url);
  const size_t total   = sizeof(proto::OpenURLHeader) + url_len;
  if (total > 16 * 1024) return false;

  auto *pkt = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pkt) pkt = (uint8_t *)heap_caps_malloc(total, MALLOC_CAP_8BIT);
  if (!pkt) return false;

  bool ok = false;
  const size_t written = proto::build_open_url_packet(url, flags, pkt, total);
  if (written && xSemaphoreTake(ws_send_mtx_, pdMS_TO_TICKS(50)) == pdTRUE) {
    const int r = esp_websocket_client_send_bin(ws_client_, (const char *)pkt, (int)written, pdMS_TO_TICKS(200));
    xSemaphoreGive(ws_send_mtx_);
    ok = (r == (int)written);
  }
  free(pkt);
  return ok;
}

bool RemoteWebView::ws_send_keepalive_() {
  if (!ws_client_ || !ws_send_mtx_ || !esp_websocket_client_is_connected(ws_client_)) return false;

  uint8_t pkt[sizeof(proto::KeepalivePacket)];
  const size_t n = proto::build_keepalive_packet(pkt);
  if (!n) return false;

  const TickType_t to = pdMS_TO_TICKS(50);
  if (xSemaphoreTake(ws_send_mtx_, to) != pdTRUE) return false;
  const int r = esp_websocket_client_send_bin(ws_client_, (const char *)pkt, (int)n, to);
  xSemaphoreGive(ws_send_mtx_);
  return r == (int)n;
}

// ── open_url (public API) ────────────────────────────────────────────────────

bool RemoteWebView::open_url(const std::string &s) {
  if (s.empty()) return false;
  if (!ws_client_ || !esp_websocket_client_is_connected(ws_client_)) return false;

  if (ws_send_open_url_(s.c_str(), 0)) {
    url_ = s;
    ESP_LOGD(TAG, "opened URL: %s", s.c_str());
    return true;
  }
  return false;
}

void RemoteWebView::disable_touch(bool disable) {
  touch_disabled_ = disable;
  ESP_LOGD(TAG, "touch %s", disable ? "disabled" : "enabled");
}

// ── Touch listener ───────────────────────────────────────────────────────────

void RemoteWebViewTouchListener::update(const touchscreen::TouchPoints_t &pts) {
  if (!parent_) return;

  const uint64_t now = esp_timer_get_time();
  for (auto &p : pts) {
    switch (p.state) {
      case touchscreen::STATE_PRESSED:
        parent_->ws_send_touch_event_(proto::TouchType::Down, p.x, p.y, p.id);
        break;
      case touchscreen::STATE_UPDATED:
        if (!RemoteWebView::kCoalesceMoves || RemoteWebView::kMoveIntervalUs == 0 ||
            (now - parent_->last_move_us_) >= RemoteWebView::kMoveIntervalUs) {
          parent_->last_move_us_ = now;
          parent_->ws_send_touch_event_(proto::TouchType::Move, p.x, p.y, p.id);
        }
        break;
      case touchscreen::STATE_RELEASING:
      case touchscreen::STATE_RELEASED:
        parent_->ws_send_touch_event_(proto::TouchType::Up, p.x, p.y, p.id);
        break;
      default: break;
    }
  }
}

void RemoteWebViewTouchListener::release() {
  if (parent_) parent_->ws_send_touch_event_(proto::TouchType::Up, 0, 0, 0);
}

void RemoteWebViewTouchListener::touch(touchscreen::TouchPoint tp) {
  if (parent_) parent_->ws_send_touch_event_(proto::TouchType::Down, tp.x, tp.y, tp.id);
}

// ── Server / URI ─────────────────────────────────────────────────────────────

void RemoteWebView::set_server(const std::string &s) {
  const auto pos = s.rfind(':');
  if (pos == std::string::npos || pos == s.size() - 1) {
    ESP_LOGE(TAG, "server must be host:port, got: %s", s.c_str());
    return;
  }
  server_host_ = s.substr(0, pos);
  server_port_ = atoi(s.c_str() + pos + 1);
  if (server_port_ <= 0 || server_port_ > 65535) {
    ESP_LOGE(TAG, "invalid port in server: %s", s.c_str());
    server_host_.clear();
    server_port_ = 0;
  }
}

std::string RemoteWebView::resolve_device_id_() const {
  if (!device_id_.empty()) return device_id_;

  uint8_t mac[6] = {0};
  esp_err_t err = ESP_FAIL;

#if ESP_IDF_VERSION_MAJOR >= 5
  err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) err = esp_read_mac(mac, ESP_MAC_BT);
  if (err != ESP_OK) err = esp_read_mac(mac, ESP_MAC_ETH);
  if (err != ESP_OK) err = esp_efuse_mac_get_default(mac);
#else
  err = esp_efuse_mac_get_default(mac);
  if (err != ESP_OK) err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
#endif

  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read MAC address, using random ID");
    const uint32_t rnd = esp_random();
    snprintf((char *)mac, sizeof(mac), "%06lx", (unsigned long)rnd);
  }

  char buf[32];
  snprintf(buf, sizeof(buf), "esp32-%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

std::string RemoteWebView::build_ws_uri_() const {
  // Tránh gọi s.find('?') lặp lại nhiều lần — dùng lambda sep() thay thế
  std::string uri = "ws://" + server_host_ + ":" + std::to_string(server_port_) + "/";

  bool first_param = true;
  auto sep = [&]() -> char {
    if (first_param) { first_param = false; return '?'; }
    return '&';
  };

  char buf[48];
  const std::string id = resolve_device_id_();

  auto add_str = [&](const char *k, const char *v) {
    if (!v || !*v) return;
    uri += sep(); uri += k; uri += '='; uri += v;
  };
  auto add_int = [&](const char *k, int v) {
    if (v < 0) return;
    snprintf(buf, sizeof(buf), "%s=%d", k, v);
    uri += sep(); uri += buf;
  };
  auto add_float = [&](const char *k, float v) {
    if (v < 0.0f) return;
    snprintf(buf, sizeof(buf), "%s=%.2f", k, (double)v);
    uri += sep(); uri += buf;
  };

  add_str  ("id",   id.c_str());
  add_int  ("w",    display_width_);
  add_int  ("h",    display_height_);
  add_int  ("r",    rotation_);
  add_int  ("ts",   tile_size_);
  add_int  ("fftc", full_frame_tile_count_);
  add_float("ffat", full_frame_area_threshold_);
  add_int  ("ffe",  full_frame_every_);
  add_int  ("enf",  every_nth_frame_);
  add_int  ("mfi",  min_frame_interval_);
  add_int  ("q",    jpeg_quality_);
  add_int  ("mbpm", max_bytes_per_msg_);

  return uri;
}

// ���─ Frame status helpers ─────────────────��───────────────────────────────────

bool RemoteWebView::is_receiving_frames(uint32_t timeout_ms) const {
  if (last_frame_us_ == 0) return false;
  return (esp_timer_get_time() - last_frame_us_) < ((uint64_t)timeout_ms * 1000ULL);
}

uint64_t RemoteWebView::get_last_frame_age_ms() const {
  if (last_frame_us_ == 0) return UINT64_MAX;
  return (esp_timer_get_time() - last_frame_us_) / 1000ULL;
}

// ── Pause / Resume ───────────────────────────────────────────────────────────

void RemoteWebView::pause() {
  // Nếu đã pause rồi thì bỏ qua
  if (paused_.exchange(true, std::memory_order_acq_rel)) return;

  // 1. Drain queue — giải phóng bộ nhớ các frame đang chờ
  if (q_decode_) {
    WsMsg m;
    while (xQueueReceive(q_decode_, &m, 0) == pdTRUE)
      free(m.buf);
  }

  // 2. Xóa màn hình về đen để tránh ghost frame chồng lên component khác.
  //    - paused_=true đã được set ở trên, nên decode task sẽ không draw thêm
  //      các frame MỚI (kiểm tra ở đầu decode_task_tramp_).
  //    - Nếu decode task đang xử lý frame DỞ CHỪNG (đã vượt qua kiểm tra paused_),
  //      nó có thể draw thêm vài tile trước khi kiểm tra paused_ bên trong
  //      process_frame_packet_. Vì vậy clear_display_() lấy display_mtx_,
  //      đảm bảo serialize với mọi draw call còn lại đó.
  //    - Dùng display_->fill() thay vì jpeg_decode_buffer_ để tránh race:
  //      decode task có thể đang ghi vào jpeg_decode_buffer_ ở bước decode JPEG
  //      (trước khi lấy display_mtx_).
  if (clear_on_pause_) {
    clear_display_();
  }

  ESP_LOGD(TAG, "paused");
}

void RemoteWebView::resume() {
  // Nếu chưa pause thì bỏ qua
  if (!paused_.exchange(false, std::memory_order_acq_rel)) return;

  // Gửi lại URL hiện tại → server sẽ gửi full frame ngay
  if (!url_.empty())
    ws_send_open_url_(url_.c_str(), 0);

  ESP_LOGD(TAG, "resumed");
}

} // namespace remote_webview
} // namespace esphome

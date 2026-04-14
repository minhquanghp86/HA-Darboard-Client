# HA-Darboard-Client



![Guition-ESP32-S3-4848S040 running Remote WebView](/images/image-001.jpg)

[Demo video](https://youtu.be/rD2aYUUrv5o)

Đây là một máy khách kết nối tới [Remote WebView Server](https://github.com/minhquanghp86/RemoteWebViewServer) — trình duyệt không có giao diện hiển thị các trang web mục tiêu (ví dụ: bảng điều khiển Home Assistant) và truyền phát chúng dưới dạng các ô hình ảnh qua WebSocket đến các máy khách nhẹ (màn hình ESP32).

## ESPHome component

Phiên bản mới nhất của máy khách được triển khai dưới dạng thành phần bên ngoài ESPHome, giúp đơn giản hóa đáng kể việc cài đặt và cấu hình cho người dùng cuối. Nó tận dụng các thành phần hiển thị và màn hình cảm ứng để hiển thị hình ảnh và xử lý đầu vào cảm ứng.

### Ví dụ (Guition-ESP32-S3-4848S040)

```yaml
esphome:
  name: esp32-4848s040-t1
  friendly_name: ESP32-4848S040-T1
  platformio_options:
    board_build.flash_mode: dio

esp32:
  board: esp32-s3-devkitc-1
  variant: esp32s3
  flash_size: 16MB
  framework:
    type: esp-idf
    sdkconfig_options:
      COMPILER_OPTIMIZATION_SIZE: y
      CONFIG_ESP32S3_DEFAULT_CPU_FREQ_240: "y"
      CONFIG_ESP32S3_DATA_CACHE_64KB: "y"
      CONFIG_ESP32S3_DATA_CACHE_LINE_64B: "y"
      CONFIG_SPIRAM_FETCH_INSTRUCTIONS: y
      CONFIG_SPIRAM_RODATA: y
    components:
      - name: "espressif/esp_websocket_client"
        ref: 1.5.0
      - name: "bitbank2/jpegdec"
        source: https://github.com/strange-v/jpegdec-esphome

psram:
  mode: octal
  speed: 80MHz

external_components:
  - source: github://strange-v/RemoteWebViewClient@main
    refresh: 0s
    components: [ remote_webview ]

logger:
  hardware_uart: UART0

api:
  encryption:
    key: "XXXXXXXXX"

ota:
  - platform: esphome
    password: "XXXXXXXXX"

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

captive_portal:
    
spi:
  clk_pin: GPIO48
  mosi_pin: GPIO47

i2c:
  - id: bus_a
    sda: GPIO19
    scl: GPIO45

display:
  - platform: st7701s
    show_test_card: False
    update_interval: never
    auto_clear_enabled: False
    spi_mode: MODE3
    data_rate: 2MHz
    color_order: RGB
    invert_colors: False
    dimensions:
      width: 480
      height: 480
    cs_pin: 39
    de_pin: 18
    hsync_pin: 16
    vsync_pin: 17
    pclk_pin: 21
    pclk_frequency: 12MHz
    pclk_inverted: False
    hsync_pulse_width: 8
    hsync_front_porch: 10
    hsync_back_porch: 20
    vsync_pulse_width: 8
    vsync_front_porch: 10
    vsync_back_porch: 10
    init_sequence:
      - 1
      - [0xFF, 0x77, 0x01, 0x00, 0x00, 0x10]
      - [0xCD, 0x00]
    data_pins:
      red:
        - GPIO11
        - GPIO12
        - GPIO13
        - GPIO14
        - GPIO0
      green:
        - GPIO8
        - GPIO20
        - GPIO3
        - GPIO46
        - GPIO9
        - GPIO10
      blue:
        - GPIO4
        - GPIO5
        - GPIO6
        - GPIO7
        - GPIO15

touchscreen:
  platform: gt911
  transform:
    mirror_x: false
    mirror_y: false
  i2c_id: bus_a

output:
  - platform: ledc
    pin: GPIO38
    id: backlight_pwm

light:
  - platform: monochromatic
    output: backlight_pwm
    name: "Display Backlight"
    id: back_light
    restore_mode: ALWAYS_ON

remote_webview:
  id: rwv
  server: 172.16.0.252:8081
  url: http://172.16.0.252:8123/dashboard-mobile/0  # set url: "self-test" to initiate the self-test
  full_frame_tile_count: 1
  max_bytes_per_msg: 61440
  jpeg_quality: 85

text:
  - platform: template
    id: rwv_url
    name: "URL"
    optimistic: true
    restore_value: false
    mode: TEXT
    min_length: 1
    set_action:
      - lambda: |-
          if (!id(rwv).open_url(std::string(x.c_str()))) {
            id(rwv).set_url(std::string(x.c_str()));
            ESP_LOGI("remote_webview", "URL queued (not connected): %s", x.c_str());
          }

```

### Thông số được hỗ trợ

| YAML key                | Type      | Required | Example                          | Description |
|-------------------------|-----------|:--------:|----------------------------------|-------------|
| `display_id`            | id        | ❌       | `panel`                           | ID của màn hình để vẽ. Tùy chọn, nếu chỉ có một màn hình được định nghĩa trong YAML|
| `touchscreen_id`        | id        | ❌       | `touch`                           | Nguồn đầu vào cảm ứng. Tùy chọn, nếu chỉ có một màn hình cảm ứng được định nghĩa trong YAML.|
| `server`                | string    | ✅       | `172.16.0.252:8081`              | địa chỉ máy chủ websocket. phải là `hostname_or_ip:port`. |
| `url`                   | string    | ✅       | `http://…/dashboard`             | Mở trang khi kết nối. |
| `device_id`             | string    | ❌       | `"my-device"` or auto (`esp32-<mac>`) | Mã định danh được máy chủ sử dụng. Nếu không được thiết lập, thành phần sẽ lấy `esp32-<mac>` từ MAC của chip và gửi nó. |
| `tile_size`             | int       | ❌       | `32`                              | Kích thước cạnh ô tính bằng pixel. Giúp máy chủ chọn cách đóng gói ô; tốt nhất nên giữ kích thước này là bội số của 16. |
| `full_frame_tile_count` | int       | ❌       | `4`                               | Số lượng ô mà máy chủ nên sử dụng để cập nhật toàn khung hình. |
| `full_frame_area_threshold` | float | ❌       | `0.50`                            | Diện tích delta (phần màn hình) mà máy chủ phải gửi một khung hình đầy đủ. |
| `full_frame_every`      | int       | ❌       | `50`                              | Buộc cập nhật toàn khung hình sau mỗi N khung hình (0 vô hiệu hóa). |
| `every_nth_frame`       | int       | ❌       | `1`                               | Bộ chia tốc độ khung hình. Máy chủ chỉ nên gửi mỗi khung hình thứ N. |
| `min_frame_interval`    | int (ms)  | ❌       | `80`                              | Thời gian tối thiểu giữa các khung hình, tính bằng mili giây.|
| `jpeg_quality`          | int       | ❌       | `85`                              | Gợi ý chất lượng JPEG cho bộ mã hóa của máy chủ. |
| `max_bytes_per_msg`     | int (B)   | ❌       | `14336` or `61440`                | Giới hạn trên cho một tin nhắn nhị phân WS duy nhất. |
| `big_endian`            | bool      | ❌       | `true` or `false`                 | Sử dụng thứ tự pixel RGB565 big-endian cho đầu ra JPEG (đặt false cho bảng điều khiển little-endian). Mặc định là `true`. |
| `rotation`              | int       | ❌       | 0, 90, 180, 270                   | Cho phép xoay phần mềm cho cả màn hình hiển thị và màn hình cảm ứng. |

## Khuyến nghị

- **full_frame_tile_count** được đặt thành 1 là cách hiệu quả nhất để thực hiện cập nhật toàn màn hình; hãy sử dụng tùy chọn này nếu bộ nhớ mạng/thiết bị của bạn cho phép.

- **every_nth_frame** phải bằng 1 nếu bạn không muốn bỏ lỡ các thay đổi (mặc dù việc tăng giá trị này có thể làm giảm tải máy chủ). Tôi khuyên bạn nên giữ nguyên giá trị này ở mức 1.

- **min_frame_interval** nên lớn hơn một chút so với thời gian render được báo cáo bởi self-test (đặt `self-test` làm tham số url trong yaml).
- **max_bytes_per_msg** nên lớn hơn kích thước ô tối đa của bạn (toàn khung hình hoặc một phần).
- **jpeg_quality** — các giá trị thấp hơn sẽ mã hóa nhanh hơn và giảm băng thông (nhưng làm tăng hiện tượng nhiễu). Bắt đầu từ **85**, giảm xuống **70–75** nếu bạn cần tốc độ.
- **big_endian** — mặc định là **true**. Nếu màu sắc trông không đúng (bị hoán đổi/bị tô màu), hãy đặt `big_endian: false` cho các bảng điều khiển yêu cầu chế độ little-endian RGB565.
- **Red tile/red screen** — điều này cho biết tải trọng ô đã vượt quá `max_bytes_per_msg`. Tăng `max_bytes_per_msg` hoặc giảm kích thước ô/chất lượng JPEG để mỗi ô vừa vặn.

## Không có bàn phím trên màn hình

Không có bàn phím trên màn hình; bạn sẽ cần [sử dụng công cụ phát triển Chrome](https://github.com/strange-v/RemoteWebViewServer#accessing-the-servers-tab-with-chrome-devtools) cho bất kỳ dữ liệu đầu vào nào cần thiết.

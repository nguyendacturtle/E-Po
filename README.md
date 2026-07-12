# 🔊 E_PO_test_sound — ESP32 Engine Sound Simulator

> **Mô phỏng âm thanh động cơ thực tế trên ESP32, sử dụng sample PCM từ xe thật, xuất âm thanh qua mạch khuếch đại PAM8610 + Loa 3W, điều khiển bằng biến trở và cấu hình bằng Bluetooth Low Energy (BLE).**

---

## 📋 Mục lục

- [Giới thiệu](#-giới-thiệu)
- [Nguồn âm thanh](#-nguồn-âm-thanh)
- [Danh sách linh kiện](#-danh-sách-linh-kiện)
- [Sơ đồ kết nối phần cứng](#-sơ-đồ-kết-nối-phần-cứng)
- [Cấp nguồn điện](#-cấp-nguồn-điện)
- [Cấu trúc phần mềm](#-cấu-trúc-phần-mềm)
- [Luồng dữ liệu tổng thể](#-luồng-dữ-liệu-tổng-thể)
- [Giải thích chi tiết từng khối code](#-giải-thích-chi-tiết-từng-khối-code)
- [Giao tiếp Bluetooth (BLE)](#-giao-tiếp-bluetooth-ble)
- [Thông số cấu hình](#-thông-số-cấu-hình)
- [Hướng dẫn cài đặt & build](#-hướng-dẫn-cài-đặt--build)
- [Gỡ lỗi qua Serial Monitor](#-gỡ-lỗi-qua-serial-monitor)
- [Lưu ý kỹ thuật](#-lưu-ý-kỹ-thuật)

---

## 🎯 Giới thiệu

Project này biến ESP32 thành một **bộ mô phỏng âm thanh động cơ** hoàn chỉnh:

- 🎙️ **Âm thanh thực** — sample PCM 8-bit 22050 Hz được ghi âm từ xe thật
- 🎚️ **Pitch-shift real-time** — thay đổi tần số âm thanh theo RPM mà không cần bộ xử lý DSP rời
- 🔀 **Crossfade idle ↔ rev** — chuyển mượt mà giữa tiếng máy cầm chừng và tiếng tua ga
- 🔑 **Tiếng đề máy** — phát âm thanh khởi động thực tế một lần trước khi vào idle
- 🎛️ **Điều khiển biến trở** — vặn biến trở để điều chỉnh RPM giống tay ga xe máy/xe hơi
- 📱 **Điều khiển bằng BLE / Serial** — dễ dàng thay đổi loại âm thanh động cơ thông qua ứng dụng Bluetooth điện thoại hoặc Serial Monitor bằng các lệnh cấu hình.

---

## 🎵 Nguồn âm thanh

Âm thanh được lấy từ project mã nguồn mở **[TheDIYGuy999/Rc_Engine_Sound_ESP32](https://github.com/TheDIYGuy999/Rc_Engine_Sound_ESP32)**.

Trong phiên bản này, firmware **tích hợp sẵn 8 loại âm thanh động cơ** khác nhau, có thể chuyển đổi linh hoạt khi đang chạy bằng các lệnh từ `S1` đến `S8` qua Serial hoặc Bluetooth:

1. **S1:** Land Rover Defender V8 Open Pipe
2. **S2:** Scania V8 diesel truck
3. **S3:** Ford Mustang V8 1965
4. **S4:** Jeep Wrangler Rubicon 392 V8
5. **S5:** Volkswagen Beetle
6. **S6:** Harley-Davidson FXSB
7. **S7:** Ferrari LaFerrari
8. **S8:** Scania V8 1000 HP

---

## 🧰 Danh sách linh kiện

| STT | Linh kiện | Số lượng | Ghi chú |
|-----|-----------|----------|---------|
| 1 | **ESP32 DevKit v1** (hoặc tương đương) | 1 | Chip ESP32-WROOM-32, 240 MHz dual-core |
| 2 | **Mạch khuếch đại PAM8610** | 1 | Class D, 2×15W, VCC 7V–15V (tối ưu 12V) |
| 3 | **Loa 3W** (4Ω hoặc 8Ω) | 1 | Kết nối vào kênh L của PAM8610 |
| 4 | **Biến trở 10kΩ** | 1 | Tuyến tính (Linear), 3 chân |
| 5 | **Nguồn 12V DC** | 1 | Cấp cho PAM8610 |
| 6 | **Mạch hạ áp Buck** (LM2596 hoặc tương đương) | 1 | Hạ từ 12V xuống 5V cho ESP32 |

---

## 🔌 Sơ đồ kết nối phần cứng

### Biến trở (Tay ga giả lập)

```text
          ┌─────────────────────────┐
          │       BIẾN TRỞ          │
          │      10kΩ Linear        │
          └──┬──────────┬──────────┬┘
             │          │          │
           Chân 1     Chân 2     Chân 3
          (Trái)    (Giữa/OUT)  (Phải)
             │          │          │
           3.3V      GPIO 34      GND
          (ESP32)   (ADC1_CH6)  (ESP32)
```

### Mạch khuếch đại PAM8610

```text
          ┌──────────────────────────────┐
          │         PAM8610              │
          │  2×15W Class D Amplifier     │
          ├──────────┬───────────────────┤
          │  INPUT   │  OUTPUT           │
          │          │                   │
  GPIO25──┤ L_IN     │ L+ ──── Loa(+)   │
  GPIO26──┤ R_IN     │ L- ──── Loa(-)   │
    GND ──┤ AGND     │                   │
          │          │ R+  (không dùng)  │
  12V  ───┤ VCC      │ R-  (không dùng)  │
    GND ──┤ GND      │                   │
          └──────────┴───────────────────┘
```

> 💡 **Ghi chú:** Cả GPIO 25 và 26 đều phát tín hiệu giống hệt nhau, nên bạn có thể cắm loa vào kênh L hoặc R tuỳ thích.

---

## ⚡ Cấp nguồn điện

### Lưu ý quan trọng — GND chung

```text
                PHẢI NỐI CHUNG GND!
                ───────────────────
  Nguồn 12V GND ──┐
  Buck/LM2596 GND─┤──── GND CHUNG ────┬── ESP32 GND
  PAM8610 GND ────┘                   └── Biến trở Chân 3
```

---

## 🏗️ Cấu trúc phần mềm

### Kiến trúc Non-Blocking Audio (Vòng đệm & Ngắt)

Ở phiên bản mới, để cho phép kết nối Bluetooth Low Energy (BLE) hoạt động song song mà không làm sập (crash) hệ thống âm thanh, project sử dụng kiến trúc **Ring Buffer** (Bộ đệm vòng) kết hợp với **thao tác ghi thanh ghi trực tiếp**.

```text
┌──────────────────────────┐     ┌────────────────────────────────┐
│        CORE 0            │     │           CORE 1               │
│  (PRO CPU — Default ISR) │     │  (APP CPU — Arduino loop())    │
│                          │     │                                │
│  ┌────────────────────┐  │     │  ┌──────────────────────────┐  │
│  │  Hardware Timer 1  │  │     │  │  loop() @ 10ms cycle     │  │
│  │  22050 Hz Alarm    │  │     │  │                          │  │
│  └────────┬───────────┘  │     │  │  1. Đọc ADC (Biến trở)   │  │
│           │ ISR          │     │  │  2. Xử lý BLE / Serial   │  │
│  ┌────────▼───────────┐  │     │  │  3. Cập nhật State       │  │
│  │   onTimerISR()     │  │◄─┬──┼──┤     (Engine RPM)         │  │
│  │   [IRAM_ATTR]      │  │  │  │  │  4. fillAudioBuffer()    │  │
│  │                    │  │  │  │  │     → Render Pitch-shift │  │
│  │ 1. Lấy sample từ   │  │  │  │  │     → Render Crossfade   │  │
│  │    AudioBuf[Tail]  │  │  │  │  │     → Đẩy vào AudioBuf   │  │
│  │ 2. Ghi trực tiếp ra│  │  │  │  │                          │  │
│  │    thanh ghi RTC   │  │  │  │  │  5. BLE Notify (Log)     │  │
│  └────────┬───────────┘  │  │  │  └──────────────────────────┘  │
│           │              │  │  │                                │
│           ▼              │  │  │  ┌──────────────────────────┐  │
│      GPIO 25 & 26        │  └──┼──┤  Audio Ring Buffer       │  │
│       (Loa)              │     │  │  (2048 samples)          │  │
└──────────────────────────┘     └────────────────────────────────┘
```

### Kiến trúc File (Modular)

Mã nguồn dự án được chia thành các module chức năng riêng biệt để dễ bảo trì và mở rộng:

- **`config.h`**: Lưu trữ toàn bộ các hằng số cài đặt như cấu hình phần cứng (Pin), thông số xử lý biến trở (ADC), cấu hình RPM, và Volume.
- **`audio_engine.h / .cpp`**: "Trái tim" của hệ thống âm thanh. Quản lý bộ đệm vòng (Ring Buffer), ngắt Timer ở tần số cao (`onTimerISR`), điều khiển thao tác thanh ghi (DAC registers), trộn âm (Crossfade), và vòng lặp `fillAudioBuffer()`.
- **`ble_manager.h / .cpp`**: Xử lý toàn bộ logic liên quan đến kết nối Bluetooth Low Energy (BLE Server), phân tích cú pháp lệnh (Command parsing), và trả log phản hồi qua điện thoại.
- **`sound_registry.h`**: Bảng điều phối lưu trữ địa chỉ và cấu trúc của 8 file dữ liệu âm thanh (PCM samples).
- **`main.cpp`**: Rất gọn nhẹ, chỉ chịu trách nhiệm đọc tín hiệu tay ga (ADC throttle) và quản lý Máy trạng thái (State Machine) của động cơ (`ENG_OFF` -> `ENG_STARTING` -> `ENG_RUNNING`).

1. `fillAudioBuffer()`: Hàm này tính toán Pitch Shift và Crossfade (bằng cách lấy dữ liệu từ Flash), sau đó điền vào mảng đệm (RAM). Nó được thực thi ở Core 1.
2. `onTimerISR()`: Hàm ngắt được kích hoạt mỗi 45 micro-giây (22050Hz). Do BLE thường xuyên khóa Cache (Cache Disabled), hàm này không được phép đọc Flash, mà chỉ rút dữ liệu đã được tính sẵn trong mảng đệm trên RAM ra loa.

---

## 📱 Giao tiếp Bluetooth (BLE)

ESP32 phát tín hiệu BLE với tên `E_PO Engine Sound`. 
Bạn có thể kết nối thông qua các ứng dụng như **nRF Connect**, **LightBlue** hoặc ứng dụng tuỳ chỉnh.

**UUIDs:**
- **Service UUID:** `4fafc201-1fb5-459e-8fcc-c5c9c331914b`
- **RX Characteristic (Gửi lệnh):** `beb5483e-36e1-4688-b7f5-ea07361b26a8` (Quyền Write)
- **TX Characteristic (Nhận log):** `8248c894-3a2d-425b-801e-2df19d08ee72` (Quyền Notify)

**Lệnh điều khiển:**
Gửi chuỗi văn bản (String) để điều khiển:
- `S1` : Đổi sang âm thanh Land Rover Defender V8 (Xe số 1)
- `S2` : Đổi sang xe số 2...
- `S8` : Đổi sang xe số 8
*(Bạn có thể gõ tương tự thông qua màn hình Serial của máy tính)*

---

## ⚙️ Thông số cấu hình

Tất cả thông số đều nằm ở đầu file [src/main.cpp](src/main.cpp).

### ADC & RPM
| Define | Giá trị | Mô tả |
|--------|---------|-------|
| `ADC_CLAMP_LOW` | `80` | Cắt bỏ nhiễu phi tuyến đầu thấp |
| `ADC_DEADBAND` | `60` | Vùng chết tay ga (tránh RPM dao động ở 0) |
| `RPM_IDLE` | `800.0f` | RPM cầm chừng (RPM tham chiếu sample) |
| `RPM_MAX` | `4500.0f` | RPM tối đa khi full throttle |

### Cấu hình Audio
| Define | Giá trị | Mô tả |
|--------|---------|-------|
| `AUDIO_BUF_SIZE` | `2048` | Kích thước mảng đệm (bytes). Tăng lên giúp giảm khựng âm nhưng tăng độ trễ (delay) |
| `VOL_IDLE` | `100` | % volume khi cầm chừng |
| `VOL_START` | `90` | % volume tiếng đề máy |

---

## 🚀 Hướng dẫn cài đặt & Build

1. **Chuẩn bị môi trường:** 
   Sử dụng [PlatformIO](https://platformio.org/) hoặc Arduino IDE (cài thư viện liên quan nếu dùng Arduino IDE).
2. **Build / Nạp Code:**
   Trong PlatformIO, chạy lệnh:
   ```bash
   pio run --target upload
   ```
3. Mạch sẽ tự động chia lại phân vùng Flash (Custom Partitions) nhờ vào cấu hình `board_build.partitions = huge_app.csv` (vì 8 file âm thanh có dung lượng khá lớn, khoảng 1.5MB).

---

## 🔍 Gỡ lỗi qua Serial Monitor

Mở Serial Monitor ở **115200 baud**. Mỗi 300ms, ESP32 in ra:

```text
=================================================
 E_PO Engine Sound — ESP32 + PAM8610
 Loaded 8 sound profiles.
 Type S1, S2, S3... to switch sounds.
 RPM range     : 800 – 4500 RPM
=================================================

[SYSTEM] Switched to sound S1: DefenderV8OpenPipe
 Xoay bien tro de khoi dong dong co...
[BLE] Initializing BLE...
[OK] BLE Started. Waiting for connections...
[OK] Timer ISR armed.
[ENGINE] Cranking...
[STARTING] RPM:     0 | THR:  11.8% | VOL: 229 | RevMix:   0 | ISR: 34389
[RUNNING] RPM:  1200 | THR:  15.3% | VOL: 106 | RevMix:  56 | ISR: 98124
```

**Giải thích các trường:**
| Trường | Mô tả |
|--------|-------|
| `RPM` | RPM hiện tại (có inertia) |
| `THR` | Throttle đọc từ biến trở (%) |
| `VOL` | Volume tổng hiện tại (0-255) |
| `RevMix` | Tỉ lệ crossfade: 0=pure idle, 255=pure rev |
| `ISR` | Số lần bộ ngắt Timer 1 đã gọi để đẩy 1 bit tín hiệu âm thanh ra Loa |

---

## ⚠️ Lưu ý kỹ thuật

### Tắt Caching (Flash Cache Miss)
Lõi Bluetooth (BLE) của ESP32 đòi hỏi CPU đáp ứng cực kì đúng hạn. Khi BLE hoạt động, nó sẽ thi thoảng vô hiệu hóa bộ đệm (Cache) liên kết tới Flash. 
Bất kì ngắt (ISR) nào cố đọc dữ liệu từ Flash (hoặc chạy hàm lưu trên Flash như `dacWrite()`) trong lúc Cache tắt sẽ làm hệ thống **Dump / Reboot (Lỗi Cache Disabled But Cached Memory Region Accessed)**.
Kiến trúc ở đây giải quyết triệt để bằng cách:
1. Load hàm ISR và các biến lên `IRAM` thông qua `IRAM_ATTR`.
2. Dùng Ring Buffer trên RAM thay vì truy xuất chuỗi Flash bằng ISR.
3. Thay `dacWrite()` bằng thao tác gán nhị phân trực tiếp trên thanh ghi `RTC_IO_PAD_DAC1_REG`.

---

*Tài liệu cập nhật: 2026-07-12*

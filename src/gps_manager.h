#ifndef GPS_MANAGER_H
#define GPS_MANAGER_H

#include <Arduino.h>
#include <HardwareSerial.h>
#include <math.h>
#include "config.h"

// ─── CẤU TRÚC DỮ LIỆU GPS ────────────────────────────────────────────────────
struct GPS_Data {
  bool  valid;        // Có tín hiệu 3D Fix hay không
  float latitude;     // Vĩ độ (thập phân)
  float longitude;    // Kinh độ (thập phân)
  float speed_kmh;    // Tốc độ di chuyển (km/h)
  float distance_m;   // Tổng quãng đường đi được (mét)
};

// Biến toàn cục chứa dữ liệu GPS
static GPS_Data gpsData = {false, 0.0f, 0.0f, 0.0f, 0.0f};

// ─── PHÂN TÍCH VÀ XỬ LÝ NỘI BỘ ───────────────────────────────────────────────
static char _gps_line_buf[256]; // Tăng từ 120 lên 256 đề phòng chuỗi NMEA dài khi bắt nhiều vệ tinh
static int  _gps_line_idx = 0;
static uint32_t gpsBytesRx = 0; // Đếm số byte thô nhận được từ GPS
static uint32_t gpsLinesRx = 0; // Đếm số câu lệnh NMEA nhận được
static int      gpsSatellites = 0; // Số lượng vệ tinh đang bắt được

// Công thức Haversine tính khoảng cách giữa 2 điểm tọa độ (Lat/Lon) ra mét
static float _gps_haversine(float lat1, float lon1, float lat2, float lon2) {
  float dLat = (lat2 - lat1) * (float)M_PI / 180.0f;
  float dLon = (lon2 - lon1) * (float)M_PI / 180.0f;
  
  float a = sinf(dLat / 2.0f) * sinf(dLat / 2.0f) +
            cosf(lat1 * (float)M_PI / 180.0f) * cosf(lat2 * (float)M_PI / 180.0f) *
            sinf(dLon / 2.0f) * sinf(dLon / 2.0f);
            
  float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
  return 6371000.0f * c; // Bán kính Trái Đất ~6,371,000 mét
}

// Chuyển đổi định dạng góc NMEA (DDMM.MMMM) sang độ thập phân (Decimal Degrees)
static float _gps_nmea_to_decimal(const char* s, bool is_lat) {
  float raw = atof(s);
  int deg = (int)(raw / 100.0f);
  float min = raw - (deg * 100.0f);
  return (float)deg + min / 60.0f;
}

// Tự cắt chuỗi CSV bảo toàn trường trống (strtok mặc định bỏ qua dấu phẩy rỗng ,,)
static int _gps_split_csv(char* str, char** fields, int max_fields) {
  int idx = 0;
  fields[idx++] = str;
  char* p = str;
  while (*p) {
    if (*p == ',') {
      *p = '\0';
      if (idx < max_fields) {
        fields[idx++] = p + 1;
      }
    }
    p++;
  }
  return idx;
}

// Phân tích chuỗi NMEA GGA ($GPGGA hoặc $GNGGA) để lấy số lượng vệ tinh
static void _gps_parse_gga(char* line) {
  char* fields[15];
  int f_count = _gps_split_csv(line, fields, 15);
  
  if (f_count >= 8) {
    // Trường 7: Số lượng vệ tinh đang kết nối
    gpsSatellites = atoi(fields[7]);
  }
}

// Phân tích chuỗi NMEA RMC ($GPRMC hoặc $GNRMC)
static void _gps_parse_rmc(char* line) {
  char* fields[15];
  int f_count = _gps_split_csv(line, fields, 15);
  
  if (f_count < 8) return; // Thiếu trường dữ liệu
  
  // Trường 2: Trạng thái (A = Active/Valid, V = Void/Invalid)
  if (fields[2][0] == 'A') {
    float lat = _gps_nmea_to_decimal(fields[3], true);
    if (fields[4][0] == 'S') lat = -lat;
    
    float lon = _gps_nmea_to_decimal(fields[5], false);
    if (fields[6][0] == 'W') lon = -lon;
    
    float speed_knots = atof(fields[7]);
    float speed_kmh = speed_knots * 1.852f; // 1 knot = 1.852 km/h
    
    // Bộ lọc thông thấp giảm nhiễu nhảy tốc độ
    gpsData.speed_kmh = 0.7f * gpsData.speed_kmh + 0.3f * speed_kmh;
    if (gpsData.speed_kmh < 0.8f) {
      gpsData.speed_kmh = 0.0f; // Vùng chết (deadband) 0.8 km/h để chống trôi khi đứng yên
    }
    
    // Tính toán tích lũy quãng đường nếu tọa độ trước đó hợp lệ
    if (gpsData.valid && gpsData.latitude != 0.0f && gpsData.longitude != 0.0f) {
      float dist = _gps_haversine(gpsData.latitude, gpsData.longitude, lat, lon);
      
      // Lọc bỏ lỗi nhảy tọa độ đột ngột (GPS Glitch)
      // Nếu trong 200ms di chuyển hơn 30 mét (~540 km/h) thì bỏ qua
      if (dist > 0.3f && dist < 30.0f) {
        gpsData.distance_m += dist;
      }
    }
    
    gpsData.latitude = lat;
    gpsData.longitude = lon;
    gpsData.valid = true;
  } else {
    gpsData.valid = false;
    gpsData.speed_kmh = 0.0f;
  }
}

// ─── API CÔNG KHAI ───────────────────────────────────────────────────────────

/**
 * Khởi tạo UART2 kết nối cảm biến GPS ATGM336H.
 * Tự động đồng bộ cấu hình tốc độ truyền 115200 bps và tần số cập nhật 5Hz.
 */
static bool GPS_init() {
  Serial.println("[GPS] Khoi tao Serial2 ket noi GPS ATGM336H...");
  
  // Tăng bộ đệm RX của UART2 lên 2048 byte để chống mất mát dữ liệu khi I2S block
  Serial2.setRxBufferSize(2048);

  // 1. Giao tiếp ở tốc độ mặc định 9600 bps trước
  Serial2.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(100);
  
  // 2. Gửi lệnh đổi baudrate của module GPS sang 115200 bps
  Serial2.print("$PCAS01,5*19\r\n");
  delay(100);
  
  // 3. Đóng Serial cũ và mở lại ở tốc độ 115200 bps
  Serial2.end();
  delay(50);
  
  Serial2.setRxBufferSize(2048);
  Serial2.begin(115200, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(100);
  
  // 4. Cấu hình tần số lấy mẫu/phát dữ liệu sang 5 Hz (mỗi 200ms)
  Serial2.print("$PCAS02,200*1D\r\n");
  delay(100);
  
  Serial.println("[GPS] Cau hinh toc do 115200 bps va tan so 5Hz thanh cong!");
  return true;
}

/**
 * Đọc bytes từ GPS Serial và cập nhật thông tin.
 * Cần được gọi liên tục trong loop().
 */
static void GPS_update() {
  while (Serial2.available() > 0) {
    char c = Serial2.read();
    gpsBytesRx++;
    
    if (c == '\r' || c == '\n') {
      if (_gps_line_idx > 0) {
        _gps_line_buf[_gps_line_idx] = '\0';
        gpsLinesRx++;
        
        // Phân tích các câu lệnh RMC (tốc độ/vị trí) hoặc GGA (số vệ tinh)
        if (strstr(_gps_line_buf, "RMC") != NULL) {
          _gps_parse_rmc(_gps_line_buf);
        } else if (strstr(_gps_line_buf, "GGA") != NULL) {
          _gps_parse_gga(_gps_line_buf);
        }
        
        _gps_line_idx = 0; // Reset bộ đệm dòng
      }
    } else {
      if (_gps_line_idx < (int)sizeof(_gps_line_buf) - 2) {
        _gps_line_buf[_gps_line_idx++] = c;
      } else {
        _gps_line_idx = 0; // Quá tải bộ đệm, reset
      }
    }
  }
}

/**
 * In dữ liệu GPS ra màn hình log.
 */
static void GPS_printLog(char* outBuf, size_t maxLen) {
  if (gpsData.valid) {
    snprintf(outBuf, maxLen,
             "===== GPS ATGM336H DATA =====\n"
             "Trang thai    : KHOA TIN HIEU (FIX)\n"
             "Vi do / Kinh do: %.6f, %.6f\n"
             "Toc do tuc thoi: %.2f km/h\n"
             "Quang duong ODO: %.1f met\n"
             "Ve tinh (Sats): %d\n"
             "Debug         : RX Bytes=%u, Lines=%u\n"
             "=============================\n",
             gpsData.latitude, gpsData.longitude, gpsData.speed_kmh, gpsData.distance_m,
             gpsSatellites, gpsBytesRx, gpsLinesRx);
  } else {
    snprintf(outBuf, maxLen,
             "===== GPS ATGM336H DATA =====\n"
             "Trang thai    : CHUA KHOA SONG (NO FIX)\n"
             "Toc do tuc thoi: 0.00 km/h\n"
             "Quang duong ODO: %.1f met\n"
             "Ve tinh (Sats): %d\n"
             "Debug         : RX Bytes=%u, Lines=%u\n"
             "=============================\n",
             gpsData.distance_m,
             gpsSatellites, gpsBytesRx, gpsLinesRx);
  }
}

#endif // GPS_MANAGER_H

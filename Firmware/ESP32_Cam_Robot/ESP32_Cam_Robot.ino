#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"
#include "Arduino.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// --- WI-FI SETTINGS ---
const char* ssid = "ESP32-Cam-Robot";
const char* password = "---";

// --- PIN DEFINITIONS (Do not change) ---
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// --- HARDWARE PINS ---
#define SERVO_PIN 2       // Camera Angle
#define TRIG_PIN 1        // Ultrasonic Trig
#define ECHO_PIN 3        // Ultrasonic Echo
#define BUILT_IN_LED 4    // Flash Light
#define IN_1 12           // Motor Left A
#define IN_2 13           // Motor Left B
#define IN_3 14           // Motor Right A
#define IN_4 15           // Motor Right B

// --- GLOBAL VARIABLES ---
int speed = 150;          // Default Speed
int servo_val = 5200;     // Center Camera
bool flash_on = false;    // Light State
httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

// --- MOTOR CONTROL HELPER ---
void setMotors(int s1, int s2, int s3, int s4) {
    ledcWrite(4, s1); ledcWrite(5, s2);
    ledcWrite(6, s3); ledcWrite(7, s4);
}

// --- WEB INTERFACE (HTML/CSS/JS) ---
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32-CAM Robot</title>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial, sans-serif; background: #1a1a1a; color: #eee; text-align: center; margin:0; padding:10px; }
        .grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; max-width: 320px; margin: 20px auto; }
        button { background: #333; color: white; padding: 20px; border-radius: 10px; border: none; font-weight: bold; font-size: 16px; cursor: pointer; width: 100%; }
        button:active { background: #555; }
        .fw { grid-column: 2; grid-row: 1; }
        .left { grid-column: 1; grid-row: 2; }
        .rev { grid-column: 2; grid-row: 2; }
        .right { grid-column: 3; grid-row: 2; }
        .stop { background: #d32f2f; grid-column: span 3; grid-row: 3; margin-top: 5px; }
        .slider-container { margin: 20px auto; max-width: 400px; text-align: left; }
        input[type=range] { width: 100%; }
        .flash-btn { background: #fbc02d; color: #000; margin-top: 20px; padding: 10px 30px; }
    </style>
</head>
<body>
    <h2>ESP32 CONTROL PANEL</h2>
    <img src="" id="stream" style="width:100%; max-width:640px; border-radius:8px; background:#000;">
    
    <div class="grid">
        <button class="fw" onmousedown="send('car', 1)" onmouseup="send('car', 3)">FWD</button>
        <button class="left" onmousedown="send('car', 2)" onmouseup="send('car', 3)">LEFT</button>
        <button class="rev" onmousedown="send('car', 5)" onmouseup="send('car', 3)">REV</button>
        <button class="right" onmousedown="send('car', 4)" onmouseup="send('car', 3)">RIGHT</button>
        <button class="stop" onclick="send('car', 3)">EMERGENCY STOP</button>
    </div>

    <div class="slider-container">
        <label>Motor Speed (120-255)</label>
        <input type="range" min="120" max="255" value="150" onchange="send('speed', this.value)">
    </div>
    
    <div class="slider-container">
        <label>Camera Angle</label>
        <input type="range" min="2600" max="7800" value="5200" onchange="send('servo', this.value)">
    </div>

    <button class="flash-btn" onclick="send('flash', 1)">TOGGLE LIGHT</button>

    <script>
        function send(v, val) { fetch(`/control?var=${v}&val=${val}`); }
        // Load stream from port 81
        window.onload = () => { document.getElementById('stream').src = window.location.origin + ':81/stream'; };
    </script>
</body>
</html>)rawliteral";

// --- REQUEST HANDLERS ---

// 1. Serve the HTML Page
static esp_err_t index_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

// 2. Handle Browser Commands (Sliders & Buttons)
static esp_err_t cmd_handler(httpd_req_t *req) {
    char var[32], val_str[32], query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "var", var, 32) == ESP_OK && httpd_query_key_value(query, "val", val_str, 32) == ESP_OK) {
            int v = atoi(val_str);
            // Toggle Light
            if (!strcmp(var, "flash")) { 
                flash_on = !flash_on; 
                ledcWrite(2, flash_on ? 255 : 0); 
            }
            // Move Servo
            else if (!strcmp(var, "servo")) {
                servo_val = v;
                ledcWrite(3, servo_val);
            }
            // Set Speed
            else if (!strcmp(var, "speed")) {
                speed = v;
            }
            // Move Car
            else if (!strcmp(var, "car")) {
                if (v == 1) setMotors(0, speed, 0, speed);        // FWD
                else if (v == 2) setMotors(speed, 0, 0, speed);   // LEFT
                else if (v == 3) setMotors(0, 0, 0, 0);           // STOP
                else if (v == 4) setMotors(0, speed, speed, 0);   // RIGHT
                else if (v == 5) setMotors(speed, 0, speed, 0);   // REV
            }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

// 3. Handle Python/AI Commands
static esp_err_t data_handler(httpd_req_t *req) {
    char buffer[32];
    int ret = httpd_req_recv(req, buffer, min((int)req->content_len, 31));
    if (ret <= 0) return ESP_FAIL;
    buffer[ret] = '\0';

    // Map Python strings to Motor Actions
    if (strcmp(buffer, "fw") == 0) setMotors(0, speed, 0, speed);
    else if (strcmp(buffer, "stop") == 0) setMotors(0, 0, 0, 0);
    else if (strcmp(buffer, "left") == 0) { setMotors(speed, 0, 0, speed); delay(150); setMotors(0,0,0,0); }
    else if (strcmp(buffer, "right") == 0) { setMotors(0, speed, speed, 0); delay(150); setMotors(0,0,0,0); }
    else if (strcmp(buffer, "servor") == 0) { if(servo_val >= 2800) servo_val -= 200; ledcWrite(3, servo_val); }
    else if (strcmp(buffer, "servol") == 0) { if(servo_val <= 7600) servo_val += 200; ledcWrite(3, servo_val); }
    else if (strcmp(buffer, "rstservo") == 0) { servo_val = 5200; ledcWrite(3, servo_val); }
    
    return httpd_resp_send(req, "OK", 2);
}

// 4. Handle Video Stream
static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    char part_buf[64];
    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=123456");
    while (true) {
        fb = esp_camera_fb_get();
        if (!fb) break;
        size_t hlen = snprintf(part_buf, 64, "\r\n--123456\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (res != ESP_OK) break;
    }
    return res;
}

// --- SETUP FUNCTIONS ---
void startCameraServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    
    httpd_uri_t index_uri = { "/", HTTP_GET, index_handler, NULL };
    httpd_uri_t cmd_uri = { "/control", HTTP_GET, cmd_handler, NULL };
    httpd_uri_t data_uri = { "/data", HTTP_POST, data_handler, NULL };
    
    if (httpd_start(&camera_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(camera_httpd, &index_uri);
        httpd_register_uri_handler(camera_httpd, &cmd_uri);
        httpd_register_uri_handler(camera_httpd, &data_uri);
    }
    
    config.server_port += 1;
    config.ctrl_port += 1;
    httpd_uri_t stream_uri = { "/stream", HTTP_GET, stream_handler, NULL };
    if (httpd_start(&stream_httpd, &config) == ESP_OK) {
        httpd_register_uri_handler(stream_httpd, &stream_uri);
    }
}

void setup() {
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // Disable brownout
    Serial.begin(115200);

    // 1. Setup Motors (Channels 4,5,6,7)
    for (int i = 4; i <= 7; i++) ledcSetup(i, 2000, 8);
    ledcAttachPin(IN_1, 4); ledcAttachPin(IN_2, 5); 
    ledcAttachPin(IN_3, 6); ledcAttachPin(IN_4, 7);

    // 2. Setup Flash LED (Channel 2)
    ledcSetup(2, 5000, 8); 
    ledcAttachPin(BUILT_IN_LED, 2);

    // 3. Setup Servo (Channel 3)
    ledcSetup(3, 50, 16); 
    ledcAttachPin(SERVO_PIN, 3);

    // 4. Setup Ultrasonic
    pinMode(TRIG_PIN, OUTPUT); 
    pinMode(ECHO_PIN, INPUT);

    // 5. Setup Camera
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM; config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM; config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM; config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM; config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
    config.pin_sscb_sda = SIOD_GPIO_NUM; config.pin_sscb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 10000000; config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_VGA; config.jpeg_quality = 12; config.fb_count = 1;
    
    esp_camera_init(&config);
    WiFi.softAP(ssid, password);
    startCameraServer();
}

void loop() {
    // Safety Loop: Checks distance every 100ms
    digitalWrite(TRIG_PIN, LOW); delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH);
    int distance = duration * 0.034 / 2;
    
    // Safety: If closer than 20cm, EMERGENCY STOP
    if (distance > 0 && distance < 20) {
        setMotors(0, 0, 0, 0);
    }
    delay(100);
}
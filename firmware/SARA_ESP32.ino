#include "esp_camera.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <sstream>

// ===================== MOTORS =====================
struct MOTOR_PINS {
  int pinEn;
  int pinIN1;
  int pinIN2;
};

std::vector<MOTOR_PINS> motorPins = {
  {12, 13, 15},  // RIGHT
  {12, 14, 2}    // LEFT
};

#define LIGHT_PIN 4

#define UP 1
#define DOWN 2
#define LEFT 3
#define RIGHT 4
#define STOP 0

#define RIGHT_MOTOR 0
#define LEFT_MOTOR 1
#define FORWARD 1
#define BACKWARD -1

const int PWMFreq = 1000;
const int PWMResolution = 8;

// ===================== CAMERA PINS =====================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

const char* ssid     = "MyWiFiCar";
const char* password = "12345678";

AsyncWebServer server(80);
AsyncWebSocket wsCamera("/Camera");
AsyncWebSocket wsCarInput("/CarInput");
uint32_t cameraClientId = 0;

// ===================== CAMERA MODES =====================
enum CamMode { 
  MODE_FAST, 
  MODE_BALANCED, 
  MODE_HIGH, 
  MODE_DATABASE,   // best stable quality for database
  MODE_MAX 
};

CamMode currentCamMode = MODE_BALANCED;

void applyCameraMode(CamMode mode) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) return;

  currentCamMode = mode;

  switch (mode) {
    case MODE_FAST:        // 320x240 - responsive driving
      s->set_framesize(s, FRAMESIZE_QVGA);
      s->set_quality(s, 18);
      break;

    case MODE_BALANCED:    // 640x480
      s->set_framesize(s, FRAMESIZE_VGA);
      s->set_quality(s, 14);
      break;

    case MODE_HIGH:        // 800x600
      s->set_framesize(s, FRAMESIZE_SVGA);
      s->set_quality(s, 12);
      break;

    case MODE_DATABASE:    // 1024x768 - best stable quality for database
      s->set_framesize(s, FRAMESIZE_XGA);
      s->set_quality(s, 10);
      break;

    case MODE_MAX:         // 1280x1024 - use only when stopped
      s->set_framesize(s, FRAMESIZE_SXGA);
      s->set_quality(s, 10);
      break;
  }

  Serial.printf("Camera mode → %d\n", mode);
}

// ===================== HTML =====================
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>SARA · Live Vision</title>
  <style>
    :root{color-scheme:dark;--bg:#000;--panel:#090909;--line:#292929;--muted:#a0a0a0;--accent:#e5e5e5;--pink:#ff38c7}
    *{box-sizing:border-box}body{margin:0;background:var(--bg);color:#edf4f8;font:14px/1.4 Inter,Segoe UI,Arial,sans-serif}
    .app{max-width:1500px;margin:auto;padding:18px;display:grid;grid-template-columns:290px minmax(0,1fr);gap:16px;min-height:100vh}
    .panel{background:var(--panel);border:1px solid var(--line);border-radius:14px;padding:16px}
    h1{font-size:18px;margin:0 0 5px;letter-spacing:.04em}h2{font-size:13px;color:var(--muted);font-weight:600;text-transform:uppercase;letter-spacing:.12em;margin:0 0 13px}
    .sub{color:var(--muted);font-size:12px}.status{display:flex;align-items:center;gap:8px;margin:14px 0 18px}
    .dot{width:8px;height:8px;border-radius:50%;background:#e4a34e}.dot.on{background:var(--accent);box-shadow:0 0 10px #54e6a566}
    .pad{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin:12px 0 22px}.drive{height:58px;border:1px solid #344550;border-radius:10px;background:#111;color:#f3f3f3;font-size:24px;touch-action:none;user-select:none}
    .drive:active{background:#252525}.drive.stop{font-size:12px;color:#ff9292}.field{margin:16px 0}.field label{display:flex;justify-content:space-between;color:var(--muted);font-size:12px;margin-bottom:8px}
    input[type=range],select{width:100%;accent-color:var(--accent)}select{background:#111;color:#f3f3f3;border:1px solid #3a3a3a;padding:10px;border-radius:8px}
    .main{display:flex;flex-direction:column;gap:16px;min-width:0}.top{display:flex;justify-content:space-between;align-items:center;gap:12px}.pill{border:1px solid var(--line);border-radius:999px;padding:6px 10px;color:var(--muted);font-size:11px}
    .feed{position:relative;background:#050709;border:1px solid var(--line);border-radius:14px;overflow:hidden;min-height:300px;display:flex;align-items:center;justify-content:center;aspect-ratio:4/3}
    #video{width:100%;height:100%;object-fit:contain;position:absolute;inset:0}.feed-placeholder{color:var(--muted);text-align:center;padding:20px}
    .feed-hud{position:absolute;top:12px;left:12px;right:12px;display:flex;justify-content:space-between;gap:8px;pointer-events:none}.hud-label{background:#07100dd9;border:1px solid #2d5545;border-radius:7px;padding:6px 9px;font-size:11px;color:var(--accent)}
    .stats{display:grid;grid-template-columns:repeat(4,1fr);gap:10px}.stat{background:#050505;border:1px solid var(--line);border-radius:10px;padding:12px}.stat span{display:block;color:var(--muted);font-size:11px;margin-bottom:5px}.stat strong{font-size:21px;font-weight:650}
    .lower{display:grid;grid-template-columns:1fr 1fr;gap:16px}.btn{border:0;border-radius:9px;padding:11px 14px;background:var(--accent);color:#07150f;font-weight:700;cursor:pointer}.btn:disabled{opacity:.4;cursor:not-allowed}
    .legend{display:flex;gap:14px;flex-wrap:wrap;color:var(--muted);font-size:11px;margin-top:12px}.swatch{display:inline-block;width:9px;height:9px;border-radius:2px;margin-right:5px;background:var(--pink)}.swatch.other{background:#00dcff}
    #message{color:var(--muted);font-size:12px;margin-top:10px;min-height:18px}.capture-list{color:var(--muted);font-size:12px;max-height:160px;overflow:auto}
    @media(max-width:850px){.app{grid-template-columns:1fr;padding:10px}.main{order:1}.sidebar{order:2}.feed{min-height:220px}.stats{grid-template-columns:repeat(2,1fr)}.lower{grid-template-columns:1fr}}
  </style>
</head>
<body>
<div class="app">
  <aside class="panel sidebar">
    <h1 style="font-size:22px;letter-spacing:.16em">SARA</h1>
    <div class="sub">Search And Reconnaissance Assistant</div>
    <div class="status"><span id="connectionDot" class="dot"></span><span id="connectionText">Connecting to rover…</span></div>
    <h2>Drive controls <span style="float:right;letter-spacing:.04em">W A S D</span></h2>
    <div class="pad">
      <div></div><button class="drive" data-move="1" aria-label="Forward">↑</button><div></div>
      <button class="drive" data-move="3" aria-label="Left">←</button><button class="drive stop" id="stop">STOP</button><button class="drive" data-move="4" aria-label="Right">→</button>
      <div></div><button class="drive" data-move="2" aria-label="Reverse">↓</button><div></div>
    </div>
    <h2>Camera & rover</h2>
    <div class="field">
      <label for="camMode">Camera mode</label>
      <select id="camMode">
        <option value="0">Fast Driving · 320×240</option>
        <option value="1" selected>Balanced · 640×480</option>
        <option value="2">High Detail · 800×600</option>
        <option value="3">Database Still · 1024×768 (Best quality)</option>
        <option value="4">Max Capture · 1280×1024</option>
      </select>
    </div>
    <div class="field"><label for="speed">Motor speed <b id="speedVal">150</b></label><input id="speed" type="range" min="0" max="255" value="150"></div>
    <div class="field"><label for="light">Torch intensity <b id="lightVal">0</b></label><input id="light" type="range" min="0" max="255" value="0"></div>
    <div class="sub">Keyboard: W forward · S reverse · A left · D right. Release the key to stop. Keep the wheels lifted during the first test.</div>
  </aside>
  <main class="main">
    <section class="panel">
      <div class="top"><div><h1>LIVE CAMERA</h1><div class="sub">Local object detection · Annotated stream</div></div><span class="pill" id="modelStatus">MODEL STARTING</span></div>
      <div class="feed" style="margin-top:14px">
        <img id="video" alt="Annotated live camera feed" hidden>
        <div id="placeholder" class="feed-placeholder">Waiting for the ESP32 camera stream…</div>
        <div class="feed-hud"><span class="hud-label">● LIVE INFERENCE</span><span class="hud-label" id="frameTime">NO FRAME</span></div>
      </div>
      <div class="stats" style="margin-top:12px">
        <div class="stat"><span>People detected</span><strong id="people">—</strong></div>
        <div class="stat"><span>Inference time</span><strong id="inference">—</strong></div>
        <div class="stat"><span>Processing FPS</span><strong id="fps">—</strong></div>
        <div class="stat"><span>Objects detected</span><strong id="objects">—</strong></div>
      </div>
      <div class="legend"><span><i class="swatch"></i>Person</span><span><i class="swatch other"></i>Other YOLO classes</span><span>Labels include confidence</span></div>
      <div id="message"></div>
    </section>
    <div class="lower">
      <section class="panel"><h2>Capture scene</h2><div class="sub" style="margin-bottom:12px">Save the original image, annotated image, timestamp, and detections together.</div><button class="btn" id="captureBtn" disabled>Capture current frame</button><div id="captureMessage" class="sub" style="margin-top:10px"></div></section>
      <section class="panel"><h2>Recent captures</h2><div id="captures" class="capture-list">No captures in this session yet.</div><div class="sub" style="margin-top:10px">AI Search will be added after live detection and capture are verified.</div></section>
    </div>
  </main>
</div>
<script>
let videoSocket, controlSocket, lastCapture = null;
const $ = id => document.getElementById(id);

function connectVideo(){
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  videoSocket = new WebSocket(`${protocol}//${location.host}/Camera`);
  videoSocket.binaryType = "blob";
  videoSocket.onmessage = event => {
    const url = URL.createObjectURL(event.data);
    const img = $("video");
    const old = img.dataset.url;
    img.src = url; img.dataset.url = url; img.hidden = false; $("placeholder").hidden = true;
    img.onload = () => { if(old) URL.revokeObjectURL(old); };
    $("captureBtn").disabled = false;
  };
  videoSocket.onclose = () => setTimeout(connectVideo, 1500);
}

function connectControl(){
  const protocol = location.protocol === "https:" ? "wss:" : "ws:";
  controlSocket = new WebSocket(`${protocol}//${location.host}/CarInput`);
  controlSocket.onopen = () => {
    send("Speed", $("speed").value);
    send("Light", $("light").value);
    send("CamMode", $("camMode").value);
  };
  controlSocket.onclose = () => { setTimeout(connectControl, 1500); stopCar(); };
}

function send(key,value){
  if(controlSocket && controlSocket.readyState===WebSocket.OPEN)
    controlSocket.send(`${key},${value}`);
}

function stopCar(){ send("MoveCar",0); }

document.querySelectorAll("[data-move]").forEach(button=>{
  const start = e => {e.preventDefault(); send("MoveCar",button.dataset.move)};
  const end = e => {e.preventDefault(); stopCar()};
  button.addEventListener("pointerdown",start);
  button.addEventListener("pointerup",end);
  button.addEventListener("pointercancel",end);
  button.addEventListener("pointerleave",e=>{if(e.buttons) end(e)});
});

// WASD keyboard driving
const movementKeys = { w: "1", s: "2", a: "3", d: "4" };
let activeDriveKey = null;

window.addEventListener("keydown", e => {
  const key = e.key.toLowerCase();
  if (!(key in movementKeys) || e.repeat) return;
  const target = e.target;
  if (target && (target.isContentEditable || ["INPUT", "TEXTAREA", "SELECT"].includes(target.tagName))) return;
  e.preventDefault();
  activeDriveKey = key;
  send("MoveCar", movementKeys[key]);
});

window.addEventListener("keyup", e => {
  const key = e.key.toLowerCase();
  if (key in movementKeys && activeDriveKey === key) {
    activeDriveKey = null;
    stopCar();
  }
});

document.addEventListener("visibilitychange", () => {
  if (document.hidden) { activeDriveKey = null; stopCar(); }
});

$("stop").addEventListener("click", stopCar);
$("speed").addEventListener("input", e => {
  $("speedVal").textContent = e.target.value;
  send("Speed", e.target.value);
});
$("light").addEventListener("input", e => {
  $("lightVal").textContent = e.target.value;
  send("Light", e.target.value);
});
$("camMode").addEventListener("change", e => send("CamMode", e.target.value));

// Status / capture still try Python endpoints (optional)
async function pollStatus(){
  try{
    const res = await fetch("/api/status");
    const s = await res.json();
    $("connectionDot").classList.toggle("on", !!s.connected);
    $("connectionText").textContent = s.connected ? "ESP32 camera connected" : "Waiting for ESP32 camera";
    $("modelStatus").textContent = s.error ? "CHECK CONNECTION" : (s.connected ? "MODEL ACTIVE" : "WAITING FOR CAMERA");
    $("people").textContent = s.people ?? "—";
    $("inference").textContent = s.inference_ms == null ? "—" : `${s.inference_ms} ms`;
    $("fps").textContent = s.fps == null ? "—" : s.fps;
    $("objects").textContent = s.detections?.length ?? "—";
    $("frameTime").textContent = s.last_frame_time || "NO FRAME";
    $("message").textContent = s.error || "";
  } catch(e) {
    $("message").textContent = "Python service not running (camera + drive still work)";
    $("connectionDot").classList.add("on");
    $("connectionText").textContent = "ESP32 connected (direct mode)";
  }
}

$("captureBtn").addEventListener("click", async () => {
  $("captureBtn").disabled = true;
  $("captureMessage").textContent = "Saving current processed frame…";
  try {
    const res = await fetch("/api/capture", {method: "POST"});
    const data = await res.json();
    if (!res.ok) throw new Error(data.error || "Capture failed");
    lastCapture = data;
    $("captureMessage").textContent = `Saved at ${data.captured_at}`;
    const line = document.createElement("div");
    line.textContent = `${data.captured_at} · ${data.metadata}`;
    if ($("captures").textContent.includes("No captures")) $("captures").textContent = "";
    $("captures").prepend(line);
  } catch(e) {
    $("captureMessage").textContent = e.message;
  } finally {
    $("captureBtn").disabled = false;
  }
});

connectVideo();
connectControl();
pollStatus();
setInterval(pollStatus, 1000);
window.addEventListener("blur", () => { activeDriveKey = null; stopCar(); });
</script>
</body>
</html>
)rawliteral";

// ===================== MOTOR FUNCTIONS =====================
void rotateMotor(int motorNumber, int motorDirection) {
  if (motorDirection == FORWARD) {
    digitalWrite(motorPins[motorNumber].pinIN1, HIGH);
    digitalWrite(motorPins[motorNumber].pinIN2, LOW);
  } else if (motorDirection == BACKWARD) {
    digitalWrite(motorPins[motorNumber].pinIN1, LOW);
    digitalWrite(motorPins[motorNumber].pinIN2, HIGH);
  } else {
    digitalWrite(motorPins[motorNumber].pinIN1, LOW);
    digitalWrite(motorPins[motorNumber].pinIN2, LOW);
  }
}

void moveCar(int inputValue) {
  switch (inputValue) {
    case UP:
      rotateMotor(RIGHT_MOTOR, FORWARD);
      rotateMotor(LEFT_MOTOR, FORWARD);
      break;
    case DOWN:
      rotateMotor(RIGHT_MOTOR, BACKWARD);
      rotateMotor(LEFT_MOTOR, BACKWARD);
      break;
    case LEFT:
      rotateMotor(RIGHT_MOTOR, FORWARD);
      rotateMotor(LEFT_MOTOR, BACKWARD);
      break;
    case RIGHT:
      rotateMotor(RIGHT_MOTOR, BACKWARD);
      rotateMotor(LEFT_MOTOR, FORWARD);
      break;
    case STOP:
    default:
      rotateMotor(RIGHT_MOTOR, STOP);
      rotateMotor(LEFT_MOTOR, STOP);
      break;
  }
}

// ===================== WEBSOCKET =====================
void onCarInputWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                              AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("Car client connected #%u\n", client->id());
  }
  else if (type == WS_EVT_DISCONNECT) {
    moveCar(STOP);
    ledcWrite(LIGHT_PIN, 0);
  }
  else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo*)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      std::string myData((char*)data, len);
      std::istringstream ss(myData);
      std::string key, value;
      std::getline(ss, key, ',');
      std::getline(ss, value, ',');
      int val = atoi(value.c_str());

      if (key == "MoveCar") {
        moveCar(val);
      }
      else if (key == "Speed") {
        ledcWrite(12, val);
      }
      else if (key == "Light") {
        ledcWrite(LIGHT_PIN, val);
      }
      else if (key == "CamMode") {
        applyCameraMode((CamMode)val);
      }
    }
  }
}

void onCameraWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
                            AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    cameraClientId = client->id();
    Serial.printf("Camera client connected #%u\n", cameraClientId);
  }
  else if (type == WS_EVT_DISCONNECT) {
    cameraClientId = 0;
  }
}

// ===================== CAMERA =====================
void setupCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_VGA;      // start balanced
  config.jpeg_quality = 14;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Camera init failed");
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 0);
  s->set_hmirror(s, 1);    // fix left-right mirror
  s->set_lenc(s, 0);
  s->set_sharpness(s, 0);

  applyCameraMode(MODE_BALANCED);
}

void sendCameraPicture() {
  if (cameraClientId == 0) return;

  AsyncWebSocketClient *client = wsCamera.client(cameraClientId);
  if (!client || client->queueIsFull()) return;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;

  wsCamera.binary(cameraClientId, fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

// ===================== SETUP =====================
void setUpPinModes() {
  ledcAttach(12, PWMFreq, PWMResolution);        // Speed
  ledcAttach(LIGHT_PIN, PWMFreq, PWMResolution); // Flash
  ledcWrite(LIGHT_PIN, 0);

  for (int i = 0; i < motorPins.size(); i++) {
    pinMode(motorPins[i].pinIN1, OUTPUT);
    pinMode(motorPins[i].pinIN2, OUTPUT);
  }
  moveCar(STOP);
}

void setup() {
  Serial.begin(115200);
  setUpPinModes();

  WiFi.softAP(ssid, password);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  wsCamera.onEvent(onCameraWebSocketEvent);
  server.addHandler(&wsCamera);

  wsCarInput.onEvent(onCarInputWebSocketEvent);
  server.addHandler(&wsCarInput);

  server.begin();
  setupCamera();

  Serial.println("System ready");
}

void loop() {
  wsCamera.cleanupClients();
  wsCarInput.cleanupClients();
  sendCameraPicture();
  delay(1);
}

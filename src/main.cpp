#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <XPT2046_Touchscreen.h>

#include "config.h"

namespace {

constexpr uint16_t kBlack = TFT_BLACK;
constexpr uint16_t kWhite = TFT_WHITE;
constexpr uint16_t kRed = TFT_RED;
constexpr uint16_t kYellow = TFT_YELLOW;
constexpr unsigned long kPollIntervalMs = 1500;
constexpr unsigned long kWifiRetryMs = 10000;
constexpr unsigned long kDimAfterMs = 60000;
constexpr unsigned long kSleepAfterMs = 300000;
constexpr uint8_t kBacklightChannel = 0;
constexpr uint8_t kBacklightBright = 255;
constexpr uint8_t kBacklightDim = 55;
constexpr int kTouchCs = 33;
constexpr int kTouchIrq = 36;
constexpr int kTouchClk = 25;
constexpr int kTouchMosi = 32;
constexpr int kTouchMiso = 39;
constexpr int kLedRed = 4;
constexpr int kLedGreen = 16;
constexpr int kLedBlue = 17;
constexpr int kNavigationTop = 207;

TFT_eSPI display;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);
Preferences preferences;
unsigned long lastPollAt = 0;
unsigned long lastWifiAttemptAt = 0;
unsigned long lastInteractionAt = 0;

enum class Page : uint8_t { Dashboard, Health, Controls, Ai };
Page activePage = Page::Dashboard;

struct TouchCalibration {
  int32_t left = 0;
  int32_t right = 0;
  int32_t top = 0;
  int32_t bottom = 0;
  bool valid = false;
};

TouchCalibration touchCalibration;

struct PrinterStatus {
  bool online = false;
  String klippyState = "connecting";
  String printState = "standby";
  String filename;
  String message;
  float nozzleTemperature = 0;
  float nozzleTarget = 0;
  float bedTemperature = 0;
  float bedTarget = 0;
  float progress = 0;
  float printDuration = 0;
  bool filamentPresent = false;
  bool filamentMoving = false;
  bool hasFilamentSensors = false;
  float eddyTemperature = 0;
};

PrinterStatus printer;

struct DisplaySnapshot {
  bool initialized = false;
  String state;
  String wifi;
  String job;
  String elapsed;
  String connection;
  int nozzleTemperature = -999;
  int nozzleTarget = -999;
  int bedTemperature = -999;
  int bedTarget = -999;
  int progressPercent = -1;
  uint16_t headerColor = 0;
};

DisplaySnapshot shown;

struct HealthSnapshot {
  String connection;
  String wifi;
  String filament;
  String eddy;
  String message;
  uint16_t headerColor = 0;
};

HealthSnapshot shownHealth;

String shorten(const String &value, size_t maximum) {
  if (value.length() <= maximum) return value;
  if (maximum < 4) return value.substring(0, maximum);
  return value.substring(0, maximum - 3) + "...";
}

String formatDuration(float seconds) {
  const uint32_t total = seconds < 0 ? 0 : static_cast<uint32_t>(seconds);
  const uint32_t hours = total / 3600;
  const uint32_t minutes = (total % 3600) / 60;
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%02lu:%02lu", static_cast<unsigned long>(hours),
           static_cast<unsigned long>(minutes));
  return String(buffer);
}

uint16_t stateColor() {
  if (!printer.online || printer.klippyState != "ready") return kRed;
  if (printer.printState == "printing") return kYellow;
  if (printer.printState == "paused" || printer.printState == "error" ||
      printer.printState == "cancelled") {
    return kRed;
  }
  return kWhite;
}

String visibleState() {
  if (WiFi.status() != WL_CONNECTED) return "WI-FI OFFLINE";
  if (!printer.online) return "MOONRAKER OFFLINE";
  if (printer.klippyState != "ready") return printer.klippyState;
  return printer.printState;
}

String wifiLabel() {
  if (WiFi.status() != WL_CONNECTED) return "NO WI-FI";
  const int quantizedRssi = (WiFi.RSSI() / 5) * 5;
  return String(quantizedRssi) + " dBm";
}

void setBacklight(uint8_t brightness) { ledcWrite(kBacklightChannel, brightness); }

void setRgb(bool red, bool green, bool blue) {
  // The CYD RGB LED is active-low.
  digitalWrite(kLedRed, red ? LOW : HIGH);
  digitalWrite(kLedGreen, green ? LOW : HIGH);
  digitalWrite(kLedBlue, blue ? LOW : HIGH);
}

void updateRgb() {
  if (!printer.online || printer.klippyState != "ready" || printer.printState == "error") {
    setRgb(true, false, false);
  } else if (printer.printState == "printing") {
    setRgb(true, true, false);
  } else if (printer.printState == "paused") {
    setRgb(false, false, true);
  } else {
    setRgb(true, true, true);
  }
}

void drawNavigation() {
  static const char *labels[] = {"HOME", "HEALTH", "CONTROL", "AI"};
  display.fillRect(0, kNavigationTop, 320, 33, kBlack);
  for (int i = 0; i < 4; ++i) {
    const bool selected = static_cast<int>(activePage) == i;
    const int x = i * 80;
    display.fillRect(x + 1, kNavigationTop + 1, 78, 31, selected ? kYellow : kBlack);
    display.drawRect(x, kNavigationTop, 80, 33, selected ? kYellow : kWhite);
    display.setTextColor(selected ? kBlack : kWhite, selected ? kYellow : kBlack);
    display.setTextDatum(MC_DATUM);
    display.drawString(labels[i], x + 40, kNavigationTop + 16, 2);
  }
}

void drawStaticLayout() {
  display.fillScreen(kBlack);

  display.drawRoundRect(8, 48, 148, 61, 8, kYellow);
  display.drawRoundRect(164, 48, 148, 61, 8, kYellow);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString("NOZZLE", 18, 55, 2);
  display.drawString("BED", 174, 55, 2);

  display.drawRoundRect(8, 117, 304, 42, 8, kWhite);
  display.setTextColor(kWhite, kBlack);
  display.drawString("JOB", 18, 123, 2);

  display.drawRoundRect(9, 167, 302, 20, 6, kWhite);

  shown = DisplaySnapshot{};
  drawNavigation();
}

void drawTemperatureValue(int x, int actual, int target) {
  display.fillRect(x + 4, 73, 140, 31, kBlack);
  char value[24];
  snprintf(value, sizeof(value), "%d / %d C", actual, target);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(kWhite, kBlack);
  display.drawString(value, x + 10, 77, 4);
}

void render(bool force = false) {
  const String state = shorten(visibleState(), 24);
  const String wifi = wifiLabel();
  const String job = printer.filename.isEmpty() ? "No active file" : shorten(printer.filename, 39);
  const String elapsed = formatDuration(printer.printDuration);
  const String connection = printer.online ? "CONNECTED" : "DISCONNECTED";
  const int nozzleTemperature = lroundf(printer.nozzleTemperature);
  const int nozzleTarget = lroundf(printer.nozzleTarget);
  const int bedTemperature = lroundf(printer.bedTemperature);
  const int bedTarget = lroundf(printer.bedTarget);
  const int progressPercent = constrain(static_cast<int>(printer.progress * 100 + 0.5f), 0, 100);
  const uint16_t headerColor = stateColor();

  if (force || state != shown.state || wifi != shown.wifi || headerColor != shown.headerColor) {
    display.fillRect(0, 0, 320, 38, headerColor);
    const uint16_t headerText = headerColor == kRed ? kWhite : kBlack;
    display.setTextColor(headerText, headerColor);
    display.setTextDatum(ML_DATUM);
    display.drawString(state, 10, 19, 4);
    display.setTextDatum(MR_DATUM);
    display.drawString(wifi, 310, 19, 2);
    shown.state = state;
    shown.wifi = wifi;
    shown.headerColor = headerColor;
  }

  if (force || nozzleTemperature != shown.nozzleTemperature || nozzleTarget != shown.nozzleTarget) {
    drawTemperatureValue(8, nozzleTemperature, nozzleTarget);
    shown.nozzleTemperature = nozzleTemperature;
    shown.nozzleTarget = nozzleTarget;
  }

  if (force || bedTemperature != shown.bedTemperature || bedTarget != shown.bedTarget) {
    drawTemperatureValue(164, bedTemperature, bedTarget);
    shown.bedTemperature = bedTemperature;
    shown.bedTarget = bedTarget;
  }

  if (force || job != shown.job) {
    display.fillRect(12, 136, 296, 19, kBlack);
    display.setTextDatum(TL_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(job, 18, 140, 2);
    shown.job = job;
  }

  if (force || progressPercent != shown.progressPercent) {
    display.fillRect(11, 169, 298, 16, kBlack);
    const int fillWidth = (296 * progressPercent) / 100;
    if (fillWidth > 0) display.fillRect(12, 170, fillWidth, 14, kYellow);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", progressPercent);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(progressPercent >= 52 ? kBlack : kWhite);
    display.drawString(percent, 160, 177, 2);
    shown.progressPercent = progressPercent;
  }

  if (force || elapsed != shown.elapsed) {
    display.fillRect(8, 190, 105, 16, kBlack);
    display.setTextDatum(TL_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(elapsed, 10, 191, 2);
    shown.elapsed = elapsed;
  }

  if (force || connection != shown.connection) {
    display.fillRect(175, 190, 137, 16, kBlack);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(printer.online ? kYellow : kRed, kBlack);
    display.drawString(connection, 310, 191, 2);
    shown.connection = connection;
  }

  shown.initialized = true;
}

void drawPageTitle(const char *title) {
  display.fillScreen(kBlack);
  display.fillRect(0, 0, 320, 38, stateColor());
  const uint16_t background = stateColor();
  display.setTextColor(background == kRed ? kWhite : kBlack, background);
  display.setTextDatum(ML_DATUM);
  display.drawString(title, 10, 19, 4);
  drawNavigation();
}

void drawHealthLayout() {
  drawPageTitle("PRINTER HEALTH");
  display.setTextDatum(TL_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString("CONNECTION", 12, 50, 2);
  display.drawString("FILAMENT", 12, 91, 2);
  display.drawString("EDDY", 12, 148, 2);
  shownHealth = HealthSnapshot{};
}

void renderHealth(bool force = false) {
  const String connection = printer.online ? "Moonraker connected" : "Moonraker disconnected";
  const String wifi = wifiLabel();
  String filament = "Sensor data unavailable";
  if (printer.hasFilamentSensors) {
    filament = String("Present: ") + (printer.filamentPresent ? "YES" : "NO") +
               "     Motion: " + (printer.filamentMoving ? "YES" : "NO");
  }
  char eddy[40];
  snprintf(eddy, sizeof(eddy), "MCU temperature: %.1f C", printer.eddyTemperature);
  const String eddyText(eddy);
  const String message = shorten(printer.message, 42);
  const uint16_t headerColor = stateColor();

  if (force || headerColor != shownHealth.headerColor) {
    display.fillRect(0, 0, 320, 38, headerColor);
    display.setTextColor(headerColor == kRed ? kWhite : kBlack, headerColor);
    display.setTextDatum(ML_DATUM);
    display.drawString("PRINTER HEALTH", 10, 19, 4);
    shownHealth.headerColor = headerColor;
  }
  display.setTextDatum(TL_DATUM);
  if (force || connection != shownHealth.connection || wifi != shownHealth.wifi) {
    display.fillRect(10, 67, 300, 18, kBlack);
    display.setTextColor(kWhite, kBlack);
    display.drawString(connection, 12, 68, 2);
    display.setTextDatum(TR_DATUM);
    display.drawString(wifi, 308, 68, 2);
    display.setTextDatum(TL_DATUM);
    shownHealth.connection = connection;
    shownHealth.wifi = wifi;
  }
  if (force || filament != shownHealth.filament) {
    display.fillRect(10, 109, 300, 20, kBlack);
    display.setTextColor(printer.hasFilamentSensors && !printer.filamentPresent ? kRed : kWhite,
                         kBlack);
    display.drawString(filament, 12, 110, 2);
    shownHealth.filament = filament;
  }
  if (force || eddyText != shownHealth.eddy) {
    display.fillRect(10, 166, 300, 18, kBlack);
    display.setTextColor(kWhite, kBlack);
    display.drawString(eddyText, 12, 167, 2);
    shownHealth.eddy = eddyText;
  }
  if (force || message != shownHealth.message) {
    display.fillRect(10, 186, 300, 18, kBlack);
    if (!message.isEmpty()) {
      display.setTextColor(kRed, kBlack);
      display.drawString(message, 12, 188, 2);
    }
    shownHealth.message = message;
  }
}

void renderControls() {
  drawPageTitle("SAFE CONTROLS");
  display.setTextDatum(MC_DATUM);
  display.setTextColor(kWhite, kBlack);
  display.drawString("Controls will unlock after", 160, 82, 2);
  display.drawString("Moonraker command safety is enabled.", 160, 106, 2);
  display.setTextColor(kYellow, kBlack);
  display.drawString("No commands sent in this build", 160, 151, 2);
}

void renderAi() {
  drawPageTitle("AI ASSISTANT");
  display.setTextDatum(MC_DATUM);
  display.setTextColor(kWhite, kBlack);
  display.drawString("Cloud analysis gateway", 160, 80, 2);
  display.setTextColor(kYellow, kBlack);
  display.drawString("Not configured yet", 160, 111, 4);
  display.setTextColor(kWhite, kBlack);
  display.drawString("Advice only - never automatic control", 160, 155, 2);
}

void redrawActivePage() {
  shown = DisplaySnapshot{};
  switch (activePage) {
    case Page::Dashboard:
      drawStaticLayout();
      render(true);
      break;
    case Page::Health:
      drawHealthLayout();
      renderHealth(true);
      break;
    case Page::Controls:
      renderControls();
      break;
    case Page::Ai:
      renderAi();
      break;
  }
}

TS_Point waitForCalibrationTouch() {
  while (touch.touched()) delay(10);
  while (!touch.touched()) delay(10);
  int32_t x = 0;
  int32_t y = 0;
  int samples = 0;
  while (touch.touched() && samples < 40) {
    const TS_Point point = touch.getPoint();
    x += point.x;
    y += point.y;
    ++samples;
    delay(8);
  }
  return TS_Point(x / max(samples, 1), y / max(samples, 1), 1);
}

TS_Point captureCalibrationPoint(int x, int y, const char *label) {
  display.fillScreen(kBlack);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(kWhite, kBlack);
  display.drawString(label, 160, 110, 2);
  display.drawCircle(x, y, 10, kYellow);
  display.drawLine(x - 14, y, x + 14, y, kYellow);
  display.drawLine(x, y - 14, x, y + 14, kYellow);
  return waitForCalibrationTouch();
}

void calibrateTouch() {
  const TS_Point topLeft = captureCalibrationPoint(24, 24, "Touch the top-left target");
  delay(250);
  const TS_Point bottomRight = captureCalibrationPoint(296, 216, "Touch the bottom-right target");

  touchCalibration.left = topLeft.x;
  touchCalibration.right = bottomRight.x;
  touchCalibration.top = topLeft.y;
  touchCalibration.bottom = bottomRight.y;
  touchCalibration.valid = abs(touchCalibration.right - touchCalibration.left) > 500 &&
                           abs(touchCalibration.bottom - touchCalibration.top) > 500;
  if (!touchCalibration.valid) {
    display.fillScreen(kBlack);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(kRed, kBlack);
    display.drawString("Calibration failed - try again", 160, 120, 2);
    delay(1500);
    calibrateTouch();
    return;
  }

  preferences.begin("cyd-touch", false);
  preferences.putInt("left", touchCalibration.left);
  preferences.putInt("right", touchCalibration.right);
  preferences.putInt("top", touchCalibration.top);
  preferences.putInt("bottom", touchCalibration.bottom);
  preferences.putBool("valid", true);
  preferences.end();
}

void initializeTouch() {
  touchSpi.begin(kTouchClk, kTouchMiso, kTouchMosi, kTouchCs);
  touch.begin(touchSpi);
  touch.setRotation(1);

  preferences.begin("cyd-touch", true);
  touchCalibration.left = preferences.getInt("left", 0);
  touchCalibration.right = preferences.getInt("right", 0);
  touchCalibration.top = preferences.getInt("top", 0);
  touchCalibration.bottom = preferences.getInt("bottom", 0);
  touchCalibration.valid = preferences.getBool("valid", false);
  preferences.end();

  // Holding the screen during boot deliberately starts calibration again.
  const unsigned long started = millis();
  bool held = false;
  while (millis() - started < 1200) {
    if (touch.touched()) held = true;
    delay(10);
  }
  if (!touchCalibration.valid || held) calibrateTouch();
}

bool readTouch(int &screenX, int &screenY) {
  if (!touchCalibration.valid || !touch.touched()) return false;
  const TS_Point point = touch.getPoint();
  const float xScale = 272.0f / static_cast<float>(touchCalibration.right - touchCalibration.left);
  const float yScale = 192.0f / static_cast<float>(touchCalibration.bottom - touchCalibration.top);
  screenX = constrain(static_cast<int>(24 + (point.x - touchCalibration.left) * xScale), 0, 319);
  screenY = constrain(static_cast<int>(24 + (point.y - touchCalibration.top) * yScale), 0, 239);
  return true;
}

void handleTouch() {
  static bool wasTouched = false;
  int x = 0;
  int y = 0;
  const bool isTouched = readTouch(x, y);
  if (isTouched) {
    lastInteractionAt = millis();
    setBacklight(kBacklightBright);
  }
  if (isTouched && !wasTouched && y >= kNavigationTop) {
    const Page selected = static_cast<Page>(constrain(x / 80, 0, 3));
    if (selected != activePage) {
      activePage = selected;
      redrawActivePage();
    }
  }
  wasTouched = isTouched;
}

void updateBacklight() {
  const unsigned long idleFor = millis() - lastInteractionAt;
  if (idleFor >= kSleepAfterMs) {
    setBacklight(0);
  } else if (idleFor >= kDimAfterMs) {
    setBacklight(kBacklightDim);
  }
}

void renderSetupPortal() {
  display.fillScreen(kBlack);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString("WI-FI SETUP", 160, 48, 4);
  display.setTextColor(kWhite, kBlack);
  display.drawString("Join this network:", 160, 94, 2);
  display.drawString("Printer-Display-Setup", 160, 119, 4);
  display.setTextColor(kYellow, kBlack);
  display.drawString("Then open 192.168.4.1", 160, 158, 2);
  display.drawString("and select your Wi-Fi", 160, 181, 2);
}

void connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setHostname("printer-display");
  WiFi.setAutoReconnect(true);

  WiFiManager manager;
  manager.setConnectTimeout(20);
  manager.setConfigPortalTimeout(300);
  manager.setAPCallback([](WiFiManager *) { renderSetupPortal(); });

  if (!manager.autoConnect("Printer-Display-Setup")) {
    display.fillScreen(kBlack);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(kRed, kBlack);
    display.drawString("SETUP TIMED OUT", 160, 105, 4);
    display.setTextColor(kWhite, kBlack);
    display.drawString("Restart to try again", 160, 142, 2);
    delay(3000);
    ESP.restart();
  }

  Serial.print("Wi-Fi connected, address: ");
  Serial.println(WiFi.localIP());
}

bool pollMoonraker() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + MOONRAKER_HOST + ":" + MOONRAKER_PORT +
      "/printer/objects/query?webhooks&print_stats&virtual_sdcard&"
      "extruder=temperature,target&heater_bed=temperature,target&display_status=progress,message&"
      "filament_switch_sensor%20sfs_switch=filament_detected&"
      "filament_motion_sensor%20sfs_motion=filament_detected&"
      "temperature_sensor%20btt_eddy_mcu=temperature";

  http.setConnectTimeout(1200);
  http.setTimeout(1200);
  if (!http.begin(client, endpoint)) return false;
  if (strlen(MOONRAKER_API_KEY) > 0) http.addHeader("X-Api-Key", MOONRAKER_API_KEY);

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) return false;

  JsonObject status = document["result"]["status"];
  if (status.isNull()) return false;

  printer.online = true;
  printer.klippyState = status["webhooks"]["state"] | "unknown";
  printer.printState = status["print_stats"]["state"] | "standby";
  printer.filename = String(status["print_stats"]["filename"] | "");
  printer.printDuration = status["print_stats"]["print_duration"] | 0.0f;
  printer.nozzleTemperature = status["extruder"]["temperature"] | 0.0f;
  printer.nozzleTarget = status["extruder"]["target"] | 0.0f;
  printer.bedTemperature = status["heater_bed"]["temperature"] | 0.0f;
  printer.bedTarget = status["heater_bed"]["target"] | 0.0f;
  printer.progress = status["virtual_sdcard"]["progress"] | 0.0f;
  printer.message = String(status["display_status"]["message"] | "");
  const JsonObject switchSensor = status["filament_switch_sensor sfs_switch"];
  const JsonObject motionSensor = status["filament_motion_sensor sfs_motion"];
  printer.hasFilamentSensors = !switchSensor.isNull() || !motionSensor.isNull();
  printer.filamentPresent = switchSensor["filament_detected"] | false;
  printer.filamentMoving = motionSensor["filament_detected"] | false;
  printer.eddyTemperature = status["temperature_sensor btt_eddy_mcu"]["temperature"] | 0.0f;
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  ledcSetup(kBacklightChannel, 5000, 8);
  ledcAttachPin(TFT_BL, kBacklightChannel);
  setBacklight(kBacklightBright);
  pinMode(kLedRed, OUTPUT);
  pinMode(kLedGreen, OUTPUT);
  pinMode(kLedBlue, OUTPUT);
  setRgb(false, false, false);

  display.init();
  display.setRotation(1);
  // This CYD panel requires the ILI9341 inversion command for normal colors.
  // Calling the runtime API sends the command twice for panels that ignore the
  // first command immediately after initialization.
  display.invertDisplay(true);
  display.setTextWrap(false);
  initializeTouch();
  lastInteractionAt = millis();
  drawStaticLayout();
  render(true);

  connectWifi();
  drawStaticLayout();
  render(true);
}

void loop() {
  const unsigned long now = millis();

  if (WiFi.status() != WL_CONNECTED && now - lastWifiAttemptAt >= kWifiRetryMs) {
    lastWifiAttemptAt = now;
    WiFi.reconnect();
    printer.online = false;
    if (activePage == Page::Dashboard) render();
    updateRgb();
  }

  if (now - lastPollAt >= kPollIntervalMs) {
    lastPollAt = now;
    if (!pollMoonraker()) printer.online = false;
    if (activePage == Page::Dashboard) {
      render();
    } else if (activePage == Page::Health) {
      renderHealth();
    }
    updateRgb();
  }

  handleTouch();
  updateBacklight();

  delay(20);
}

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "config.h"

namespace {

constexpr uint16_t kBlack = TFT_BLACK;
constexpr uint16_t kWhite = TFT_WHITE;
constexpr uint16_t kRed = TFT_RED;
constexpr uint16_t kYellow = TFT_YELLOW;
constexpr unsigned long kPollIntervalMs = 1500;
constexpr unsigned long kWifiRetryMs = 10000;

TFT_eSPI display;
unsigned long lastPollAt = 0;
unsigned long lastWifiAttemptAt = 0;

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

  display.setTextColor(kYellow, kBlack);
  display.drawString("ELAPSED", 10, 199, 2);
  display.setTextDatum(TR_DATUM);
  display.drawString("MOONRAKER", 310, 199, 2);

  shown = DisplaySnapshot{};
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
    display.fillRect(8, 214, 105, 23, kBlack);
    display.setTextDatum(TL_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(elapsed, 10, 217, 2);
    shown.elapsed = elapsed;
  }

  if (force || connection != shown.connection) {
    display.fillRect(175, 214, 137, 23, kBlack);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(printer.online ? kYellow : kRed, kBlack);
    display.drawString(connection, 310, 217, 2);
    shown.connection = connection;
  }

  shown.initialized = true;
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
      "extruder=temperature,target&heater_bed=temperature,target&display_status=progress,message";

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
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  display.init();
  display.setRotation(1);
  // This CYD panel requires the ILI9341 inversion command for normal colors.
  // Calling the runtime API sends the command twice for panels that ignore the
  // first command immediately after initialization.
  display.invertDisplay(true);
  display.setTextWrap(false);
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
    render();
  }

  if (now - lastPollAt >= kPollIntervalMs) {
    lastPollAt = now;
    if (!pollMoonraker()) printer.online = false;
    render();
  }

  delay(20);
}

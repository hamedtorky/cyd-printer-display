#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebSocketsClient.h>
#include <XPT2046_Touchscreen.h>
#include <mbedtls/sha256.h>

#include "config.h"
#include "firmware_version.h"

#ifndef AI_GATEWAY_HOST
#define AI_GATEWAY_HOST "printer.lan"
#endif
#ifndef AI_GATEWAY_PORT
#define AI_GATEWAY_PORT 8787
#endif
#ifndef AI_GATEWAY_TOKEN
#define AI_GATEWAY_TOKEN ""
#endif

namespace {

constexpr uint16_t kBlack = TFT_BLACK;
constexpr uint16_t kWhite = TFT_WHITE;
constexpr uint16_t kRed = TFT_RED;
constexpr uint16_t kYellow = TFT_YELLOW;
constexpr unsigned long kFallbackPollIntervalMs = 2000;
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
constexpr unsigned long kAiPollIntervalMs = 5000;
constexpr unsigned long kPreviewRetryMs = 10000;
constexpr uint16_t kModelPreviewWidth = 220;
constexpr uint16_t kModelPreviewHeight = 145;
constexpr size_t kModelPreviewBytes = kModelPreviewWidth * kModelPreviewHeight * 2;

TFT_eSPI display;
SPIClass touchSpi(VSPI);
XPT2046_Touchscreen touch(kTouchCs, kTouchIrq);
WebSocketsClient moonrakerSocket;
Preferences preferences;
unsigned long lastPollAt = 0;
unsigned long lastWifiAttemptAt = 0;
unsigned long lastInteractionAt = 0;
unsigned long lastAiPollAt = 0;
bool socketConnected = false;
String socketHeaders;
uint16_t modelPreviewPixels[kModelPreviewWidth * kModelPreviewHeight];
bool modelPreviewReady = false;
String modelPreviewFilename;
String modelPreviewError;
unsigned long lastPreviewAttemptAt = 0;
bool otaBootCheckDone = false;

enum class Page : uint8_t { Dashboard, Model, Health, Toolhead, Extruder, Ai };
Page activePage = Page::Dashboard;

enum class OtaState : uint8_t { Unchecked, Checking, Current, Available, Failed };

struct OtaInfo {
  OtaState state = OtaState::Unchecked;
  String version;
  String sha256;
  String message;
  size_t size = 0;
};

OtaInfo ota;
bool otaHoldArmed = false;
unsigned long otaHoldStartedAt = 0;

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
  float positionX = 0;
  float positionY = 0;
  float positionZ = 0;
  float zOffset = 0;
  float pressureAdvance = 0;
  float smoothTime = 0;
  String homedAxes;
  int speedPercent = 100;
  int flowPercent = 100;
};

PrinterStatus printer;

struct DisplaySnapshot {
  bool initialized = false;
  String state;
  String wifi;
  String job;
  String remaining;
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
  String ota;
  uint16_t headerColor = 0;
};

HealthSnapshot shownHealth;

struct ToolheadSnapshot {
  String position;
  String homedAxes;
  String zOffset;
  String message;
  int speedPercent = -1;
};

ToolheadSnapshot shownToolhead;
String toolheadMessage;

struct ExtruderSnapshot {
  String temperature;
  String pressureAdvance;
  String smoothTime;
  String message;
  int flowPercent = -1;
};

ExtruderSnapshot shownExtruder;
String extruderMessage;

struct ModelSnapshot {
  String filename;
  String state;
  int progressPercent = -1;
};

ModelSnapshot shownModel;

struct AiAssessment {
  bool gatewayOnline = false;
  bool available = false;
  bool analyzing = false;
  String status = "unknown";
  String summary;
  String recommendation;
  String analyzedAt;
  int confidencePercent = 0;
};

struct AiSnapshot {
  bool gatewayOnline = false;
  bool available = false;
  bool analyzing = false;
  String status;
  String summary;
  String recommendation;
  int confidencePercent = -1;
};

AiAssessment aiAssessment;
AiSnapshot shownAi;

bool postMoonraker(const String &path);
bool requestAiAnalysis();
bool fetchModelPreview();
bool printerIdleForOta();
bool checkForOtaUpdate();
bool performOtaUpdate();

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

String estimatedRemaining() {
  if (printer.progress < 0.005f || printer.printDuration <= 0) return "--:--";
  const float estimatedTotal = printer.printDuration / printer.progress;
  return formatDuration(max(0.0f, estimatedTotal - printer.printDuration));
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
  const bool filamentAlert = printer.printState == "printing" && printer.hasFilamentSensors &&
                             (!printer.filamentPresent || !printer.filamentMoving);
  if (!printer.online || printer.klippyState != "ready" || printer.printState == "error" ||
      filamentAlert) {
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
  static const char *labels[] = {"HOME", "MODEL", "HEALTH", "TOOL", "EXT", "AI"};
  display.fillRect(0, kNavigationTop, 320, 33, kBlack);
  for (int i = 0; i < 6; ++i) {
    const bool selected = static_cast<int>(activePage) == i;
    const int x = (i * 320) / 6;
    const int nextX = ((i + 1) * 320) / 6;
    const int width = nextX - x;
    display.fillRect(x + 1, kNavigationTop + 1, width - 2, 31, selected ? kYellow : kBlack);
    display.drawRect(x, kNavigationTop, width, 33, selected ? kYellow : kWhite);
    display.setTextColor(selected ? kBlack : kWhite, selected ? kYellow : kBlack);
    display.setTextDatum(MC_DATUM);
    display.drawString(labels[i], x + width / 2, kNavigationTop + 16, 1);
  }
}

void drawModelPreview(int x, int y) {
  display.fillRect(x, y, kModelPreviewWidth, kModelPreviewHeight, kBlack);
  if (modelPreviewReady) {
    display.pushImage(x, y, kModelPreviewWidth, kModelPreviewHeight, modelPreviewPixels);
    return;
  }
  display.setTextDatum(MC_DATUM);
  display.setTextColor(modelPreviewError.isEmpty() ? kYellow : kRed, kBlack);
  display.drawString(modelPreviewError.isEmpty() ? "LOADING MODEL" : "PREVIEW RETRY", x + 110,
                     y + 67, 2);
}

void drawStaticLayout() {
  display.fillScreen(kBlack);

  // HOME uses 70% of the content width for the model and 30% for live status.
  display.drawRect(3, 41, kModelPreviewWidth + 2, kModelPreviewHeight + 2, kYellow);
  drawModelPreview(4, 42);
  display.drawRoundRect(228, 41, 89, 163, 6, kWhite);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString("NOZZLE", 234, 47, 2);
  display.drawString("BED", 234, 78, 2);
  display.drawString("PROGRESS", 234, 109, 2);
  display.drawString("TIME LEFT", 234, 158, 2);

  display.drawRect(234, 140, 76, 10, kWhite);

  shown = DisplaySnapshot{};
  drawNavigation();
}

void drawCompactTemperature(int y, int actual, int target) {
  display.fillRect(233, y, 79, 17, kBlack);
  char value[24];
  snprintf(value, sizeof(value), "%d/%d C", actual, target);
  display.setTextDatum(TR_DATUM);
  display.setTextColor(kWhite, kBlack);
  display.drawString(value, 310, y, 2);
}

void render(bool force = false) {
  const String state = shorten(visibleState(), 24);
  const String wifi = wifiLabel();
  const String job = printer.filename.isEmpty() ? "No active file" : shorten(printer.filename, 29);
  const String remaining = estimatedRemaining();
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
    drawCompactTemperature(62, nozzleTemperature, nozzleTarget);
    shown.nozzleTemperature = nozzleTemperature;
    shown.nozzleTarget = nozzleTarget;
  }

  if (force || bedTemperature != shown.bedTemperature || bedTarget != shown.bedTarget) {
    drawCompactTemperature(93, bedTemperature, bedTarget);
    shown.bedTemperature = bedTemperature;
    shown.bedTarget = bedTarget;
  }

  if (force || job != shown.job) {
    display.fillRect(3, 189, 221, 16, kBlack);
    display.setTextDatum(TL_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(job, 4, 190, 2);
    shown.job = job;
  }

  if (force || progressPercent != shown.progressPercent) {
    display.fillRect(233, 124, 79, 15, kBlack);
    display.fillRect(235, 141, 74, 8, kBlack);
    const int fillWidth = (74 * progressPercent) / 100;
    if (fillWidth > 0) display.fillRect(235, 141, fillWidth, 8, kYellow);
    char percent[8];
    snprintf(percent, sizeof(percent), "%d%%", progressPercent);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(percent, 310, 124, 2);
    shown.progressPercent = progressPercent;
  }

  if (force || remaining != shown.remaining) {
    display.fillRect(233, 175, 79, 24, kBlack);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(printer.printState == "printing" ? kYellow : kWhite, kBlack);
    display.drawString(remaining, 310, 176, 4);
    shown.remaining = remaining;
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
  display.drawString("CONNECTION", 12, 44, 2);
  display.drawString("FILAMENT", 12, 82, 2);
  display.drawString("EDDY", 12, 120, 2);
  display.drawRoundRect(10, 162, 300, 37, 6, kYellow);
  shownHealth = HealthSnapshot{};
}

void drawModelLayout() {
  drawPageTitle("PRINT MODEL");
  display.drawRect(49, 41, kModelPreviewWidth + 2, kModelPreviewHeight + 2, kYellow);
  drawModelPreview(50, 42);
  shownModel = ModelSnapshot{};
}

void renderModel(bool force = false) {
  const String filename = printer.filename.isEmpty() ? "Current G-code preview"
                                                      : shorten(printer.filename, 31);
  const int progressPercent = constrain(static_cast<int>(printer.progress * 100 + 0.5f), 0, 100);
  const String state = printer.printState;
  if (!force && filename == shownModel.filename && progressPercent == shownModel.progressPercent &&
      state == shownModel.state) {
    return;
  }
  display.fillRect(6, 190, 308, 16, kBlack);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(kWhite, kBlack);
  display.drawString(filename, 8, 191, 2);
  display.setTextDatum(TR_DATUM);
  display.setTextColor(state == "printing" ? kYellow : kWhite, kBlack);
  display.drawString(String(progressPercent) + "%", 312, 191, 2);
  shownModel.filename = filename;
  shownModel.progressPercent = progressPercent;
  shownModel.state = state;
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
  const bool activePrint = printer.printState == "printing" || printer.printState == "paused";
  String otaText;
  uint16_t otaColor = kYellow;
  if (otaHoldArmed) {
    otaText = "KEEP HOLDING TO UPDATE";
  } else if (ota.state == OtaState::Checking) {
    otaText = "CHECKING FOR UPDATE...";
  } else if (ota.state == OtaState::Current) {
    otaText = String("UP TO DATE  v") + CYD_FIRMWARE_VERSION;
    otaColor = kWhite;
  } else if (ota.state == OtaState::Available) {
    otaText = activePrint ? "UPDATE LOCKED DURING PRINT" : String("HOLD TO INSTALL  v") + ota.version;
    otaColor = activePrint ? kRed : kYellow;
  } else if (ota.state == OtaState::Failed) {
    otaText = String("RETRY UPDATE: ") + shorten(ota.message, 20);
    otaColor = kRed;
  } else {
    otaText = String("CHECK UPDATE  v") + CYD_FIRMWARE_VERSION;
  }
  const String otaKey = String(static_cast<int>(ota.state)) + "|" + otaText;
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
    display.fillRect(10, 61, 300, 18, kBlack);
    display.setTextColor(kWhite, kBlack);
    display.drawString(connection, 12, 62, 2);
    display.setTextDatum(TR_DATUM);
    display.drawString(wifi, 308, 62, 2);
    display.setTextDatum(TL_DATUM);
    shownHealth.connection = connection;
    shownHealth.wifi = wifi;
  }
  if (force || filament != shownHealth.filament) {
    display.fillRect(10, 99, 300, 18, kBlack);
    display.setTextColor(printer.hasFilamentSensors && !printer.filamentPresent ? kRed : kWhite,
                         kBlack);
    display.drawString(filament, 12, 100, 2);
    shownHealth.filament = filament;
  }
  if (force || eddyText != shownHealth.eddy) {
    display.fillRect(10, 137, 300, 18, kBlack);
    display.setTextColor(kWhite, kBlack);
    display.drawString(eddyText, 12, 138, 2);
    shownHealth.eddy = eddyText;
  }
  if (force || otaKey != shownHealth.ota) {
    display.fillRect(12, 164, 296, 33, kBlack);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(otaColor, kBlack);
    display.drawString(otaText, 160, 180, 2);
    shownHealth.ota = otaKey;
  }
}

void drawButton(int x, int y, int width, int height, const String &label,
                uint16_t color = kWhite) {
  display.drawRoundRect(x, y, width, height, 5, color);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(color, kBlack);
  display.drawString(label, x + width / 2, y + height / 2, 2);
}

void drawToolheadLayout() {
  drawPageTitle("TOOLHEAD");
  for (int i = 0; i < 3; ++i) display.drawRoundRect(6 + i * 103, 43, 96, 33, 5, kWhite);
  display.setTextDatum(TL_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString("X", 12, 47, 1);
  display.drawString("Y", 115, 47, 1);
  display.drawString("Z", 218, 47, 1);

  drawButton(8, 82, 71, 29, "HOME", kYellow);
  drawButton(84, 82, 55, 29, "S-");
  display.drawRoundRect(144, 82, 95, 29, 5, kWhite);
  drawButton(244, 82, 68, 29, "S+");
  drawButton(8, 117, 70, 29, "X-");
  drawButton(86, 117, 70, 29, "X+");
  drawButton(164, 117, 70, 29, "Y-");
  drawButton(242, 117, 70, 29, "Y+");
  drawButton(8, 152, 70, 29, "Z-");
  drawButton(86, 152, 70, 29, "Z+");
  drawButton(164, 152, 70, 29, "OFF-");
  drawButton(242, 152, 70, 29, "OFF+");
  shownToolhead = ToolheadSnapshot{};
}

void renderToolhead(bool force = false) {
  char position[72];
  snprintf(position, sizeof(position), "%.2f|%.2f|%.3f", printer.positionX, printer.positionY,
           printer.positionZ);
  const String positionText(position);
  char offset[20];
  snprintf(offset, sizeof(offset), "Z %.3f", printer.zOffset);
  const String offsetText(offset);

  if (force || positionText != shownToolhead.position) {
    display.setTextDatum(TR_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.fillRect(25, 46, 73, 25, kBlack);
    display.fillRect(128, 46, 73, 25, kBlack);
    display.fillRect(231, 46, 73, 25, kBlack);
    display.drawString(String(printer.positionX, 2), 96, 55, 2);
    display.drawString(String(printer.positionY, 2), 199, 55, 2);
    display.drawString(String(printer.positionZ, 3), 302, 55, 2);
    shownToolhead.position = positionText;
  }
  if (force || printer.speedPercent != shownToolhead.speedPercent) {
    display.fillRect(146, 84, 91, 25, kBlack);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(String(printer.speedPercent) + "%", 191, 96, 2);
    shownToolhead.speedPercent = printer.speedPercent;
  }
  if (force || offsetText != shownToolhead.zOffset) {
    display.fillRect(10, 184, 145, 19, kBlack);
    display.setTextDatum(TL_DATUM);
    display.setTextColor(kYellow, kBlack);
    display.drawString(offsetText, 10, 188, 2);
    shownToolhead.zOffset = offsetText;
  }
  if (force || toolheadMessage != shownToolhead.message) {
    display.fillRect(158, 184, 154, 19, kBlack);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(toolheadMessage.startsWith("Failed") || toolheadMessage == "Movement locked"
                             ? kRed
                             : kWhite,
                         kBlack);
    display.drawString(shorten(toolheadMessage, 22), 310, 188, 1);
    shownToolhead.message = toolheadMessage;
  }
  shownToolhead.homedAxes = printer.homedAxes;
}

void drawExtruderLayout() {
  drawPageTitle("EXTRUDER");
  display.setTextDatum(TL_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString("NOZZLE", 12, 47, 2);
  display.drawString("FLOW", 12, 78, 2);
  drawButton(91, 70, 50, 34, "-");
  display.drawRoundRect(147, 70, 107, 34, 5, kWhite);
  drawButton(260, 70, 50, 34, "+");
  display.drawRoundRect(10, 112, 145, 35, 5, kWhite);
  display.drawRoundRect(165, 112, 145, 35, 5, kWhite);
  display.setTextColor(kYellow, kBlack);
  display.drawString("PRESSURE ADV", 16, 115, 1);
  display.drawString("SMOOTH TIME", 171, 115, 1);
  drawButton(10, 168, 145, 31, "RETRACT", kYellow);
  drawButton(165, 168, 145, 31, "EXTRUDE", kYellow);
  shownExtruder = ExtruderSnapshot{};
}

void renderExtruder(bool force = false) {
  char temperature[24];
  snprintf(temperature, sizeof(temperature), "%d/%d C", lroundf(printer.nozzleTemperature),
           lroundf(printer.nozzleTarget));
  const String temperatureText(temperature);
  const String pressureText(printer.pressureAdvance, 3);
  const String smoothText(printer.smoothTime, 3);

  if (force || temperatureText != shownExtruder.temperature) {
    display.fillRect(90, 43, 220, 24, kBlack);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(temperatureText, 308, 47, 2);
    shownExtruder.temperature = temperatureText;
  }
  if (force || printer.flowPercent != shownExtruder.flowPercent) {
    display.fillRect(149, 72, 103, 30, kBlack);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(String(printer.flowPercent) + "%", 200, 87, 4);
    shownExtruder.flowPercent = printer.flowPercent;
  }
  if (force || pressureText != shownExtruder.pressureAdvance) {
    display.fillRect(15, 128, 135, 16, kBlack);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(pressureText + " s", 148, 129, 2);
    shownExtruder.pressureAdvance = pressureText;
  }
  if (force || smoothText != shownExtruder.smoothTime) {
    display.fillRect(170, 128, 135, 16, kBlack);
    display.setTextDatum(TR_DATUM);
    display.setTextColor(kWhite, kBlack);
    display.drawString(smoothText + " s", 303, 129, 2);
    shownExtruder.smoothTime = smoothText;
  }
  if (force || extruderMessage != shownExtruder.message) {
    display.fillRect(10, 150, 300, 16, kBlack);
    display.setTextDatum(MC_DATUM);
    display.setTextColor(extruderMessage.startsWith("Failed") || extruderMessage.endsWith("locked")
                             ? kRed
                             : kWhite,
                         kBlack);
    const String message = extruderMessage.isEmpty() ? "25 mm  @  10 mm/s" : extruderMessage;
    display.drawString(shorten(message, 42), 160, 157, 2);
    shownExtruder.message = extruderMessage;
  }
}

void drawAiLayout() {
  drawPageTitle("AI ASSISTANT");
  display.drawRoundRect(12, 163, 296, 36, 7, kYellow);
  shownAi = AiSnapshot{};
}

void drawWrappedText(const String &text, int x, int y, int widthChars, int maxLines,
                     uint16_t color) {
  String remaining = text;
  display.setTextDatum(TL_DATUM);
  display.setTextColor(color, kBlack);
  for (int line = 0; line < maxLines && !remaining.isEmpty(); ++line) {
    int take = min(static_cast<int>(remaining.length()), widthChars);
    if (take < static_cast<int>(remaining.length())) {
      const int space = remaining.substring(0, take + 1).lastIndexOf(' ');
      if (space > widthChars / 2) take = space;
    }
    display.drawString(remaining.substring(0, take), x, y + line * 18, 2);
    remaining = remaining.substring(take);
    remaining.trim();
  }
}

void renderAi(bool force = false) {
  const bool changed = force || aiAssessment.gatewayOnline != shownAi.gatewayOnline ||
                       aiAssessment.available != shownAi.available ||
                       aiAssessment.analyzing != shownAi.analyzing ||
                       aiAssessment.status != shownAi.status ||
                       aiAssessment.summary != shownAi.summary ||
                       aiAssessment.recommendation != shownAi.recommendation ||
                       aiAssessment.confidencePercent != shownAi.confidencePercent;
  if (!changed) return;

  display.fillRect(10, 46, 300, 112, kBlack);
  display.setTextDatum(MC_DATUM);
  if (!aiAssessment.gatewayOnline) {
    display.setTextColor(kRed, kBlack);
    display.drawString("GATEWAY OFFLINE", 160, 78, 4);
    display.setTextColor(kWhite, kBlack);
    display.drawString("Start the service on the CM4", 160, 111, 2);
  } else if (aiAssessment.analyzing) {
    display.setTextColor(kYellow, kBlack);
    display.drawString("ANALYZING...", 160, 82, 4);
    display.setTextColor(kWhite, kBlack);
    display.drawString("The display remains responsive", 160, 119, 2);
  } else if (!aiAssessment.available) {
    display.setTextColor(kWhite, kBlack);
    display.drawString("No assessment yet", 160, 78, 4);
    display.drawString("Tap below to inspect the print", 160, 117, 2);
  } else {
    const uint16_t statusColor = aiAssessment.status == "critical" ? kRed
                                 : aiAssessment.status == "warning" ? kYellow
                                                                     : kWhite;
    display.setTextColor(statusColor, kBlack);
    display.drawString(aiAssessment.status + "  " + aiAssessment.confidencePercent + "%", 160,
                       57, 4);
    drawWrappedText(aiAssessment.summary, 14, 81, 45, 2, kWhite);
    drawWrappedText(aiAssessment.recommendation, 14, 119, 45, 2, kYellow);
  }
  display.fillRect(14, 165, 292, 32, kBlack);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString(aiAssessment.analyzing ? "ANALYSIS RUNNING" : "ANALYZE NOW", 160, 181, 2);
  shownAi.gatewayOnline = aiAssessment.gatewayOnline;
  shownAi.available = aiAssessment.available;
  shownAi.analyzing = aiAssessment.analyzing;
  shownAi.status = aiAssessment.status;
  shownAi.summary = aiAssessment.summary;
  shownAi.recommendation = aiAssessment.recommendation;
  shownAi.confidencePercent = aiAssessment.confidencePercent;
}

void redrawActivePage() {
  shown = DisplaySnapshot{};
  switch (activePage) {
    case Page::Dashboard:
      drawStaticLayout();
      render(true);
      break;
    case Page::Model:
      drawModelLayout();
      renderModel(true);
      break;
    case Page::Health:
      drawHealthLayout();
      renderHealth(true);
      break;
    case Page::Toolhead:
      drawToolheadLayout();
      renderToolhead(true);
      break;
    case Page::Extruder:
      drawExtruderLayout();
      renderExtruder(true);
      break;
    case Page::Ai:
      drawAiLayout();
      renderAi(true);
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
  const bool wasSleeping = millis() - lastInteractionAt >= kSleepAfterMs;
  if (isTouched) {
    lastInteractionAt = millis();
    setBacklight(kBacklightBright);
  }
  if (isTouched && !wasTouched && !wasSleeping && y >= kNavigationTop) {
    const Page selected = static_cast<Page>(constrain((x * 6) / 320, 0, 5));
    if (selected != activePage) {
      activePage = selected;
      if (activePage == Page::Ai) lastAiPollAt = 0;
      otaHoldArmed = false;
      redrawActivePage();
    }
  }
  if (!isTouched && otaHoldArmed) {
    otaHoldArmed = false;
    if (activePage == Page::Health) renderHealth();
  }
  if (activePage == Page::Health && isTouched && !wasSleeping && y >= 162 && y <= 199) {
    if (!wasTouched) {
      if (ota.state == OtaState::Available) {
        if (printerIdleForOta()) {
          otaHoldArmed = true;
          otaHoldStartedAt = millis();
        }
      } else {
        checkForOtaUpdate();
      }
      renderHealth();
    }
    if (otaHoldArmed && millis() - otaHoldStartedAt >= 2000) performOtaUpdate();
  }
  if (activePage == Page::Toolhead && isTouched && !wasTouched && !wasSleeping &&
      y < kNavigationTop) {
    const bool activePrint = printer.printState == "printing" || printer.printState == "paused";
    if (x >= 8 && x <= 79 && y >= 82 && y <= 111) {
      const bool ok = !activePrint && postMoonraker("/printer/gcode/script?script=G28");
      toolheadMessage = activePrint ? "Movement locked" : ok ? "Homing requested" : "Failed to home";
    } else if (y >= 82 && y <= 111 && (x >= 84 && x <= 139 || x >= 244 && x <= 312)) {
      const int delta = x < 200 ? -10 : 10;
      const int requested = constrain(printer.speedPercent + delta, 50, 200);
      const bool ok = printer.printState == "printing" &&
                      postMoonraker(String("/printer/gcode/script?script=M220%20S") + requested);
      toolheadMessage = printer.printState != "printing" ? "Speed needs print"
                            : ok                           ? String("Speed ") + requested + "%"
                                                           : "Failed speed";
    } else if (y >= 117 && y <= 181) {
      String axis;
      float amount = 0;
      if (y <= 146 && x >= 8 && x <= 78) axis = "X", amount = -1;
      else if (y <= 146 && x >= 86 && x <= 156) axis = "X", amount = 1;
      else if (y <= 146 && x >= 164 && x <= 234) axis = "Y", amount = -1;
      else if (y <= 146 && x >= 242 && x <= 312) axis = "Y", amount = 1;
      else if (y >= 152 && x >= 8 && x <= 78) axis = "Z", amount = -1;
      else if (y >= 152 && x >= 86 && x <= 156) axis = "Z", amount = 1;

      if (!axis.isEmpty()) {
        const char lowerAxis = axis[0] == 'X' ? 'x' : axis[0] == 'Y' ? 'y' : 'z';
        const bool homed = printer.homedAxes.indexOf(lowerAxis) >= 0;
        const String amountText = amount < 0 ? "-1" : "1";
        const String path = String("/printer/gcode/script?script=SAVE_GCODE_STATE%20NAME=CYD_JOG%0A") +
                            "G91%0AG0%20" + axis + amountText + "%20F1800%0A" +
                            "RESTORE_GCODE_STATE%20NAME=CYD_JOG";
        const bool ok = !activePrint && homed && postMoonraker(path);
        toolheadMessage = activePrint ? "Movement locked"
                          : !homed     ? axis + " not homed"
                          : ok         ? axis + amountText + " mm"
                                       : "Failed to jog";
      } else if (y >= 152 && (x >= 164 && x <= 234 || x >= 242 && x <= 312)) {
        const float adjustment = x < 240 ? -0.025f : 0.025f;
        const bool homed = printer.homedAxes.indexOf('z') >= 0;
        const bool inRange = fabsf(printer.zOffset + adjustment) <= 2.0f;
        const String adjustmentText = adjustment < 0 ? "-0.025" : "0.025";
        const String path = String("/printer/gcode/script?script=SET_GCODE_OFFSET%20Z_ADJUST=") +
                            adjustmentText + "%20MOVE=1";
        const bool ok = homed && inRange && postMoonraker(path);
        toolheadMessage = !homed   ? "Z not homed"
                          : !inRange ? "Offset limit"
                          : ok       ? String("Offset ") + adjustmentText
                                     : "Failed offset";
      }
    }
    renderToolhead();
  }
  if (activePage == Page::Extruder && isTouched && !wasTouched && !wasSleeping &&
      y < kNavigationTop) {
    if (y >= 70 && y <= 104 && (x >= 91 && x <= 141 || x >= 260 && x <= 310)) {
      const int delta = x < 200 ? -5 : 5;
      const int requested = constrain(printer.flowPercent + delta, 50, 150);
      const bool ok = printer.printState == "printing" &&
                      postMoonraker(String("/printer/gcode/script?script=M221%20S") + requested);
      extruderMessage = printer.printState != "printing" ? "Flow needs active print"
                          : ok ? String("Flow ") + requested + "%" : "Failed flow";
    } else if (y >= 168 && y <= 199 && (x >= 10 && x <= 155 || x >= 165 && x <= 310)) {
      const bool activePrint = printer.printState == "printing" || printer.printState == "paused";
      const bool hot = printer.nozzleTemperature >= 170.0f;
      const bool extrude = x >= 165;
      const String amount = extrude ? "25" : "-25";
      const String path = String("/printer/gcode/script?script=SAVE_GCODE_STATE%20NAME=CYD_EXT%0A") +
                          "M83%0AG1%20E" + amount + "%20F600%0A" +
                          "RESTORE_GCODE_STATE%20NAME=CYD_EXT";
      const bool ok = !activePrint && hot && postMoonraker(path);
      extruderMessage = activePrint ? "Manual extrusion locked"
                        : !hot       ? "Heat nozzle to 170 C"
                        : ok         ? (extrude ? "Extruding 25 mm" : "Retracting 25 mm")
                                     : "Failed extrusion";
    }
    renderExtruder();
  }
  if (activePage == Page::Ai && isTouched && !wasTouched && !wasSleeping && y >= 163 && y <= 199) {
    if (!aiAssessment.analyzing) {
      aiAssessment.analyzing = true;
      renderAi();
      if (!requestAiAnalysis()) {
        aiAssessment.analyzing = false;
        renderAi();
      }
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

void refreshActivePage() {
  switch (activePage) {
    case Page::Dashboard:
      render();
      break;
    case Page::Model:
      renderModel();
      break;
    case Page::Health:
      renderHealth();
      break;
    case Page::Toolhead:
      renderToolhead();
      break;
    case Page::Extruder:
      renderExtruder();
      break;
    case Page::Ai:
      renderAi();
      break;
  }
  updateRgb();
}

void applyPrinterStatus(JsonObjectConst status) {
  if (!status["webhooks"].isNull()) {
    printer.klippyState = status["webhooks"]["state"] | printer.klippyState;
  }
  if (!status["print_stats"].isNull()) {
    const String previousPrintState = printer.printState;
    printer.printState = status["print_stats"]["state"] | printer.printState;
    printer.filename = String(status["print_stats"]["filename"] | printer.filename.c_str());
    printer.printDuration = status["print_stats"]["print_duration"] | printer.printDuration;
    if (printer.printState == "printing" && previousPrintState != "printing" &&
        previousPrintState != "paused") {
      modelPreviewReady = false;
      modelPreviewFilename = "";
      lastPreviewAttemptAt = 0;
    }
  }
  if (!status["extruder"].isNull()) {
    printer.nozzleTemperature = status["extruder"]["temperature"] | printer.nozzleTemperature;
    printer.nozzleTarget = status["extruder"]["target"] | printer.nozzleTarget;
    printer.pressureAdvance = status["extruder"]["pressure_advance"] | printer.pressureAdvance;
    printer.smoothTime = status["extruder"]["smooth_time"] | printer.smoothTime;
  }
  if (!status["heater_bed"].isNull()) {
    printer.bedTemperature = status["heater_bed"]["temperature"] | printer.bedTemperature;
    printer.bedTarget = status["heater_bed"]["target"] | printer.bedTarget;
  }
  if (!status["virtual_sdcard"].isNull()) {
    printer.progress = status["virtual_sdcard"]["progress"] | printer.progress;
  }
  if (!status["display_status"].isNull()) {
    printer.message = String(status["display_status"]["message"] | printer.message.c_str());
  }
  if (!status["gcode_move"].isNull()) {
    const float speedFactor = status["gcode_move"]["speed_factor"] | (printer.speedPercent / 100.0f);
    const float flowFactor = status["gcode_move"]["extrude_factor"] | (printer.flowPercent / 100.0f);
    printer.speedPercent = constrain(static_cast<int>(lroundf(speedFactor * 100)), 1, 999);
    printer.flowPercent = constrain(static_cast<int>(lroundf(flowFactor * 100)), 1, 999);
    const JsonArrayConst position = status["gcode_move"]["gcode_position"].as<JsonArrayConst>();
    if (position.size() >= 3) {
      printer.positionX = position[0] | printer.positionX;
      printer.positionY = position[1] | printer.positionY;
      printer.positionZ = position[2] | printer.positionZ;
    }
    const JsonArrayConst origin = status["gcode_move"]["homing_origin"].as<JsonArrayConst>();
    if (origin.size() >= 3) printer.zOffset = origin[2] | printer.zOffset;
  }
  if (!status["toolhead"].isNull()) {
    printer.homedAxes = String(status["toolhead"]["homed_axes"] | printer.homedAxes.c_str());
  }
  const JsonObjectConst switchSensor = status["filament_switch_sensor sfs_switch"];
  const JsonObjectConst motionSensor = status["filament_motion_sensor sfs_motion"];
  if (!switchSensor.isNull() || !motionSensor.isNull()) {
    printer.hasFilamentSensors = true;
    if (!switchSensor.isNull()) {
      printer.filamentPresent = switchSensor["filament_detected"] | printer.filamentPresent;
    }
    if (!motionSensor.isNull()) {
      printer.filamentMoving = motionSensor["filament_detected"] | printer.filamentMoving;
    }
  }
  if (!status["temperature_sensor btt_eddy_mcu"].isNull()) {
    printer.eddyTemperature =
        status["temperature_sensor btt_eddy_mcu"]["temperature"] | printer.eddyTemperature;
  }
  printer.online = true;
}

void sendSubscription() {
  JsonDocument request;
  request["jsonrpc"] = "2.0";
  request["method"] = "printer.objects.subscribe";
  request["id"] = 1;
  JsonObject objects = request["params"]["objects"].to<JsonObject>();
  objects["webhooks"].to<JsonArray>().add("state");
  JsonArray printStats = objects["print_stats"].to<JsonArray>();
  printStats.add("state");
  printStats.add("filename");
  printStats.add("print_duration");
  JsonArray virtualSd = objects["virtual_sdcard"].to<JsonArray>();
  virtualSd.add("progress");
  JsonArray extruder = objects["extruder"].to<JsonArray>();
  extruder.add("temperature");
  extruder.add("target");
  extruder.add("pressure_advance");
  extruder.add("smooth_time");
  JsonArray bed = objects["heater_bed"].to<JsonArray>();
  bed.add("temperature");
  bed.add("target");
  JsonArray displayStatus = objects["display_status"].to<JsonArray>();
  displayStatus.add("progress");
  displayStatus.add("message");
  JsonArray gcodeMove = objects["gcode_move"].to<JsonArray>();
  gcodeMove.add("speed_factor");
  gcodeMove.add("extrude_factor");
  gcodeMove.add("gcode_position");
  gcodeMove.add("homing_origin");
  objects["toolhead"].to<JsonArray>().add("homed_axes");
  objects["filament_switch_sensor sfs_switch"].to<JsonArray>().add("filament_detected");
  objects["filament_motion_sensor sfs_motion"].to<JsonArray>().add("filament_detected");
  objects["temperature_sensor btt_eddy_mcu"].to<JsonArray>().add("temperature");

  String payload;
  serializeJson(request, payload);
  moonrakerSocket.sendTXT(payload);
}

void handleSocketEvent(WStype_t type, uint8_t *payload, size_t length) {
  if (type == WStype_CONNECTED) {
    socketConnected = true;
    sendSubscription();
    return;
  }
  if (type == WStype_DISCONNECTED) {
    socketConnected = false;
    return;
  }
  if (type != WStype_TEXT) return;

  JsonDocument document;
  if (deserializeJson(document, payload, length)) return;
  JsonObjectConst status;
  const String method = String(document["method"] | "");
  if (method == "notify_status_update") {
    status = document["params"][0].as<JsonObjectConst>();
  } else if (!document["result"]["status"].isNull()) {
    status = document["result"]["status"].as<JsonObjectConst>();
  } else if (method == "notify_klippy_ready") {
    sendSubscription();
    return;
  }
  if (!status.isNull()) {
    applyPrinterStatus(status);
    refreshActivePage();
  }
}

void startMoonrakerSocket() {
  if (strlen(MOONRAKER_API_KEY) > 0) {
    socketHeaders = String("X-Api-Key: ") + MOONRAKER_API_KEY + "\r\n";
    moonrakerSocket.setExtraHeaders(socketHeaders.c_str());
  }
  moonrakerSocket.begin(MOONRAKER_HOST, MOONRAKER_PORT, "/websocket");
  moonrakerSocket.onEvent(handleSocketEvent);
  moonrakerSocket.setReconnectInterval(5000);
  moonrakerSocket.enableHeartbeat(15000, 3000, 2);
}

bool postMoonraker(const String &path) {
  if (WiFi.status() != WL_CONNECTED || !printer.online) return false;
  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + MOONRAKER_HOST + ":" + MOONRAKER_PORT + path;
  http.setConnectTimeout(1500);
  http.setTimeout(2000);
  if (!http.begin(client, endpoint)) return false;
  if (strlen(MOONRAKER_API_KEY) > 0) http.addHeader("X-Api-Key", MOONRAKER_API_KEY);
  const int statusCode = http.POST("");
  http.end();
  return statusCode >= 200 && statusCode < 300;
}

void addGatewayToken(HTTPClient &http) {
  if (strlen(AI_GATEWAY_TOKEN) > 0) http.addHeader("X-Display-Token", AI_GATEWAY_TOKEN);
}

bool printerIdleForOta() {
  return printer.klippyState == "ready" && printer.printState != "printing" &&
         printer.printState != "paused";
}

bool parseVersion(const String &input, int parts[3]) {
  String version = input;
  version.trim();
  if (version.startsWith("v")) version.remove(0, 1);
  int start = 0;
  for (int index = 0; index < 3; ++index) {
    const int separator = version.indexOf('.', start);
    const int end = index == 2 ? version.length() : separator;
    if (end <= start || (index < 2 && separator < 0)) return false;
    for (int cursor = start; cursor < end; ++cursor) {
      if (!isDigit(version[cursor])) return false;
    }
    parts[index] = version.substring(start, end).toInt();
    start = end + 1;
  }
  return start == version.length() + 1;
}

bool versionIsNewer(const String &candidate) {
  int remote[3] = {0, 0, 0};
  int current[3] = {0, 0, 0};
  if (!parseVersion(candidate, remote) || !parseVersion(CYD_FIRMWARE_VERSION, current)) return false;
  for (int index = 0; index < 3; ++index) {
    if (remote[index] != current[index]) return remote[index] > current[index];
  }
  return false;
}

bool checkForOtaUpdate() {
  ota.state = OtaState::Checking;
  ota.message = "";
  if (activePage == Page::Health) renderHealth();
  if (WiFi.status() != WL_CONNECTED) {
    ota.state = OtaState::Failed;
    ota.message = "Wi-Fi offline";
    return false;
  }

  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + AI_GATEWAY_HOST + ":" + AI_GATEWAY_PORT +
                          "/v1/firmware/manifest";
  http.setConnectTimeout(2000);
  http.setTimeout(5000);
  if (!http.begin(client, endpoint)) {
    ota.state = OtaState::Failed;
    ota.message = "Gateway unavailable";
    return false;
  }
  addGatewayToken(http);
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    ota.state = OtaState::Failed;
    ota.message = String("HTTP ") + statusCode;
    http.end();
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) {
    ota.state = OtaState::Failed;
    ota.message = "Invalid manifest";
    return false;
  }
  ota.version = String(document["version"] | "");
  ota.sha256 = String(document["sha256"] | "");
  ota.size = document["size"] | 0;
  int parsed[3] = {0, 0, 0};
  if (!parseVersion(ota.version, parsed) || ota.sha256.length() != 64 || ota.size == 0 ||
      ota.size > ESP.getFreeSketchSpace()) {
    ota.state = OtaState::Failed;
    ota.message = "Invalid firmware";
    return false;
  }
  ota.state = versionIsNewer(ota.version) ? OtaState::Available : OtaState::Current;
  return true;
}

void drawOtaProgress(int percent, const String &label) {
  display.fillScreen(kBlack);
  display.setTextDatum(MC_DATUM);
  display.setTextColor(kYellow, kBlack);
  display.drawString("WI-FI FIRMWARE UPDATE", 160, 58, 4);
  display.setTextColor(kWhite, kBlack);
  display.drawString(label, 160, 103, 2);
  display.drawRect(20, 132, 280, 24, kWhite);
  display.fillRect(22, 134, 276, 20, kBlack);
  const int width = (276 * constrain(percent, 0, 100)) / 100;
  if (width > 0) display.fillRect(22, 134, width, 20, kYellow);
  display.setTextColor(percent >= 52 ? kBlack : kWhite);
  display.drawString(String(percent) + "%", 160, 144, 2);
  display.setTextColor(kWhite, kBlack);
  display.drawString("Do not remove display power", 160, 184, 2);
}

bool failOta(const String &message, HTTPClient *http = nullptr, bool abortUpdate = false) {
  if (abortUpdate) Update.abort();
  if (http) http->end();
  ota.state = OtaState::Failed;
  ota.message = message;
  otaHoldArmed = false;
  drawHealthLayout();
  renderHealth(true);
  return false;
}

bool performOtaUpdate() {
  if (!printerIdleForOta()) return failOta("Printer is active");
  drawOtaProgress(0, String("Installing v") + ota.version);

  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + AI_GATEWAY_HOST + ":" + AI_GATEWAY_PORT +
                          "/v1/firmware/image";
  http.setConnectTimeout(3000);
  http.setTimeout(15000);
  if (!http.begin(client, endpoint)) return failOta("Gateway unavailable");
  addGatewayToken(http);
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK || http.getSize() != static_cast<int>(ota.size)) {
    return failOta(String("Firmware HTTP ") + statusCode, &http);
  }
  if (!Update.begin(ota.size, U_FLASH)) return failOta("OTA partition unavailable", &http);

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts_ret(&sha, 0);
  WiFiClient *stream = http.getStreamPtr();
  uint8_t buffer[1024];
  size_t received = 0;
  int shownPercent = -1;
  unsigned long lastDataAt = millis();
  while (received < ota.size) {
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastDataAt > 10000) {
        mbedtls_sha256_free(&sha);
        return failOta("Firmware timeout", &http, true);
      }
      delay(2);
      continue;
    }
    const size_t wanted = min(static_cast<size_t>(available),
                              min(sizeof(buffer), ota.size - received));
    const int count = stream->read(buffer, wanted);
    if (count <= 0) continue;
    lastDataAt = millis();
    mbedtls_sha256_update_ret(&sha, buffer, count);
    if (Update.write(buffer, count) != static_cast<size_t>(count)) {
      mbedtls_sha256_free(&sha);
      return failOta("Flash write failed", &http, true);
    }
    received += count;
    const int percent = static_cast<int>((received * 100) / ota.size);
    if (percent != shownPercent) {
      shownPercent = percent;
      drawOtaProgress(percent, String("Installing v") + ota.version);
    }
  }

  uint8_t digest[32];
  mbedtls_sha256_finish_ret(&sha, digest);
  mbedtls_sha256_free(&sha);
  char digestHex[65];
  for (int index = 0; index < 32; ++index) snprintf(digestHex + index * 2, 3, "%02x", digest[index]);
  digestHex[64] = '\0';
  if (!ota.sha256.equalsIgnoreCase(digestHex)) return failOta("SHA-256 mismatch", &http, true);
  if (!Update.end()) return failOta("Firmware validation failed", &http, true);
  http.end();

  drawOtaProgress(100, "Verified - restarting");
  delay(1500);
  ESP.restart();
  return true;
}

bool fetchModelPreview() {
  if (WiFi.status() != WL_CONNECTED || printer.filename.isEmpty()) return false;
  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + AI_GATEWAY_HOST + ":" + AI_GATEWAY_PORT +
                          "/v1/model-preview";
  http.setConnectTimeout(2000);
  http.setTimeout(30000);
  if (!http.begin(client, endpoint)) return false;
  addGatewayToken(http);
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK || http.getSize() != static_cast<int>(kModelPreviewBytes)) {
    modelPreviewError = String("HTTP ") + statusCode;
    http.end();
    return false;
  }
  WiFiClient *stream = http.getStreamPtr();
  const size_t received =
      stream->readBytes(reinterpret_cast<char *>(modelPreviewPixels), kModelPreviewBytes);
  http.end();
  if (received != kModelPreviewBytes) {
    modelPreviewError = "Short preview";
    return false;
  }
  modelPreviewError = "";
  return true;
}

void refreshModelPreviewIfNeeded(unsigned long now) {
  if (printer.filename.isEmpty() ||
      (modelPreviewReady && printer.filename == modelPreviewFilename)) {
    return;
  }
  if (lastPreviewAttemptAt != 0 && now - lastPreviewAttemptAt < kPreviewRetryMs) return;
  lastPreviewAttemptAt = now;
  modelPreviewReady = false;
  modelPreviewError = "";
  if (activePage == Page::Dashboard) drawModelPreview(4, 42);
  if (activePage == Page::Model) drawModelPreview(50, 42);

  if (fetchModelPreview()) {
    modelPreviewFilename = printer.filename;
    modelPreviewReady = true;
  }
  if (activePage == Page::Dashboard) drawModelPreview(4, 42);
  if (activePage == Page::Model) drawModelPreview(50, 42);
}

bool requestAiAnalysis() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + AI_GATEWAY_HOST + ":" + AI_GATEWAY_PORT +
                          "/v1/analyze/start";
  http.setConnectTimeout(1200);
  http.setTimeout(1800);
  if (!http.begin(client, endpoint)) return false;
  addGatewayToken(http);
  const int statusCode = http.POST("");
  http.end();
  aiAssessment.gatewayOnline = statusCode > 0;
  return statusCode == HTTP_CODE_ACCEPTED;
}

bool pollAiAssessment() {
  if (WiFi.status() != WL_CONNECTED) return false;
  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + AI_GATEWAY_HOST + ":" + AI_GATEWAY_PORT +
                          "/v1/assessment";
  http.setConnectTimeout(1000);
  http.setTimeout(1500);
  if (!http.begin(client, endpoint)) return false;
  addGatewayToken(http);
  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    http.end();
    aiAssessment.gatewayOnline = statusCode > 0;
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) return false;
  aiAssessment.gatewayOnline = true;
  if (document.isNull()) {
    aiAssessment.available = false;
    return true;
  }

  const String newAnalyzedAt = String(document["analyzed_at"] | "");
  const bool isNewAssessment = !newAnalyzedAt.isEmpty() && newAnalyzedAt != aiAssessment.analyzedAt;
  aiAssessment.available = true;
  aiAssessment.status = String(document["status"] | "unknown");
  aiAssessment.summary = String(document["summary"] | "No summary returned");
  aiAssessment.recommendation = String(document["recommendation"] | "Inspect the printer manually");
  aiAssessment.confidencePercent =
      constrain(static_cast<int>(lroundf((document["confidence"] | 0.0f) * 100)), 0, 100);
  aiAssessment.analyzedAt = newAnalyzedAt;
  if (isNewAssessment) aiAssessment.analyzing = false;
  return true;
}

bool pollMoonraker() {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClient client;
  HTTPClient http;
  const String endpoint = String("http://") + MOONRAKER_HOST + ":" + MOONRAKER_PORT +
      "/printer/objects/query?webhooks&print_stats&virtual_sdcard&"
      "extruder=temperature,target,pressure_advance,smooth_time&heater_bed=temperature,target&"
      "display_status=progress,message&toolhead=homed_axes&"
      "gcode_move=speed_factor,extrude_factor,gcode_position,homing_origin&"
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

  applyPrinterStatus(status);
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
  startMoonrakerSocket();
  activePage = Page::Dashboard;
  redrawActivePage();
}

void loop() {
  const unsigned long now = millis();
  moonrakerSocket.loop();
  refreshModelPreviewIfNeeded(now);
  if (!otaBootCheckDone && WiFi.status() == WL_CONNECTED && now >= 10000) {
    otaBootCheckDone = true;
    checkForOtaUpdate();
  }

  if (WiFi.status() != WL_CONNECTED && now - lastWifiAttemptAt >= kWifiRetryMs) {
    lastWifiAttemptAt = now;
    WiFi.reconnect();
    printer.online = false;
    if (activePage == Page::Dashboard) render();
    updateRgb();
  }

  if (!socketConnected && now - lastPollAt >= kFallbackPollIntervalMs) {
    lastPollAt = now;
    if (!pollMoonraker()) printer.online = false;
    if (activePage == Page::Dashboard) {
      render();
    } else if (activePage == Page::Model) {
      renderModel();
    } else if (activePage == Page::Health) {
      renderHealth();
    } else if (activePage == Page::Toolhead) {
      renderToolhead();
    } else if (activePage == Page::Extruder) {
      renderExtruder();
    }
    updateRgb();
  }

  if ((activePage == Page::Ai || aiAssessment.analyzing) &&
      now - lastAiPollAt >= kAiPollIntervalMs) {
    lastAiPollAt = now;
    if (!pollAiAssessment()) aiAssessment.gatewayOnline = false;
    if (activePage == Page::Ai) renderAi();
  }

  handleTouch();
  updateBacklight();

  delay(20);
}

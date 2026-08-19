#include <Arduino.h>
#include <Network.h>
#include <WiFi.h>
#include <Preferences.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>

#include "esp_freertos_hooks.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

// TFT pins
#define TFT_CS    10
#define TFT_DC     9
#define TFT_RST    8
#define TFT_MOSI  11
#define TFT_SCLK  12

// Other hardware
#define BUZZER_PIN 7
#define BUTTON_PIN 6

// UDP
#define UDP_PORT 4210

// Tetris board
#define BOARD_WIDTH  10
#define BOARD_HEIGHT 20
#define CELL_SIZE 7

#define BOARD_X 2
#define BOARD_Y 19

#define INFO_X 77

// System monitor
#define MONITOR_X 76
#define MONITOR_Y 123
#define MONITOR_W 52
#define MONITOR_H 36

// Game timing
#define LOCK_DELAY_MS 400
#define MIN_FALL_INTERVAL 100

// FreeRTOS queue lengths
#define COMMAND_QUEUE_LENGTH 10
#define AUDIO_QUEUE_LENGTH 8

// Number of remembered WiFi networks
#define MAX_SAVED_WIFI 8

// Event group bits
#define EVT_WIFI      BIT0
#define EVT_STARTED   BIT1
#define EVT_PAUSED    BIT2
#define EVT_GAMEOVER  BIT3

Adafruit_ST7735 tft(
  TFT_CS,
  TFT_DC,
  TFT_MOSI,
  TFT_SCLK,
  TFT_RST
);

NetworkUDP udp;
Preferences preferences;

// FreeRTOS objects
QueueHandle_t commandQueue = nullptr;
QueueHandle_t audioQueue = nullptr;
QueueHandle_t displayQueue = nullptr;

SemaphoreHandle_t tftMutex = nullptr;

EventGroupHandle_t systemEvents = nullptr;

// Task handles
TaskHandle_t gestureTaskHandle = nullptr;
TaskHandle_t gameTaskHandle = nullptr;
TaskHandle_t displayTaskHandle = nullptr;
TaskHandle_t audioTaskHandle = nullptr;
TaskHandle_t monitorTaskHandle = nullptr;

// WiFi state
String activeSSID = "";
String activePassword = "";

bool udpStarted = false;

// Persistent score
uint32_t highScore = 0;

// CPU monitor data
volatile uint32_t idleCounter0 = 0;
volatile uint32_t idleCounter1 = 0;

uint32_t idleBaseline0 = 1;
uint32_t idleBaseline1 = 1;

bool cpuMonitorReady = false;

volatile uint8_t monitorCPU0 = 0;
volatile uint8_t monitorCPU1 = 0;
volatile uint8_t monitorRAM = 0;

// Last FreeRTOS queue dispatch latency
volatile uint16_t monitorLatencyMs = 0;
volatile bool monitorLatencyValid = false;

// Tetromino types
enum PieceType {
  PIECE_I,
  PIECE_O,
  PIECE_T,
  PIECE_S,
  PIECE_Z,
  PIECE_J,
  PIECE_L
};

// Game commands
enum GameCommand {
  CMD_NONE,
  CMD_LEFT,
  CMD_RIGHT,
  CMD_ROTATE,
  CMD_DROP,
  CMD_PAUSE,
  CMD_RESUME,
  CMD_BUTTON
};

// FreeRTOS command message
struct CommandMessage {
  GameCommand command;
  TickType_t queuedAt;
};

// Tetromino colors
const uint16_t PIECE_COLORS[7] = {
  ST77XX_CYAN,
  ST77XX_YELLOW,
  ST77XX_MAGENTA,
  ST77XX_GREEN,
  ST77XX_RED,
  ST77XX_BLUE,
  0xFD20
};

const uint16_t GRID_COLOR = 0x2104;
const uint16_t GHOST_COLOR = 0x630C;

// Tetromino definitions
const int8_t SHAPES[7][4][4][2] = {

  // I
  {
    {{0,1},{1,1},{2,1},{3,1}},
    {{2,0},{2,1},{2,2},{2,3}},
    {{0,2},{1,2},{2,2},{3,2}},
    {{1,0},{1,1},{1,2},{1,3}}
  },

  // O
  {
    {{1,0},{2,0},{1,1},{2,1}},
    {{1,0},{2,0},{1,1},{2,1}},
    {{1,0},{2,0},{1,1},{2,1}},
    {{1,0},{2,0},{1,1},{2,1}}
  },

  // T
  {
    {{1,0},{0,1},{1,1},{2,1}},
    {{1,0},{1,1},{2,1},{1,2}},
    {{0,1},{1,1},{2,1},{1,2}},
    {{1,0},{0,1},{1,1},{1,2}}
  },

  // S
  {
    {{1,0},{2,0},{0,1},{1,1}},
    {{1,0},{1,1},{2,1},{2,2}},
    {{1,1},{2,1},{0,2},{1,2}},
    {{0,0},{0,1},{1,1},{1,2}}
  },

  // Z
  {
    {{0,0},{1,0},{1,1},{2,1}},
    {{2,0},{1,1},{2,1},{1,2}},
    {{0,1},{1,1},{1,2},{2,2}},
    {{1,0},{0,1},{1,1},{0,2}}
  },

  // J
  {
    {{0,0},{0,1},{1,1},{2,1}},
    {{1,0},{2,0},{1,1},{1,2}},
    {{0,1},{1,1},{2,1},{2,2}},
    {{1,0},{1,1},{0,2},{1,2}}
  },

  // L
  {
    {{2,0},{0,1},{1,1},{2,1}},
    {{1,0},{1,1},{1,2},{2,2}},
    {{0,1},{1,1},{2,1},{0,2}},
    {{0,0},{1,0},{1,1},{1,2}}
  }
};

// Active piece
struct Piece {
  uint8_t type;
  uint8_t rotation;
  int8_t x;
  int8_t y;
};

// Copy of the game passed to DisplayTask
struct GameSnapshot {
  uint8_t board[BOARD_HEIGHT][BOARD_WIDTH];

  Piece currentPiece;

  uint8_t nextPieceType;

  uint32_t score;
  uint32_t highScore;

  uint16_t totalLines;

  uint8_t level;

  bool gameStarted;
  bool paused;
  bool gameOver;
};

// Game board
uint8_t board[BOARD_HEIGHT][BOARD_WIDTH];

Piece currentPiece;

uint8_t nextPieceType;

uint32_t score = 0;
uint16_t totalLines = 0;
uint8_t level = 1;

bool gameStarted = false;
bool paused = false;
bool gameOver = false;

bool gameStateDirty = false;

// Game timers
uint32_t lastFallTime = 0;
uint32_t groundedSince = 0;

// 7-bag
uint8_t bag[7];
uint8_t bagIndex = 7;

// Button debounce
uint32_t lastButtonCommandTime = 0;


// Center text on 128px TFT
int centeredX(const String &text, uint8_t textSize) {

  int textWidth =
    text.length() * 6 * textSize;

  int x =
    (128 - textWidth) / 2;

  if (x < 0) {
    x = 0;
  }

  return x;
}


// Type text character by character
void typeCenteredText(
  const String &text,
  int y,
  uint8_t textSize,
  uint16_t color,
  uint16_t characterDelay
) {

  tft.setTextSize(textSize);
  tft.setTextColor(color);

  tft.setCursor(
    centeredX(text, textSize),
    y
  );

  for (int i = 0; i < text.length(); i++) {

    tft.print(text[i]);

    delay(characterDelay);
  }
}


// First animated splash screen
void animateGestureOSTitle() {

  tft.fillScreen(ST77XX_BLACK);

  // Animate upper and lower frame lines
  for (int width = 0; width <= 120; width += 6) {

    tft.drawFastHLine(
      4,
      5,
      width,
      ST77XX_CYAN
    );

    tft.drawFastHLine(
      4,
      154,
      width,
      ST77XX_CYAN
    );

    delay(15);
  }

  // Animate side frame lines
  for (int height = 0; height <= 149; height += 7) {

    tft.drawFastVLine(
      4,
      5,
      height,
      ST77XX_CYAN
    );

    tft.drawFastVLine(
      123,
      5,
      height,
      ST77XX_CYAN
    );

    delay(12);
  }

  typeCenteredText(
    "GestureOS",
    41,
    2,
    ST77XX_CYAN,
    75
  );

  // Animated divider
  for (int width = 0; width <= 82; width += 5) {

    tft.drawFastHLine(
      23,
      65,
      width,
      ST77XX_WHITE
    );

    delay(12);
  }

  typeCenteredText(
    "TETRIS",
    76,
    2,
    ST77XX_YELLOW,
    90
  );

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(
    44,
    114
  );

  tft.print("BOOTING");

  for (int i = 0; i < 3; i++) {

    tft.print(".");

    delay(220);
  }

  delay(450);
}


// Course information animation
void animateCourseScreen() {

  tft.fillScreen(ST77XX_BLACK);

  // Expanding rectangle effect
  for (int margin = 30; margin >= 8; margin -= 2) {

    tft.drawRect(
      margin,
      margin,
      128 - margin * 2,
      160 - margin * 2,
      ST77XX_BLUE
    );

    delay(20);
  }

  typeCenteredText(
    "CSE323.7",
    46,
    2,
    ST77XX_CYAN,
    85
  );

  delay(250);

  typeCenteredText(
    "Group 8",
    79,
    2,
    ST77XX_YELLOW,
    90
  );

  delay(1000);
}


// Group-member credit animation
void animateMemberScreen() {

  tft.fillScreen(ST77XX_BLACK);

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_MAGENTA);

  tft.setCursor(
    centeredX(
      "GROUP MEMBERS",
      1
    ),
    16
  );

  tft.print(
    "GROUP MEMBERS"
  );

  // Animate underline
  for (int width = 0; width <= 90; width += 5) {

    tft.drawFastHLine(
      19,
      30,
      width,
      ST77XX_MAGENTA
    );

    delay(15);
  }

  typeCenteredText(
    "Sabiha Binte Siraj",
    50,
    1,
    ST77XX_WHITE,
    50
  );

  delay(220);

  typeCenteredText(
    "Sabbir Ahamed",
    78,
    1,
    ST77XX_WHITE,
    50
  );

  delay(220);

  typeCenteredText(
    "Tarif Bin Mehedi",
    106,
    1,
    ST77XX_WHITE,
    50
  );

  delay(1100);
}


// Final boot transition
void animateBootWipe() {

  for (int x = 0; x < 128; x += 8) {

    tft.fillRect(
      x,
      0,
      8,
      160,
      ST77XX_BLACK
    );

    delay(25);
  }
}


// Complete startup sequence
void runBootAnimation() {

  animateGestureOSTitle();

  animateCourseScreen();

  animateMemberScreen();

  animateBootWipe();
}


// Permanent top header
void drawHeader() {

  tft.fillRect(
    0,
    0,
    128,
    18,
    ST77XX_BLACK
  );

  tft.setTextSize(1);

  // Project title
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(
    2,
    4
  );

  tft.print(
    "GestureOS"
  );

  // Course information
  tft.setTextColor(ST77XX_CYAN);

  tft.setCursor(
    77,
    1
  );

  tft.print(
    "CSE323.7"
  );

  // Group information
  tft.setTextColor(ST77XX_YELLOW);

  tft.setCursor(
    86,
    9
  );

  tft.print(
    "Group 8"
  );
}


// CPU Core 0 idle hook
bool idleHookCore0() {

  idleCounter0++;

  return false;
}


// CPU Core 1 idle hook
bool idleHookCore1() {

  idleCounter1++;

  return false;
}


// Set up live CPU monitor
void setupCPUMonitor() {

  esp_err_t result0 =
    esp_register_freertos_idle_hook_for_cpu(
      idleHookCore0,
      0
    );

  esp_err_t result1 =
    esp_register_freertos_idle_hook_for_cpu(
      idleHookCore1,
      1
    );

  if (
    result0 != ESP_OK ||
    result1 != ESP_OK
  ) {

    Serial.println(
      "CPU monitor hook registration FAILED."
    );

    cpuMonitorReady = false;

    return;
  }

  Serial.println(
    "Calibrating CPU monitor..."
  );

  uint32_t start0 =
    idleCounter0;

  uint32_t start1 =
    idleCounter1;

  uint32_t startTime =
    millis();

  delay(1500);

  uint32_t elapsed =
    millis() - startTime;

  uint32_t delta0 =
    idleCounter0 - start0;

  uint32_t delta1 =
    idleCounter1 - start1;

  idleBaseline0 =
    (
      (uint64_t)delta0 *
      1000ULL
    ) /
    elapsed;

  idleBaseline1 =
    (
      (uint64_t)delta1 *
      1000ULL
    ) /
    elapsed;

  if (idleBaseline0 == 0) {
    idleBaseline0 = 1;
  }

  if (idleBaseline1 == 0) {
    idleBaseline1 = 1;
  }

  cpuMonitorReady = true;

  Serial.println(
    "CPU monitor READY."
  );
}


// Calculate CPU busy percentage
uint8_t calculateCPUUsage(
  uint32_t idlePerSecond,
  uint32_t &baseline
) {

  // Allow baseline to adapt if system becomes more idle
  if (
    idlePerSecond >
    baseline
  ) {

    baseline =
      idlePerSecond;
  }

  if (baseline == 0) {
    return 0;
  }

  uint32_t idlePercent =
    (
      (uint64_t)idlePerSecond *
      100ULL
    ) /
    baseline;

  if (idlePercent > 100) {
    idlePercent = 100;
  }

  return
    100 - idlePercent;
}


// Calculate actual internal RAM usage
uint8_t getRAMUsagePercent() {

  size_t totalRAM =
    heap_caps_get_total_size(
      MALLOC_CAP_INTERNAL |
      MALLOC_CAP_8BIT
    );

  size_t freeRAM =
    heap_caps_get_free_size(
      MALLOC_CAP_INTERNAL |
      MALLOC_CAP_8BIT
    );

  if (totalRAM == 0) {
    return 0;
  }

  size_t usedRAM =
    totalRAM -
    freeRAM;

  uint32_t percent =
    (
      usedRAM *
      100UL
    ) /
    totalRAM;

  if (percent > 100) {
    percent = 100;
  }

  return (uint8_t)percent;
}


// Preferences key for SSID
String ssidKey(int slot) {

  return
    "ssid" +
    String(slot);
}


// Preferences key for password
String passKey(int slot) {

  return
    "pass" +
    String(slot);
}


// Find saved WiFi by SSID
int findSavedWiFi(
  const String &ssid
) {

  for (
    int i = 0;
    i < MAX_SAVED_WIFI;
    i++
  ) {

    String savedSSID =
      preferences.getString(
        ssidKey(i).c_str(),
        ""
      );

    if (
      savedSSID.length() > 0 &&
      savedSSID == ssid
    ) {

      return i;
    }
  }

  return -1;
}


// Find empty WiFi credential slot
int findEmptyWiFiSlot() {

  for (
    int i = 0;
    i < MAX_SAVED_WIFI;
    i++
  ) {

    String savedSSID =
      preferences.getString(
        ssidKey(i).c_str(),
        ""
      );

    if (
      savedSSID.length() == 0
    ) {

      return i;
    }
  }

  return -1;
}


// Get saved WiFi password
String getSavedPassword(
  int slot
) {

  if (
    slot < 0 ||
    slot >= MAX_SAVED_WIFI
  ) {

    return "";
  }

  return preferences.getString(
    passKey(slot).c_str(),
    ""
  );
}


// Save or replace WiFi credential
void saveWiFiCredential(
  const String &ssid,
  const String &password
) {

  int slot =
    findSavedWiFi(
      ssid
    );

  if (slot < 0) {

    slot =
      findEmptyWiFiSlot();

    // Replace first slot if all eight are used
    if (slot < 0) {
      slot = 0;
    }
  }

  preferences.putString(
    ssidKey(slot).c_str(),
    ssid
  );

  preferences.putString(
    passKey(slot).c_str(),
    password
  );

  Serial.print(
    "Credentials saved for: "
  );

  Serial.println(
    ssid
  );
}


// Read one line from Serial Monitor
String readSerialLine(
  const String &prompt,
  bool allowEmpty = false
) {

  Serial.print(
    prompt
  );

  String text = "";

  while (true) {

    while (
      Serial.available()
    ) {

      char c =
        Serial.read();

      if (
        c == '\r' ||
        c == '\n'
      ) {

        // Consume any additional newline characters
        while (
          Serial.available()
        ) {

          char next =
            Serial.peek();

          if (
            next == '\r' ||
            next == '\n'
          ) {

            Serial.read();

          } else {

            break;
          }
        }

        if (
          text.length() > 0 ||
          allowEmpty
        ) {

          return text;
        }

      } else {

        text += c;
      }
    }

    delay(10);
  }
}


// Generic boot screen
void showBootMessage(
  String line1,
  String line2 = "",
  String line3 = "",
  String line4 = ""
) {

  tft.fillScreen(
    ST77XX_BLACK
  );

  drawHeader();

  tft.drawLine(
    4,
    18,
    123,
    18,
    ST77XX_WHITE
  );

  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);

  tft.setCursor(
    5,
    30
  );

  tft.print(line1);

  tft.setCursor(
    5,
    47
  );

  tft.print(line2);

  tft.setCursor(
    5,
    64
  );

  tft.print(line3);

  tft.setCursor(
    5,
    81
  );

  tft.print(line4);
}


// Connect to selected network
bool connectToWiFi(
  const String &ssid,
  const String &password,
  uint32_t timeoutMs = 12000
) {

  Serial.println();

  Serial.print(
    "Connecting to: "
  );

  Serial.println(
    ssid
  );

  showBootMessage(
    "Connecting...",
    ssid,
    "Please wait"
  );

  WiFi.disconnect();

  delay(300);

  if (
    password.length() == 0
  ) {

    WiFi.begin(
      ssid.c_str()
    );

  } else {

    WiFi.begin(
      ssid.c_str(),
      password.c_str()
    );
  }

  uint32_t start =
    millis();

  Serial.print(
    "Connecting"
  );

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - start < timeoutMs
  ) {

    Serial.print(".");

    delay(500);
  }

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    activeSSID =
      WiFi.SSID();

    activePassword =
      password;

    Serial.println(
      "WiFi CONNECTED!"
    );

    Serial.print(
      "Connected WiFi: "
    );

    Serial.println(
      WiFi.SSID()
    );

    Serial.print(
      "ESP32 IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    return true;
  }

  Serial.println(
    "Connection FAILED."
  );

  return false;
}


// Scan networks every boot and let user choose
bool selectWiFi() {

  while (true) {

    showBootMessage(
      "Scanning WiFi...",
      "Open Serial",
      "Monitor"
    );

    Serial.println();

    Serial.println(
      "Available WiFi networks:"
    );

    Serial.println();

    WiFi.disconnect();

    delay(300);

    int networkCount =
      WiFi.scanNetworks();

    if (
      networkCount <= 0
    ) {

      Serial.println(
        "No WiFi networks found."
      );

      readSerialLine(
        "Press ENTER to rescan: ",
        true
      );

      continue;
    }

    // Display networks
    for (
      int i = 0;
      i < networkCount;
      i++
    ) {

      String ssid =
        WiFi.SSID(i);

      int savedSlot =
        findSavedWiFi(
          ssid
        );

      Serial.print("[");
      Serial.print(i + 1);
      Serial.print("] ");

      Serial.print(
        ssid
      );

      if (
        savedSlot >= 0
      ) {

        Serial.print(
          "  [SAVED]"
        );
      }

      Serial.print(
        "  RSSI: "
      );

      Serial.print(
        WiFi.RSSI(i)
      );

      Serial.println(
        " dBm"
      );
    }

    Serial.println();

    Serial.println(
      "[0] OFFLINE MODE"
    );

    Serial.println();

    String input =
      readSerialLine(
        "Select WiFi number: "
      );

    // Offline option
    if (
      input == "0"
    ) {

      WiFi.scanDelete();

      activeSSID = "";
      activePassword = "";

      Serial.println(
        "OFFLINE MODE."
      );

      return false;
    }

    int selection =
      input.toInt();

    if (
      selection < 1 ||
      selection > networkCount
    ) {

      Serial.println(
        "Invalid selection."
      );

      WiFi.scanDelete();

      continue;
    }

    String selectedSSID =
      WiFi.SSID(
        selection - 1
      );

    int savedSlot =
      findSavedWiFi(
        selectedSSID
      );

    WiFi.scanDelete();

    Serial.println();

    Serial.print(
      "Selected: "
    );

    Serial.println(
      selectedSSID
    );

    // Saved network
    if (
      savedSlot >= 0
    ) {

      Serial.println(
        "Saved password found."
      );

      Serial.println(
        "Trying saved password..."
      );

      String savedPassword =
        getSavedPassword(
          savedSlot
        );

      if (
        connectToWiFi(
          selectedSSID,
          savedPassword
        )
      ) {

        Serial.println(
          "Saved password accepted."
        );

        return true;
      }

      Serial.println();

      Serial.println(
        "Saved password failed."
      );

      Serial.println(
        "Password may have changed."
      );

      Serial.println(
        "Enter the new password."
      );
    }

    // New or changed password
    while (true) {

      Serial.println();

      Serial.println(
        "Type BACK to choose another WiFi."
      );

      String password =
        readSerialLine(
          "Password: ",
          true
        );

      if (
        password.equalsIgnoreCase(
          "BACK"
        )
      ) {

        break;
      }

      if (
        connectToWiFi(
          selectedSSID,
          password
        )
      ) {

        saveWiFiCredential(
          selectedSSID,
          password
        );

        return true;
      }

      Serial.println();

      Serial.println(
        "Connection failed."
      );

      Serial.println(
        "Try again or type BACK."
      );
    }
  }
}


// Start UDP receiver
void startUDP() {

  if (
    WiFi.status() !=
    WL_CONNECTED
  ) {

    udpStarted = false;

    return;
  }

  udp.begin(
    UDP_PORT
  );

  udpStarted = true;

  Serial.println();

  Serial.println(
    "UDP READY"
  );

  Serial.print(
    "ESP32 IP: "
  );

  Serial.println(
    WiFi.localIP()
  );

  Serial.print(
    "UDP Port: "
  );

  Serial.println(
    UDP_PORT
  );
}


// Print network status
void printNetworkInfo() {

  Serial.println();

  Serial.println(
    "Network status:"
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "WiFi: "
    );

    Serial.println(
      WiFi.SSID()
    );

    Serial.print(
      "IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "UDP: "
    );

    Serial.println(
      UDP_PORT
    );

    Serial.print(
      "RSSI: "
    );

    Serial.print(
      WiFi.RSSI()
    );

    Serial.println(
      " dBm"
    );

  } else {

    Serial.println(
      "WiFi: OFFLINE"
    );
  }

  Serial.println();
}


// Send sound request to AudioTask
void queueSound(
  uint16_t durationMs
) {

  if (
    audioQueue == nullptr
  ) {

    return;
  }

  xQueueSend(
    audioQueue,
    &durationMs,
    0
  );
}


// Save high score
void saveHighScoreIfNeeded() {

  if (
    score >
    highScore
  ) {

    highScore =
      score;

    preferences.putUInt(
      "highscore",
      highScore
    );

    Serial.print(
      "NEW HIGH SCORE: "
    );

    Serial.println(
      highScore
    );
  }
}


// Update FreeRTOS event group
void syncGameEventBits() {

  if (
    systemEvents == nullptr
  ) {

    return;
  }

  xEventGroupClearBits(
    systemEvents,
    EVT_STARTED |
    EVT_PAUSED |
    EVT_GAMEOVER
  );

  EventBits_t bits = 0;

  if (gameStarted) {
    bits |= EVT_STARTED;
  }

  if (paused) {
    bits |= EVT_PAUSED;
  }

  if (gameOver) {
    bits |= EVT_GAMEOVER;
  }

  if (bits != 0) {

    xEventGroupSetBits(
      systemEvents,
      bits
    );
  }
}


// Send latest state to DisplayTask
void publishGameSnapshot() {

  if (
    displayQueue == nullptr
  ) {

    return;
  }

  GameSnapshot snapshot;

  memcpy(
    snapshot.board,
    board,
    sizeof(board)
  );

  snapshot.currentPiece =
    currentPiece;

  snapshot.nextPieceType =
    nextPieceType;

  snapshot.score =
    score;

  snapshot.highScore =
    highScore;

  snapshot.totalLines =
    totalLines;

  snapshot.level =
    level;

  snapshot.gameStarted =
    gameStarted;

  snapshot.paused =
    paused;

  snapshot.gameOver =
    gameOver;

  xQueueOverwrite(
    displayQueue,
    &snapshot
  );

  gameStateDirty = false;
}


// Game over
void setGameOver() {

  if (gameOver) {
    return;
  }

  gameOver = true;

  saveHighScoreIfNeeded();

  queueSound(
    300
  );

  syncGameEventBits();

  gameStateDirty = true;

  Serial.println(
    "GAME OVER"
  );
}


// Refill 7-bag
void refillBag() {

  for (
    int i = 0;
    i < 7;
    i++
  ) {

    bag[i] = i;
  }

  for (
    int i = 6;
    i > 0;
    i--
  ) {

    int j =
      esp_random() %
      (i + 1);

    uint8_t temp =
      bag[i];

    bag[i] =
      bag[j];

    bag[j] =
      temp;
  }

  bagIndex = 0;
}


// Get next piece from bag
uint8_t getNextFromBag() {

  if (
    bagIndex >= 7
  ) {

    refillBag();
  }

  return bag[
    bagIndex++
  ];
}


// Check collision
bool canPlace(
  uint8_t type,
  uint8_t rotation,
  int8_t px,
  int8_t py
) {

  for (
    int i = 0;
    i < 4;
    i++
  ) {

    int x =
      px +
      SHAPES
      [type]
      [rotation]
      [i][0];

    int y =
      py +
      SHAPES
      [type]
      [rotation]
      [i][1];

    if (
      x < 0 ||
      x >= BOARD_WIDTH
    ) {

      return false;
    }

    if (
      y >= BOARD_HEIGHT
    ) {

      return false;
    }

    if (
      y < 0
    ) {

      continue;
    }

    if (
      board[y][x] != 0
    ) {

      return false;
    }
  }

  return true;
}


// Spawn next piece
void spawnPiece() {

  currentPiece.type =
    nextPieceType;

  nextPieceType =
    getNextFromBag();

  currentPiece.rotation = 0;

  currentPiece.x = 3;

  currentPiece.y = -1;

  lastFallTime =
    millis();

  groundedSince = 0;

  if (
    !canPlace(
      currentPiece.type,
      currentPiece.rotation,
      currentPiece.x,
      currentPiece.y
    )
  ) {

    setGameOver();
  }

  gameStateDirty = true;
}


// Move one cell
bool movePiece(
  int8_t dx
) {

  if (
    canPlace(
      currentPiece.type,
      currentPiece.rotation,
      currentPiece.x + dx,
      currentPiece.y
    )
  ) {

    currentPiece.x += dx;

    groundedSince = 0;

    gameStateDirty = true;

    return true;
  }

  return false;
}


// Rotate with wall kicks
bool rotatePiece() {

  uint8_t newRotation =
    (
      currentPiece.rotation +
      1
    ) % 4;

  const int8_t kicks[][2] = {
    { 0,  0},
    {-1,  0},
    { 1,  0},
    {-2,  0},
    { 2,  0},
    { 0, -1},
    {-1, -1},
    { 1, -1}
  };

  for (
    int i = 0;
    i < 8;
    i++
  ) {

    int nx =
      currentPiece.x +
      kicks[i][0];

    int ny =
      currentPiece.y +
      kicks[i][1];

    if (
      canPlace(
        currentPiece.type,
        newRotation,
        nx,
        ny
      )
    ) {

      currentPiece.rotation =
        newRotation;

      currentPiece.x =
        nx;

      currentPiece.y =
        ny;

      groundedSince = 0;

      queueSound(
        25
      );

      gameStateDirty = true;

      return true;
    }
  }

  return false;
}


// Clear completed rows
uint8_t clearCompletedLines() {

  uint8_t cleared = 0;

  for (
    int row =
      BOARD_HEIGHT - 1;

    row >= 0;

    row--
  ) {

    bool full = true;

    for (
      int col = 0;
      col < BOARD_WIDTH;
      col++
    ) {

      if (
        board[row][col] ==
        0
      ) {

        full = false;

        break;
      }
    }

    if (!full) {
      continue;
    }

    cleared++;

    for (
      int y = row;
      y > 0;
      y--
    ) {

      for (
        int x = 0;
        x < BOARD_WIDTH;
        x++
      ) {

        board[y][x] =
          board[y - 1][x];
      }
    }

    for (
      int x = 0;
      x < BOARD_WIDTH;
      x++
    ) {

      board[0][x] = 0;
    }

    row++;
  }

  return cleared;
}


// Update score/level
void addLineScore(
  uint8_t cleared
) {

  if (
    cleared == 0
  ) {

    return;
  }

  uint16_t points = 0;

  switch (cleared) {

    case 1:
      points = 100;
      break;

    case 2:
      points = 300;
      break;

    case 3:
      points = 500;
      break;

    case 4:
      points = 800;
      break;
  }

  score +=
    (uint32_t)points *
    level;

  totalLines +=
    cleared;

  level =
    (
      totalLines /
      10
    ) + 1;

  if (
    cleared == 4
  ) {

    queueSound(
      220
    );

  } else {

    queueSound(
      100
    );
  }

  Serial.print(
    "Score: "
  );

  Serial.print(
    score
  );

  Serial.print(
    " | Lines: "
  );

  Serial.print(
    totalLines
  );

  Serial.print(
    " | Level: "
  );

  Serial.println(
    level
  );
}


// Lock piece
void lockPiece() {

  for (
    int i = 0;
    i < 4;
    i++
  ) {

    int x =
      currentPiece.x +
      SHAPES
      [currentPiece.type]
      [currentPiece.rotation]
      [i][0];

    int y =
      currentPiece.y +
      SHAPES
      [currentPiece.type]
      [currentPiece.rotation]
      [i][1];

    if (
      y < 0
    ) {

      setGameOver();

      return;
    }

    board[y][x] =
      currentPiece.type + 1;
  }

  uint8_t cleared =
    clearCompletedLines();

  addLineScore(
    cleared
  );

  spawnPiece();

  gameStateDirty = true;
}


// Hard drop
void hardDrop() {

  int rows = 0;

  while (
    canPlace(
      currentPiece.type,
      currentPiece.rotation,
      currentPiece.x,
      currentPiece.y + 1
    )
  ) {

    currentPiece.y++;

    rows++;
  }

  score +=
    rows * 2;

  queueSound(
    50
  );

  Serial.print(
    "DROP +"
  );

  Serial.print(
    rows * 2
  );

  Serial.print(
    " | Score: "
  );

  Serial.println(
    score
  );

  lockPiece();
}


// Falling speed
uint32_t getFallInterval() {

  int32_t interval =
    850 -
    (
      ((int32_t)level - 1) *
      65
    );

  if (
    interval <
    MIN_FALL_INTERVAL
  ) {

    interval =
      MIN_FALL_INTERVAL;
  }

  return interval;
}


// Automatic game physics
void updateGamePhysics() {

  if (
    !gameStarted ||
    paused ||
    gameOver
  ) {

    return;
  }

  uint32_t now =
    millis();

  bool grounded =
    !canPlace(
      currentPiece.type,
      currentPiece.rotation,
      currentPiece.x,
      currentPiece.y + 1
    );

  if (grounded) {

    if (
      groundedSince == 0
    ) {

      groundedSince =
        now;
    }

    if (
      now -
      groundedSince
      >=
      LOCK_DELAY_MS
    ) {

      lockPiece();

      groundedSince = 0;

      return;
    }

  } else {

    groundedSince = 0;
  }

  if (
    !grounded &&
    now -
    lastFallTime
    >=
    getFallInterval()
  ) {

    currentPiece.y++;

    lastFallTime =
      now;

    gameStateDirty = true;
  }
}


// Pause game
void pauseGame() {

  if (
    !gameStarted ||
    gameOver ||
    paused
  ) {

    return;
  }

  paused = true;

  groundedSince = 0;

  queueSound(
    60
  );

  syncGameEventBits();

  gameStateDirty = true;

  Serial.println(
    "GAME PAUSED"
  );
}


// Resume game
void resumeGame() {

  if (
    !gameStarted ||
    gameOver ||
    !paused
  ) {

    return;
  }

  paused = false;

  groundedSince = 0;

  lastFallTime =
    millis();

  queueSound(
    60
  );

  syncGameEventBits();

  gameStateDirty = true;

  Serial.println(
    "GAME RESUMED"
  );
}


// Start a fresh game
void startNewGame() {

  saveHighScoreIfNeeded();

  memset(
    board,
    0,
    sizeof(board)
  );

  score = 0;

  totalLines = 0;

  level = 1;

  gameStarted = true;

  paused = false;

  gameOver = false;

  groundedSince = 0;

  refillBag();

  nextPieceType =
    getNextFromBag();

  spawnPiece();

  queueSound(
    80
  );

  syncGameEventBits();

  gameStateDirty = true;

  Serial.println(
    "GAME STARTED"
  );
}


// Convert network text to FreeRTOS command
GameCommand commandFromText(
  String command
) {

  command.trim();

  command.toUpperCase();

  if (
    command == "LEFT" ||
    command == "A"
  ) {

    return CMD_LEFT;
  }

  if (
    command == "RIGHT" ||
    command == "D"
  ) {

    return CMD_RIGHT;
  }

  if (
    command == "ROTATE" ||
    command == "W"
  ) {

    return CMD_ROTATE;
  }

  if (
    command == "DROP" ||
    command == "X"
  ) {

    return CMD_DROP;
  }

  if (
    command == "PAUSE" ||
    command == "PAUSED" ||
    command == "P"
  ) {

    return CMD_PAUSE;
  }

  if (
    command == "RESUME"
  ) {

    return CMD_RESUME;
  }

  return CMD_NONE;
}


// Process command inside GameLogicTask
void processGameCommand(
  GameCommand command
) {

  // Physical button
  if (
    command ==
    CMD_BUTTON
  ) {

    uint32_t now =
      millis();

    // Debounce
    if (
      now -
      lastButtonCommandTime
      < 250
    ) {

      return;
    }

    lastButtonCommandTime =
      now;

    // Start or restart
    if (
      !gameStarted ||
      gameOver
    ) {

      startNewGame();

      return;
    }

    // Resume
    if (paused) {

      resumeGame();

      return;
    }

    // Pause
    pauseGame();

    return;
  }

  // Gestures cannot start the game
  if (!gameStarted) {
    return;
  }

  // Gestures cannot restart game over
  if (gameOver) {
    return;
  }

  // While paused, an open left/right hand only resumes
  if (paused) {

    if (
      command == CMD_LEFT ||
      command == CMD_RIGHT ||
      command == CMD_RESUME
    ) {

      resumeGame();
    }

    return;
  }

  switch (command) {

    case CMD_LEFT:

      movePiece(-1);

      break;

    case CMD_RIGHT:

      movePiece(1);

      break;

    case CMD_ROTATE:

      rotatePiece();

      break;

    case CMD_DROP:

      hardDrop();

      break;

    case CMD_PAUSE:

      pauseGame();

      break;

    default:

      break;
  }
}


// Check collision using DisplayTask snapshot
bool snapshotCanPlace(
  const GameSnapshot &snapshot,
  uint8_t type,
  uint8_t rotation,
  int8_t px,
  int8_t py
) {

  for (
    int i = 0;
    i < 4;
    i++
  ) {

    int x =
      px +
      SHAPES
      [type]
      [rotation]
      [i][0];

    int y =
      py +
      SHAPES
      [type]
      [rotation]
      [i][1];

    if (
      x < 0 ||
      x >= BOARD_WIDTH
    ) {

      return false;
    }

    if (
      y >= BOARD_HEIGHT
    ) {

      return false;
    }

    if (y < 0) {
      continue;
    }

    if (
      snapshot.board[y][x]
      != 0
    ) {

      return false;
    }
  }

  return true;
}


// Draw permanent game frame
void drawGameShell() {

  tft.fillScreen(
    ST77XX_BLACK
  );

  drawHeader();

  tft.drawRect(
    BOARD_X - 1,
    BOARD_Y - 1,
    BOARD_WIDTH * CELL_SIZE + 2,
    BOARD_HEIGHT * CELL_SIZE + 2,
    ST77XX_WHITE
  );
}


// Waiting-for-button screen
void drawWaitingScreen() {

  tft.fillScreen(
    ST77XX_BLACK
  );

  drawHeader();

  tft.drawLine(
    4,
    18,
    123,
    18,
    ST77XX_WHITE
  );

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    tft.setTextColor(
      ST77XX_GREEN
    );

    tft.setCursor(
      12,
      35
    );

    tft.print(
      "WIFI CONNECTED"
    );

    tft.setTextColor(
      ST77XX_WHITE
    );

    tft.setCursor(
      5,
      52
    );

    tft.print(
      WiFi.SSID()
    );

    tft.setCursor(
      5,
      67
    );

    tft.print(
      WiFi.localIP()
    );

  } else {

    tft.setTextColor(
      ST77XX_YELLOW
    );

    tft.setCursor(
      18,
      46
    );

    tft.print(
      "OFFLINE MODE"
    );
  }

  tft.drawRect(
    17,
    101,
    94,
    35,
    ST77XX_CYAN
  );

  tft.setTextColor(
    ST77XX_WHITE
  );

  tft.setCursor(
    31,
    110
  );

  tft.print(
    "PRESS BUTTON"
  );

  tft.setCursor(
    43,
    122
  );

  tft.print(
    "TO START"
  );
}


// Draw one board cell
void drawCell(
  int col,
  int row,
  uint16_t color
) {

  int x =
    BOARD_X +
    col *
    CELL_SIZE;

  int y =
    BOARD_Y +
    row *
    CELL_SIZE;

  tft.drawRect(
    x,
    y,
    CELL_SIZE,
    CELL_SIZE,
    GRID_COLOR
  );

  tft.fillRect(
    x + 1,
    y + 1,
    CELL_SIZE - 2,
    CELL_SIZE - 2,
    color
  );
}


// Draw locked blocks
void drawSnapshotBoard(
  const GameSnapshot &snapshot
) {

  for (
    int row = 0;
    row < BOARD_HEIGHT;
    row++
  ) {

    for (
      int col = 0;
      col < BOARD_WIDTH;
      col++
    ) {

      uint8_t value =
        snapshot.board[row][col];

      if (
        value == 0
      ) {

        drawCell(
          col,
          row,
          ST77XX_BLACK
        );

      } else {

        drawCell(
          col,
          row,
          PIECE_COLORS[
            value - 1
          ]
        );
      }
    }
  }
}


// Calculate ghost Y
int8_t snapshotGhostY(
  const GameSnapshot &snapshot
) {

  int8_t ghostY =
    snapshot.currentPiece.y;

  while (
    snapshotCanPlace(
      snapshot,
      snapshot.currentPiece.type,
      snapshot.currentPiece.rotation,
      snapshot.currentPiece.x,
      ghostY + 1
    )
  ) {

    ghostY++;
  }

  return ghostY;
}


// Draw ghost
void drawSnapshotGhost(
  const GameSnapshot &snapshot
) {

  if (
    snapshot.paused ||
    snapshot.gameOver
  ) {

    return;
  }

  int8_t ghostY =
    snapshotGhostY(
      snapshot
    );

  if (
    ghostY ==
    snapshot.currentPiece.y
  ) {

    return;
  }

  for (
    int i = 0;
    i < 4;
    i++
  ) {

    int col =
      snapshot.currentPiece.x +
      SHAPES
      [snapshot.currentPiece.type]
      [snapshot.currentPiece.rotation]
      [i][0];

    int row =
      ghostY +
      SHAPES
      [snapshot.currentPiece.type]
      [snapshot.currentPiece.rotation]
      [i][1];

    if (
      row < 0 ||
      row >= BOARD_HEIGHT
    ) {

      continue;
    }

    int x =
      BOARD_X +
      col *
      CELL_SIZE;

    int y =
      BOARD_Y +
      row *
      CELL_SIZE;

    tft.drawRect(
      x + 1,
      y + 1,
      CELL_SIZE - 2,
      CELL_SIZE - 2,
      GHOST_COLOR
    );
  }
}


// Draw active piece
void drawSnapshotCurrentPiece(
  const GameSnapshot &snapshot
) {

  if (
    snapshot.gameOver
  ) {

    return;
  }

  uint16_t color =
    PIECE_COLORS[
      snapshot.currentPiece.type
    ];

  for (
    int i = 0;
    i < 4;
    i++
  ) {

    int col =
      snapshot.currentPiece.x +
      SHAPES
      [snapshot.currentPiece.type]
      [snapshot.currentPiece.rotation]
      [i][0];

    int row =
      snapshot.currentPiece.y +
      SHAPES
      [snapshot.currentPiece.type]
      [snapshot.currentPiece.rotation]
      [i][1];

    if (
      row < 0 ||
      row >= BOARD_HEIGHT
    ) {

      continue;
    }

    drawCell(
      col,
      row,
      color
    );
  }
}


// Draw next piece
void drawSnapshotNextPiece(
  const GameSnapshot &snapshot
) {

  const int px = 81;
  const int py = 99;
  const int size = 4;

  tft.fillRect(
    78,
    98,
    48,
    21,
    ST77XX_BLACK
  );

  uint16_t color =
    PIECE_COLORS[
      snapshot.nextPieceType
    ];

  for (
    int i = 0;
    i < 4;
    i++
  ) {

    int x =
      px +
      SHAPES
      [snapshot.nextPieceType]
      [0]
      [i][0] *
      size;

    int y =
      py +
      SHAPES
      [snapshot.nextPieceType]
      [0]
      [i][1] *
      size;

    tft.fillRect(
      x,
      y,
      size - 1,
      size - 1,
      color
    );
  }
}


// Draw live system statistics
void drawSystemMonitor() {

  tft.fillRect(
    MONITOR_X,
    MONITOR_Y,
    MONITOR_W,
    MONITOR_H,
    ST77XX_BLACK
  );

  tft.drawRect(
    MONITOR_X,
    MONITOR_Y,
    MONITOR_W,
    MONITOR_H,
    ST77XX_WHITE
  );

  tft.setTextSize(1);

  // Core 0 CPU
  tft.setTextColor(
    ST77XX_CYAN
  );

  tft.setCursor(
    79,
    125
  );

  tft.print(
    "C0:"
  );

  if (
    cpuMonitorReady
  ) {

    tft.print(
      monitorCPU0
    );

    tft.print("%");

  } else {

    tft.print(
      "NA"
    );
  }

  // Core 1 CPU
  tft.setCursor(
    79,
    133
  );

  tft.print(
    "C1:"
  );

  if (
    cpuMonitorReady
  ) {

    tft.print(
      monitorCPU1
    );

    tft.print("%");

  } else {

    tft.print(
      "NA"
    );
  }

  // RAM usage
  tft.setTextColor(
    ST77XX_GREEN
  );

  tft.setCursor(
    79,
    141
  );

  tft.print(
    "RAM:"
  );

  tft.print(
    monitorRAM
  );

  tft.print("%");

  // Queue-to-game dispatch latency
  tft.setTextColor(
    ST77XX_YELLOW
  );

  tft.setCursor(
    79,
    149
  );

  tft.print(
    "LAT:"
  );

  if (
    monitorLatencyValid
  ) {

    if (
      monitorLatencyMs >
      99
    ) {

      tft.print(
        "99+"
      );

    } else {

      tft.print(
        monitorLatencyMs
      );
    }

    tft.print(
      "ms"
    );

  } else {

    tft.print(
      "--"
    );
  }
}


// Draw stats / next piece
void drawSnapshotInfo(
  const GameSnapshot &snapshot
) {

  tft.fillRect(
    76,
    18,
    52,
    105,
    ST77XX_BLACK
  );

  tft.setTextSize(1);

  // Score
  tft.setTextColor(
    ST77XX_GREEN
  );

  tft.setCursor(
    INFO_X,
    21
  );

  tft.print(
    "SCORE"
  );

  tft.setTextColor(
    ST77XX_WHITE
  );

  tft.setCursor(
    INFO_X,
    31
  );

  tft.print(
    snapshot.score
  );

  // Lines
  tft.setTextColor(
    ST77XX_YELLOW
  );

  tft.setCursor(
    INFO_X,
    43
  );

  tft.print(
    "LINES"
  );

  tft.setTextColor(
    ST77XX_WHITE
  );

  tft.setCursor(
    INFO_X,
    53
  );

  tft.print(
    snapshot.totalLines
  );

  // Level
  tft.setTextColor(
    ST77XX_CYAN
  );

  tft.setCursor(
    INFO_X,
    65
  );

  tft.print(
    "LEVEL"
  );

  tft.setTextColor(
    ST77XX_WHITE
  );

  tft.setCursor(
    INFO_X,
    75
  );

  tft.print(
    snapshot.level
  );

  // Next piece
  tft.setTextColor(
    ST77XX_MAGENTA
  );

  tft.setCursor(
    INFO_X,
    87
  );

  tft.print(
    "NEXT"
  );

  drawSnapshotNextPiece(
    snapshot
  );

  drawSystemMonitor();
}


// Draw pause / game over overlays
void drawSnapshotOverlay(
  const GameSnapshot &snapshot
) {

  if (
    snapshot.paused
  ) {

    tft.fillRect(
      7,
      65,
      61,
      41,
      ST77XX_BLACK
    );

    tft.drawRect(
      7,
      65,
      61,
      41,
      ST77XX_YELLOW
    );

    tft.setTextSize(1);

    tft.setTextColor(
      ST77XX_YELLOW
    );

    tft.setCursor(
      20,
      72
    );

    tft.print(
      "PAUSED"
    );

    tft.setTextColor(
      ST77XX_WHITE
    );

    tft.setCursor(
      11,
      85
    );

    tft.print(
      "OPEN HAND"
    );

    tft.setCursor(
      14,
      96
    );

    tft.print(
      "TO RESUME"
    );
  }

  if (
    snapshot.gameOver
  ) {

    tft.fillRect(
      6,
      61,
      63,
      51,
      ST77XX_BLACK
    );

    tft.drawRect(
      6,
      61,
      63,
      51,
      ST77XX_RED
    );

    tft.setTextSize(1);

    tft.setTextColor(
      ST77XX_RED
    );

    tft.setCursor(
      11,
      68
    );

    tft.print(
      "GAME OVER"
    );

    tft.setTextColor(
      ST77XX_WHITE
    );

    tft.setCursor(
      11,
      82
    );

    tft.print(
      "HI:"
    );

    tft.print(
      snapshot.highScore
    );

    tft.setCursor(
      11,
      96
    );

    tft.print(
      "BTN=NEW"
    );
  }
}


// Render complete game snapshot
void renderSnapshot(
  const GameSnapshot &snapshot
) {

  drawSnapshotBoard(
    snapshot
  );

  drawSnapshotGhost(
    snapshot
  );

  drawSnapshotCurrentPiece(
    snapshot
  );

  drawSnapshotInfo(
    snapshot
  );

  drawSnapshotOverlay(
    snapshot
  );
}


// Send command through FreeRTOS command queue
void enqueueCommand(
  GameCommand command
) {

  if (
    commandQueue == nullptr ||
    command == CMD_NONE
  ) {

    return;
  }

  EventBits_t bits =
    xEventGroupGetBits(
      systemEvents
    );

  // Gestures cannot start game
  if (
    !(bits & EVT_STARTED)
  ) {

    return;
  }

  // Ignore gestures after game over
  if (
    bits & EVT_GAMEOVER
  ) {

    return;
  }

  CommandMessage message;

  message.command =
    command;

  message.queuedAt =
    xTaskGetTickCount();

  xQueueSend(
    commandQueue,
    &message,
    0
  );
}


// Physical button interrupt
void IRAM_ATTR buttonISR() {

  if (
    commandQueue == nullptr
  ) {

    return;
  }

  CommandMessage message;

  message.command =
    CMD_BUTTON;

  message.queuedAt =
    xTaskGetTickCountFromISR();

  BaseType_t higherPriorityTaskWoken =
    pdFALSE;

  xQueueSendFromISR(
    commandQueue,
    &message,
    &higherPriorityTaskWoken
  );

  if (
    higherPriorityTaskWoken
  ) {

    portYIELD_FROM_ISR();
  }
}


// Gesture / WiFi communication task
void GestureTask(
  void *parameter
) {

  String serialBuffer = "";

  bool previousConnected =
    (
      WiFi.status() ==
      WL_CONNECTED
    );

  uint32_t lastReconnectAttempt = 0;

  for (;;) {

    bool connected =
      (
        WiFi.status() ==
        WL_CONNECTED
      );

    // WiFi lost
    if (
      !connected &&
      previousConnected
    ) {

      Serial.println(
        "WiFi LOST."
      );

      xEventGroupClearBits(
        systemEvents,
        EVT_WIFI
      );

      if (
        udpStarted
      ) {

        udp.stop();

        udpStarted = false;
      }
    }

    // WiFi recovered
    if (
      connected &&
      !previousConnected
    ) {

      Serial.println(
        "WiFi RECONNECTED."
      );

      Serial.print(
        "ESP32 IP: "
      );

      Serial.println(
        WiFi.localIP()
      );

      xEventGroupSetBits(
        systemEvents,
        EVT_WIFI
      );

      startUDP();
    }

    previousConnected =
      connected;

    // Attempt reconnection
    if (
      !connected &&
      activeSSID.length() > 0 &&
      millis() -
      lastReconnectAttempt >= 5000
    ) {

      lastReconnectAttempt =
        millis();

      Serial.println(
        "Retrying WiFi..."
      );

      if (
        activePassword.length() == 0
      ) {

        WiFi.begin(
          activeSSID.c_str()
        );

      } else {

        WiFi.begin(
          activeSSID.c_str(),
          activePassword.c_str()
        );
      }
    }

    // UDP gestures
    if (
      connected &&
      udpStarted
    ) {

      int packetSize =
        udp.parsePacket();

      while (
        packetSize > 0
      ) {

        char buffer[32];

        int length =
          udp.read(
            buffer,
            sizeof(buffer) - 1
          );

        if (
          length > 0
        ) {

          buffer[length] =
            '\0';

          String text =
            String(buffer);

          text.trim();

          Serial.print(
            "UDP -> "
          );

          Serial.println(
            text
          );

          GameCommand command =
            commandFromText(
              text
            );

          enqueueCommand(
            command
          );
        }

        packetSize =
          udp.parsePacket();
      }
    }

    // Serial fallback controls
    while (
      Serial.available()
    ) {

      char c =
        Serial.read();

      if (
        c == '\r' ||
        c == '\n'
      ) {

        if (
          serialBuffer.length() >
          0
        ) {

          String text =
            serialBuffer;

          serialBuffer = "";

          text.trim();

          text.toUpperCase();

          if (
            text == "IP" ||
            text == "NETWORK"
          ) {

            printNetworkInfo();

          } else {

            GameCommand command =
              commandFromText(
                text
              );

            enqueueCommand(
              command
            );
          }
        }

      } else {

        if (
          serialBuffer.length() <
          30
        ) {

          serialBuffer += c;
        }
      }
    }

    vTaskDelay(
      pdMS_TO_TICKS(5)
    );
  }
}


// Game logic FreeRTOS task
void GameLogicTask(
  void *parameter
) {

  CommandMessage message;

  for (;;) {

    if (
      xQueueReceive(
        commandQueue,
        &message,
        pdMS_TO_TICKS(10)
      ) ==
      pdTRUE
    ) {

      // Measure command queue dispatch latency
      TickType_t now =
        xTaskGetTickCount();

      TickType_t difference =
        now -
        message.queuedAt;

      uint32_t latency =
        difference *
        portTICK_PERIOD_MS;

      if (
        latency > 999
      ) {

        latency = 999;
      }

      monitorLatencyMs =
        (uint16_t)latency;

      monitorLatencyValid =
        true;

      processGameCommand(
        message.command
      );

      // Drain remaining commands
      while (
        xQueueReceive(
          commandQueue,
          &message,
          0
        ) ==
        pdTRUE
      ) {

        now =
          xTaskGetTickCount();

        difference =
          now -
          message.queuedAt;

        latency =
          difference *
          portTICK_PERIOD_MS;

        if (
          latency > 999
        ) {

          latency = 999;
        }

        monitorLatencyMs =
          (uint16_t)latency;

        monitorLatencyValid =
          true;

        processGameCommand(
          message.command
        );
      }
    }

    updateGamePhysics();

    if (
      gameStateDirty
    ) {

      publishGameSnapshot();
    }

    vTaskDelay(
      pdMS_TO_TICKS(1)
    );
  }
}


// TFT rendering task
void DisplayTask(
  void *parameter
) {

  GameSnapshot snapshot;

  bool gameShellShown =
    false;

  for (;;) {

    if (
      xQueueReceive(
        displayQueue,
        &snapshot,
        portMAX_DELAY
      ) ==
      pdTRUE
    ) {

      if (
        xSemaphoreTake(
          tftMutex,
          pdMS_TO_TICKS(500)
        ) ==
        pdTRUE
      ) {

        if (
          snapshot.gameStarted &&
          !gameShellShown
        ) {

          drawGameShell();

          gameShellShown =
            true;
        }

        renderSnapshot(
          snapshot
        );

        xSemaphoreGive(
          tftMutex
        );
      }
    }
  }
}


// Buzzer task
void AudioTask(
  void *parameter
) {

  uint16_t durationMs;

  for (;;) {

    if (
      xQueueReceive(
        audioQueue,
        &durationMs,
        portMAX_DELAY
      ) ==
      pdTRUE
    ) {

      digitalWrite(
        BUZZER_PIN,
        HIGH
      );

      vTaskDelay(
        pdMS_TO_TICKS(
          durationMs
        )
      );

      digitalWrite(
        BUZZER_PIN,
        LOW
      );
    }
  }
}


// Print task stack information
void printTaskStacks() {

  Serial.println(
    "Task stack minimum-free:"
  );

  Serial.print(
    "Gesture: "
  );

  Serial.println(
    uxTaskGetStackHighWaterMark(
      gestureTaskHandle
    )
  );

  Serial.print(
    "Game:    "
  );

  Serial.println(
    uxTaskGetStackHighWaterMark(
      gameTaskHandle
    )
  );

  Serial.print(
    "Display: "
  );

  Serial.println(
    uxTaskGetStackHighWaterMark(
      displayTaskHandle
    )
  );

  Serial.print(
    "Audio:   "
  );

  Serial.println(
    uxTaskGetStackHighWaterMark(
      audioTaskHandle
    )
  );

  Serial.print(
    "Monitor: "
  );

  Serial.println(
    uxTaskGetStackHighWaterMark(
      monitorTaskHandle
    )
  );
}


// System monitor task
void MonitorTask(
  void *parameter
) {

  uint32_t previousIdle0 =
    idleCounter0;

  uint32_t previousIdle1 =
    idleCounter1;

  uint32_t previousTime =
    millis();

  uint8_t reportCounter = 0;

  TickType_t lastWakeTime =
    xTaskGetTickCount();

  for (;;) {

    // Exact 1-second monitor period
    vTaskDelayUntil(
      &lastWakeTime,
      pdMS_TO_TICKS(1000)
    );

    uint32_t now =
      millis();

    uint32_t elapsed =
      now -
      previousTime;

    uint32_t currentIdle0 =
      idleCounter0;

    uint32_t currentIdle1 =
      idleCounter1;

    uint32_t delta0 =
      currentIdle0 -
      previousIdle0;

    uint32_t delta1 =
      currentIdle1 -
      previousIdle1;

    previousIdle0 =
      currentIdle0;

    previousIdle1 =
      currentIdle1;

    previousTime =
      now;

    // Calculate live CPU usage
    if (
      cpuMonitorReady &&
      elapsed > 0
    ) {

      uint32_t normalized0 =
        (
          (uint64_t)delta0 *
          1000ULL
        ) /
        elapsed;

      uint32_t normalized1 =
        (
          (uint64_t)delta1 *
          1000ULL
        ) /
        elapsed;

      uint8_t rawCPU0 =
        calculateCPUUsage(
          normalized0,
          idleBaseline0
        );

      uint8_t rawCPU1 =
        calculateCPUUsage(
          normalized1,
          idleBaseline1
        );

      // Smooth changing percentages
      monitorCPU0 =
        (
          monitorCPU0 * 2 +
          rawCPU0
        ) /
        3;

      monitorCPU1 =
        (
          monitorCPU1 * 2 +
          rawCPU1
        ) /
        3;
    }

    // Actual current RAM usage
    monitorRAM =
      getRAMUsagePercent();

    EventBits_t bits =
      xEventGroupGetBits(
        systemEvents
      );

    // Update TFT statistics during game
    if (
      bits &
      EVT_STARTED
    ) {

      if (
        xSemaphoreTake(
          tftMutex,
          pdMS_TO_TICKS(100)
        ) ==
        pdTRUE
      ) {

        drawSystemMonitor();

        xSemaphoreGive(
          tftMutex
        );
      }
    }

    reportCounter++;

    // Detailed Serial report every five seconds
    if (
      reportCounter >= 5
    ) {

      reportCounter = 0;

      Serial.println();

      Serial.print(
        "[MON] C0: "
      );

      Serial.print(
        monitorCPU0
      );

      Serial.print(
        "% | C1: "
      );

      Serial.print(
        monitorCPU1
      );

      Serial.print(
        "% | RAM: "
      );

      Serial.print(
        monitorRAM
      );

      Serial.print(
        "% | LAT: "
      );

      if (
        monitorLatencyValid
      ) {

        Serial.print(
          monitorLatencyMs
        );

        Serial.println(
          " ms"
        );

      } else {

        Serial.println(
          "--"
        );
      }

      printTaskStacks();

      Serial.println();
    }
  }
}


// Create RTOS queues, mutex and event group
bool createRTOSObjects() {

  commandQueue =
    xQueueCreate(
      COMMAND_QUEUE_LENGTH,
      sizeof(CommandMessage)
    );

  audioQueue =
    xQueueCreate(
      AUDIO_QUEUE_LENGTH,
      sizeof(uint16_t)
    );

  // Queue length 1 allows overwrite with latest screen
  displayQueue =
    xQueueCreate(
      1,
      sizeof(GameSnapshot)
    );

  tftMutex =
    xSemaphoreCreateMutex();

  systemEvents =
    xEventGroupCreate();

  if (
    commandQueue == nullptr ||
    audioQueue == nullptr ||
    displayQueue == nullptr ||
    tftMutex == nullptr ||
    systemEvents == nullptr
  ) {

    return false;
  }

  return true;
}


// Create project FreeRTOS tasks
bool createTasks() {

  BaseType_t result;

  // UDP and gesture communication
  result =
    xTaskCreatePinnedToCore(
      GestureTask,
      "GestureTask",
      6144,
      nullptr,
      4,
      &gestureTaskHandle,
      0
    );

  if (
    result != pdPASS
  ) {
    return false;
  }

  // Main game logic
  result =
    xTaskCreatePinnedToCore(
      GameLogicTask,
      "GameLogic",
      8192,
      nullptr,
      4,
      &gameTaskHandle,
      1
    );

  if (
    result != pdPASS
  ) {
    return false;
  }

  // TFT rendering
  result =
    xTaskCreatePinnedToCore(
      DisplayTask,
      "DisplayTask",
      8192,
      nullptr,
      3,
      &displayTaskHandle,
      1
    );

  if (
    result != pdPASS
  ) {
    return false;
  }

  // Buzzer
  result =
    xTaskCreatePinnedToCore(
      AudioTask,
      "AudioTask",
      2048,
      nullptr,
      2,
      &audioTaskHandle,
      0
    );

  if (
    result != pdPASS
  ) {
    return false;
  }

  // Performance monitor
  result =
    xTaskCreatePinnedToCore(
      MonitorTask,
      "MonitorTask",
      4096,
      nullptr,
      1,
      &monitorTaskHandle,
      0
    );

  if (
    result != pdPASS
  ) {
    return false;
  }

  return true;
}


// Arduino setup
void setup() {

  Serial.begin(
    115200
  );

  delay(700);

  // Buzzer
  pinMode(
    BUZZER_PIN,
    OUTPUT
  );

  digitalWrite(
    BUZZER_PIN,
    LOW
  );

  // TFT
  tft.initR(
    INITR_BLACKTAB
  );

  tft.setRotation(0);

  tft.fillScreen(
    ST77XX_BLACK
  );

  // Run final project boot animation
  runBootAnimation();

  // Open persistent NVS storage
  preferences.begin(
    "gestureos",
    false
  );

  highScore =
    preferences.getUInt(
      "highscore",
      0
    );

  // Start WiFi system
  Network.begin();

  WiFi.mode(
    WIFI_STA
  );

  Serial.println();

  Serial.println(
    "GestureOS Tetris"
  );

  Serial.println(
    "CSE323.7 - Group 8"
  );

  Serial.println();

  Serial.println(
    "Members:"
  );

  Serial.println(
    "Sabiha Binte Siraj"
  );

  Serial.println(
    "Sabbir Ahamed"
  );

  Serial.println(
    "Tarif Bin Mehedi"
  );

  Serial.println();

  Serial.print(
    "Saved High Score: "
  );

  Serial.println(
    highScore
  );

  // Always scan and allow user to select WiFi
  bool connected =
    selectWiFi();

  if (
    connected
  ) {

    startUDP();
  }

  // Create FreeRTOS objects
  if (
    !createRTOSObjects()
  ) {

    Serial.println(
      "FATAL: RTOS object creation failed."
    );

    while (true) {
      delay(1000);
    }
  }

  // Set WiFi event bit if online
  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    xEventGroupSetBits(
      systemEvents,
      EVT_WIFI
    );
  }

  // Initialize dual-core monitoring
  setupCPUMonitor();

  // Show game-ready screen
  drawWaitingScreen();

  // Create FreeRTOS application tasks
  if (
    !createTasks()
  ) {

    Serial.println(
      "FATAL: task creation failed."
    );

    while (true) {
      delay(1000);
    }
  }

  // Configure button after queue exists
  pinMode(
    BUTTON_PIN,
    INPUT_PULLUP
  );

  attachInterrupt(
    digitalPinToInterrupt(
      BUTTON_PIN
    ),
    buttonISR,
    FALLING
  );

  Serial.println();

  Serial.println(
    "================================"
  );

  Serial.println(
    " GestureOS FreeRTOS READY"
  );

  Serial.println(
    "================================"
  );

  Serial.println();

  Serial.println(
    "GestureTask : Core 0 Priority 4"
  );

  Serial.println(
    "GameLogic   : Core 1 Priority 4"
  );

  Serial.println(
    "DisplayTask : Core 1 Priority 3"
  );

  Serial.println(
    "AudioTask   : Core 0 Priority 2"
  );

  Serial.println(
    "MonitorTask : Core 0 Priority 1"
  );

  Serial.println();

  Serial.println(
    "Command Queue: ACTIVE"
  );

  Serial.println(
    "Display Queue: ACTIVE"
  );

  Serial.println(
    "Audio Queue: ACTIVE"
  );

  Serial.println(
    "TFT Mutex: ACTIVE"
  );

  Serial.println(
    "Event Group: ACTIVE"
  );

  Serial.println();

  if (
    WiFi.status() ==
    WL_CONNECTED
  ) {

    Serial.print(
      "WiFi: "
    );

    Serial.println(
      WiFi.SSID()
    );

    Serial.print(
      "ESP32 IP: "
    );

    Serial.println(
      WiFi.localIP()
    );

    Serial.print(
      "UDP Port: "
    );

    Serial.println(
      UDP_PORT
    );
  }

  Serial.println();

  Serial.println(
    "PRESS BUTTON TO START."
  );

  Serial.println();
}


// Arduino loop is deliberately unused
void loop() {

  // All real project work runs inside FreeRTOS tasks
  vTaskDelay(
    pdMS_TO_TICKS(1000)
  );
}
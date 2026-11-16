
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <RF24.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// =====================================================
// 1. OLED CONFIGURATION
// =====================================================

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_ADDR 0x3C

#define OLED_SDA 8
#define OLED_SCL 9

Adafruit_SSD1306 display(
  SCREEN_WIDTH,
  SCREEN_HEIGHT,
  &Wire,
  -1
);

// =====================================================
// 2. BUTTON PINS
// All buttons use INPUT_PULLUP
// =====================================================

#define BTN_UP       4
#define BTN_DOWN     5
#define BTN_SELECT   6
#define BTN_BACK     7
#define BTN_RECORD  13
#define BTN_PLAY    14

// =====================================================
// 3. JOYSTICK PINS
// =====================================================

#define J1_X   1
#define J1_Y   2
#define J1_SW  3

#define J2_X  10
#define J2_Y  11
#define J2_SW 12

#define JOY_CENTER   2048
#define JOY_DEADZONE 300

// =====================================================
// 4. NRF24L01 CONFIGURATION
// Keep the confirmed radio wiring and settings
// =====================================================

#define NRF_CE   15
#define NRF_CSN  16
#define NRF_SCK  17
#define NRF_MOSI 18
#define NRF_MISO 21

RF24 radio(NRF_CE, NRF_CSN);

const byte radioAddress[6] = "ARM01";

const uint8_t RADIO_CHANNEL = 108;

const unsigned long SEND_INTERVAL = 30;
const unsigned long SCREEN_INTERVAL = 100;
const unsigned long DEBOUNCE_MS = 30;

// Commands must be compatible with the receiver.
enum Command : uint8_t {
  CMD_CONTROL = 0,
  CMD_HOME    = 1,
  CMD_RECORD  = 2,
  CMD_PLAY    = 3,
  CMD_STOP    = 4
};

// Do not change this packet layout without changing
// the receiver to match it.
struct ControlData {
  int8_t baseDirection;
  int8_t shoulderDirection;
  int8_t elbowDirection;
  int8_t gripperDirection;
  uint8_t command;
  uint8_t fileNumber;
};

ControlData txData = {0, 0, 0, 0, CMD_CONTROL, 1};

// =====================================================
// 5. BUTTON DEBOUNCING
// =====================================================

struct Button {
  uint8_t pin;
  bool stableState;
  bool lastReading;
  unsigned long lastChange;
};

Button buttonUp;
Button buttonDown;
Button buttonSelect;
Button buttonBack;
Button buttonRecord;
Button buttonPlay;

void initializeButton(Button &button, uint8_t pin) {
  button.pin = pin;

  pinMode(pin, INPUT_PULLUP);

  bool initialState = digitalRead(pin);

  button.stableState = initialState;
  button.lastReading = initialState;
  button.lastChange = millis();
}

bool buttonPressed(Button &button) {
  bool reading = digitalRead(button.pin);

  if (reading != button.lastReading) {
    button.lastChange = millis();
    button.lastReading = reading;
  }

  if (millis() - button.lastChange >= DEBOUNCE_MS) {
    if (reading != button.stableState) {
      button.stableState = reading;

      // INPUT_PULLUP: LOW means pressed.
      if (button.stableState == LOW) {
        return true;
      }
    }
  }

  return false;
}

// =====================================================
// 6. SCREEN STATES
// =====================================================

enum ScreenState {
  SPLASH_SCREEN,
  MAIN_MENU,
  LIVE_MODE,
  RECORDING_SCREEN,
  PLAY_FILE_SELECT,
  RECORDED_FILES,
  HOME_SCREEN,
  GAMES_MENU,
  JOYSTICK_TESTER,
  ARM_CONTROLLER,
  WIRELESS_SCREEN
};

ScreenState currentScreen = SPLASH_SCREEN;

// Main menu
const char *mainMenuItems[] = {
  "Live Mode",
  "Record Motion",
  "Play Motion",
  "Recorded Files",
  "Home Position",
  "Joystick Games",
  "Wireless / nRF Range"
};

const uint8_t MAIN_MENU_COUNT = 7;

uint8_t mainMenuIndex = 0;
uint8_t gamesMenuIndex = 0;

const char *gameMenuItems[] = {
  "Joystick X/Y Tester",
  "Arm Controller"
};

const uint8_t GAMES_MENU_COUNT = 2;

// File number range.
// The receiver stores files as REC_01.csv, REC_02.csv, etc.
uint8_t selectedFile = 1;
uint8_t recordingFile = 1;

// =====================================================
// 7. RECORDING AND PLAYBACK STATE
// =====================================================

bool isRecording = false;
bool playbackActive = false;

// Radio commands such as PLAY, STOP and HOME are sent
// temporarily, then ordinary control packets resume.
Command temporaryCommand = CMD_CONTROL;
unsigned long temporaryCommandUntil = 0;

void requestCommand(Command command,
                    unsigned long durationMs = 300) {
  temporaryCommand = command;
  temporaryCommandUntil = millis() + durationMs;
}

Command getTransmitCommand() {
  if (isRecording) {
    return CMD_RECORD;
  }

  if (temporaryCommand != CMD_CONTROL) {
    if ((long)(temporaryCommandUntil - millis()) > 0) {
      return temporaryCommand;
    }

    temporaryCommand = CMD_CONTROL;
  }

  return CMD_CONTROL;
}

// =====================================================
// 8. NRF ACK / LINK QUALITY HISTORY
// These bars represent recent acknowledged transmissions,
// not a true RF signal-strength or RSSI measurement.
// =====================================================

const uint8_t ACK_HISTORY_SIZE = 20;

bool ackHistory[ACK_HISTORY_SIZE] = {false};
uint8_t ackPosition = 0;
uint8_t ackCount = 0;
uint8_t ackSuccessCount = 0;

bool lastAckSuccess = false;
bool radioReady = false;

void recordAck(bool success) {
  if (ackCount == ACK_HISTORY_SIZE) {
    if (ackHistory[ackPosition]) {
      ackSuccessCount--;
    }
  } else {
    ackCount++;
  }

  ackHistory[ackPosition] = success;

  if (success) {
    ackSuccessCount++;
  }

  ackPosition = (ackPosition + 1) % ACK_HISTORY_SIZE;
  lastAckSuccess = success;
}

uint8_t getLinkBars() {
  if (ackCount == 0) {
    return 0;
  }

  return (ackSuccessCount * 4 + ackCount - 1) / ackCount;
}

// =====================================================
// 9. JOYSTICK READING
// =====================================================

int joystick1X = JOY_CENTER;
int joystick1Y = JOY_CENTER;
int joystick2X = JOY_CENTER;
int joystick2Y = JOY_CENTER;

bool joystick1Switch = false;
bool joystick2Switch = false;

int8_t axisDirection(int value, bool invert = false) {
  int8_t direction = 0;

  if (value < JOY_CENTER - JOY_DEADZONE) {
    direction = -1;
  } else if (value > JOY_CENTER + JOY_DEADZONE) {
    direction = 1;
  }

  if (invert) {
    direction = -direction;
  }

  return direction;
}

void readJoysticks() {
  joystick1X = analogRead(J1_X);
  joystick1Y = analogRead(J1_Y);
  joystick2X = analogRead(J2_X);
  joystick2Y = analogRead(J2_Y);

  joystick1Switch = (digitalRead(J1_SW) == LOW);
  joystick2Switch = (digitalRead(J2_SW) == LOW);

  // Preserve the confirmed joystick mapping:
  // J1 X -> Base
  // J1 Y -> Shoulder, inverted
  // J2 X -> Elbow
  // J2 Y -> Gripper, inverted

  txData.baseDirection =
    axisDirection(joystick1X, false);

  txData.shoulderDirection =
    axisDirection(joystick1Y, true);

  txData.elbowDirection =
    axisDirection(joystick2X, false);

  txData.gripperDirection =
    axisDirection(joystick2Y, true);
}

// =====================================================
// 10. RADIO TRANSMISSION
// =====================================================

unsigned long lastSendTime = 0;

void sendRadioPacket() {
  if (!radioReady) {
    return;
  }

  unsigned long now = millis();

  if (now - lastSendTime < SEND_INTERVAL) {
    return;
  }

  lastSendTime = now;

  txData.command = getTransmitCommand();
  txData.fileNumber = isRecording
                    ? recordingFile
                    : selectedFile;

  bool success = radio.write(&txData, sizeof(txData));

  recordAck(success);
}

// =====================================================
// 11. COMMON OLED DRAWING FUNCTIONS
// =====================================================

void drawLinkIndicator() {
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  if (lastAckSuccess) {
    display.setCursor(0, 0);
    display.print("NRF LINK");
  } else {
    display.setCursor(0, 0);
    display.print("NO LINK");
  }

  uint8_t bars = getLinkBars();

  for (uint8_t i = 0; i < 4; i++) {
    int x = 91 + i * 8;
    int h = 3 + i * 3;
    int y = 10 - h;

    if (i < bars) {
      display.fillRect(x, y, 5, h, SSD1306_WHITE);
    } else {
      display.drawRect(x, y, 5, h, SSD1306_WHITE);
    }
  }

  display.drawLine(0, 13, 127, 13, SSD1306_WHITE);
}

void drawHeader(const char *title) {
  drawLinkIndicator();

  display.setCursor(0, 17);
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.print(title);
}

void drawCenteredText(const char *text, int y, uint8_t size) {
  display.setTextSize(size);

  int16_t x1, y1;
  uint16_t w, h;

  display.getTextBounds(
    text, 0, y, &x1, &y1, &w, &h
  );

  int x = (SCREEN_WIDTH - (int)w) / 2;

  display.setCursor(x, y);
  display.print(text);
}

// =====================================================
// 12. ROBOTIC ARM SPLASH DRAWING
// =====================================================

void drawRobotArm() {
  // Base
  display.drawLine(9, 53, 43, 53, SSD1306_WHITE);
  display.drawLine(14, 53, 14, 58, SSD1306_WHITE);
  display.drawLine(38, 53, 38, 58, SSD1306_WHITE);
  display.drawLine(14, 58, 38, 58, SSD1306_WHITE);

  // Base rotation joint
  display.drawCircle(26, 48, 5, SSD1306_WHITE);

  // Shoulder and upper arm
  display.drawLine(26, 44, 39, 31, SSD1306_WHITE);
  display.drawLine(30, 47, 43, 34, SSD1306_WHITE);
  display.drawCircle(41, 32, 4, SSD1306_WHITE);

  // Forearm
  display.drawLine(44, 29, 57, 38, SSD1306_WHITE);
  display.drawLine(42, 34, 55, 43, SSD1306_WHITE);
  display.drawCircle(56, 41, 3, SSD1306_WHITE);

  // Gripper wrist and fingers
  display.drawLine(58, 39, 65, 32, SSD1306_WHITE);
  display.drawLine(58, 43, 65, 48, SSD1306_WHITE);
  display.drawLine(65, 32, 71, 30, SSD1306_WHITE);
  display.drawLine(65, 48, 71, 50, SSD1306_WHITE);
  display.drawLine(71, 30, 73, 34, SSD1306_WHITE);
  display.drawLine(71, 50, 73, 46, SSD1306_WHITE);

  // Joint detail
  display.drawPixel(41, 32, SSD1306_WHITE);
  display.drawPixel(56, 41, SSD1306_WHITE);
}

// =====================================================
// 13. SCREEN RENDERING
// =====================================================

void showSplashScreen() {
  display.clearDisplay();

  drawRobotArm();

  display.setTextSize(1);
  display.setCursor(76, 23);
  display.print("WIRELESS");

  display.setCursor(76, 34);
  display.print("ROBOT");

  display.setCursor(76, 44);
  display.print("ARM");

  display.drawLine(0, 61, 127, 61, SSD1306_WHITE);

  drawCenteredText("PRESS UP TO START", 1, 1);

  display.display();
}

void showMainMenu() {
  display.clearDisplay();
  drawHeader("MAIN MENU");

  // Four menu items fit on screen at a time.
  const uint8_t visibleItems = 4;

  uint8_t firstItem = 0;

  if (mainMenuIndex >= visibleItems) {
    firstItem = mainMenuIndex - visibleItems + 1;
  }

  for (uint8_t row = 0; row < visibleItems; row++) {
    uint8_t index = firstItem + row;

    if (index >= MAIN_MENU_COUNT) {
      break;
    }

    int y = 25 + row * 9;

    if (index == mainMenuIndex) {
      display.fillRect(0, y - 1, 128, 9, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(3, y);
    display.print(index + 1);
    display.print(". ");
    display.print(mainMenuItems[index]);
  }

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 59);
  display.print("UP/DN  SELECT  BACK");

  display.display();
}

void showLiveMode() {
  display.clearDisplay();
  drawHeader("LIVE MODE");

  display.setCursor(0, 19);
  display.print("BASE: ");

  if (txData.baseDirection < 0) {
    display.print("< LEFT");
  } else if (txData.baseDirection > 0) {
    display.print("> RIGHT");
  } else {
    display.print("STOP");
  }

  display.setCursor(0, 29);
  display.print("SHOULDER: ");

  if (txData.shoulderDirection < 0) {
    display.print("DOWN");
  } else if (txData.shoulderDirection > 0) {
    display.print("UP");
  } else {
    display.print("STOP");
  }

  display.setCursor(0, 39);
  display.print("ELBOW: ");

  if (txData.elbowDirection < 0) {
    display.print("LEFT");
  } else if (txData.elbowDirection > 0) {
    display.print("RIGHT");
  } else {
    display.print("STOP");
  }

  display.setCursor(0, 49);
  display.print("GRIPPER: ");

  if (txData.gripperDirection < 0) {
    display.print("OPEN");
  } else if (txData.gripperDirection > 0) {
    display.print("CLOSE");
  } else {
    display.print("STOP");
  }

  display.setCursor(0, 59);
  display.print("SELECT: MENU");

  display.display();
}

void showRecordingScreen() {
  display.clearDisplay();
  drawHeader("RECORD MOTION");

  display.setTextSize(2);
  display.setCursor(8, 23);
  display.print("RECORDING");

  display.setTextSize(1);
  display.setCursor(8, 45);
  display.print("FILE: REC_");

  if (recordingFile < 10) {
    display.print("0");
  }

  display.print(recordingFile);
  display.print(".csv");

  display.setCursor(8, 57);
  display.print("PRESS RECORD TO STOP");

  display.display();
}

void showPlayFileSelect() {
  display.clearDisplay();
  drawHeader("SELECT PLAY FILE");

  display.setCursor(5, 25);
  display.print("Selected file:");

  display.setTextSize(2);
  display.setCursor(15, 36);
  display.print("REC_");

  if (selectedFile < 10) {
    display.print("0");
  }

  display.print(selectedFile);

  display.setTextSize(1);
  display.print(".csv");

  display.setCursor(0, 57);
  display.print("UP/DN  PLAY: START");

  display.display();
}

void showRecordedFiles() {
  display.clearDisplay();
  drawHeader("RECORDED FILES");

  display.setCursor(5, 25);
  display.print("File number:");

  display.setTextSize(2);
  display.setCursor(15, 36);
  display.print("REC_");

  if (selectedFile < 10) {
    display.print("0");
  }

  display.print(selectedFile);

  display.setTextSize(1);
  display.print(".csv");

  display.setCursor(0, 57);
  display.print("UP/DN  PLAY: SELECT");

  display.display();
}

void showHomeScreen() {
  display.clearDisplay();
  drawHeader("HOME POSITION");

  display.setCursor(0, 21);
  display.print("Base       : 90");

  display.setCursor(0, 30);
  display.print("Shoulder   : 90");

  display.setCursor(0, 39);
  display.print("Elbow      : 90");

  display.setCursor(0, 48);
  display.print("Gripper    : 50");

  display.setCursor(0, 58);
  display.print("SELECT: MOVE HOME");

  display.display();
}

void showGamesMenu() {
  display.clearDisplay();
  drawHeader("JOYSTICK GAMES");

  for (uint8_t i = 0; i < GAMES_MENU_COUNT; i++) {
    int y = 29 + i * 14;

    if (i == gamesMenuIndex) {
      display.fillRect(0, y - 2, 128, 12, SSD1306_WHITE);
      display.setTextColor(SSD1306_BLACK);
    } else {
      display.setTextColor(SSD1306_WHITE);
    }

    display.setCursor(3, y);
    display.print(i == 0 ? "1. X/Y Tester" : "2. Arm Controller");
  }

  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 58);
  display.print("UP/DN SELECT BACK");

  display.display();
}

void showJoystickTester() {
  display.clearDisplay();
  drawHeader("X/Y JOYSTICK TEST");

  display.setCursor(0, 19);
  display.print("J1 X:");
  display.print(joystick1X);

  display.setCursor(67, 19);
  display.print("Y:");
  display.print(joystick1Y);

  display.setCursor(0, 30);
  display.print("J2 X:");
  display.print(joystick2X);

  display.setCursor(67, 30);
  display.print("Y:");
  display.print(joystick2Y);

  display.setCursor(0, 42);
  display.print("J1 SW:");
  display.print(joystick1Switch ? "PRESSED" : "OPEN");

  display.setCursor(0, 52);
  display.print("J2 SW:");
  display.print(joystick2Switch ? "PRESSED" : "OPEN");

  display.display();
}

void showArmController() {
  display.clearDisplay();
  drawHeader("ARM CONTROLLER");

  display.setCursor(0, 20);
  display.print("BASE: ");

  display.print(txData.baseDirection < 0 ? "LEFT" :
                txData.baseDirection > 0 ? "RIGHT" : "STOP");

  display.setCursor(0, 30);
  display.print("SHOULDER: ");

  display.print(txData.shoulderDirection < 0 ? "DOWN" :
                txData.shoulderDirection > 0 ? "UP" : "STOP");

  display.setCursor(0, 40);
  display.print("ELBOW: ");

  display.print(txData.elbowDirection < 0 ? "LEFT" :
                txData.elbowDirection > 0 ? "RIGHT" : "STOP");

  display.setCursor(0, 50);
  display.print("GRIPPER: ");

  display.print(txData.gripperDirection < 0 ? "OPEN" :
                txData.gripperDirection > 0 ? "CLOSE" : "STOP");

  display.display();
}

void showWirelessScreen() {
  display.clearDisplay();
  drawHeader("WIRELESS / NRF");

  display.setCursor(0, 21);
  display.print("Radio: nRF24L01");

  display.setCursor(0, 31);
  display.print("Speed: 250 KBPS");

  display.setCursor(0, 41);
  display.print("ACK: ");
  display.print(ackSuccessCount);
  display.print("/");
  display.print(ackCount);

  display.setCursor(0, 52);

  if (ackCount == 0) {
    display.print("Waiting for ACK...");
  } else if (lastAckSuccess) {
    display.print("Link responding");
  } else {
    display.print("No latest ACK");
  }

  display.display();
}

// =====================================================
// 14. SCREEN MANAGEMENT
// =====================================================

void renderCurrentScreen() {
  switch (currentScreen) {
    case SPLASH_SCREEN:
      showSplashScreen();
      break;

    case MAIN_MENU:
      showMainMenu();
      break;

    case LIVE_MODE:
      showLiveMode();
      break;

    case RECORDING_SCREEN:
      showRecordingScreen();
      break;

    case PLAY_FILE_SELECT:
      showPlayFileSelect();
      break;

    case RECORDED_FILES:
      showRecordedFiles();
      break;

    case HOME_SCREEN:
      showHomeScreen();
      break;

    case GAMES_MENU:
      showGamesMenu();
      break;

    case JOYSTICK_TESTER:
      showJoystickTester();
      break;

    case ARM_CONTROLLER:
      showArmController();
      break;

    case WIRELESS_SCREEN:
      showWirelessScreen();
      break;
  }
}

// =====================================================
// 15. NAVIGATION HELPERS
// =====================================================

void enterMainMenu() {
  currentScreen = MAIN_MENU;
  renderCurrentScreen();
}

void enterLiveMode() {
  currentScreen = LIVE_MODE;
  renderCurrentScreen();
}

void enterPlayFileSelection() {
  currentScreen = PLAY_FILE_SELECT;
  renderCurrentScreen();
}

void stopPlaybackAndReturnLive() {
  requestCommand(CMD_STOP);
  playbackActive = false;
  currentScreen = LIVE_MODE;
  renderCurrentScreen();
}

void startRecording() {
  if (playbackActive) {
    requestCommand(CMD_STOP);
    playbackActive = false;
  }

  // Move to the next number for a new recording.
  recordingFile++;

  if (recordingFile > 99) {
    recordingFile = 1;
  }

  selectedFile = recordingFile;
  isRecording = true;

  temporaryCommand = CMD_CONTROL;

  currentScreen = RECORDING_SCREEN;
  renderCurrentScreen();
}

void stopRecording() {
  isRecording = false;

  // A normal control packet tells the receiver that the
  // recording command has ended, as in the existing setup.
  temporaryCommand = CMD_CONTROL;

  currentScreen = LIVE_MODE;
  renderCurrentScreen();
}

void startPlayback() {
  if (isRecording) {
    stopRecording();
  }

  playbackActive = true;

  // Send PLAY briefly. The receiver should latch the command
  // and play the selected file after receiving this packet.
  requestCommand(CMD_PLAY, 300);

  currentScreen = LIVE_MODE;
  renderCurrentScreen();
}

void openSelectedMainMenuItem() {
  switch (mainMenuIndex) {
    case 0:
      enterLiveMode();
      break;

    case 1:
      startRecording();
      break;

    case 2:
      enterPlayFileSelection();
      break;

    case 3:
      currentScreen = RECORDED_FILES;
      renderCurrentScreen();
      break;

    case 4:
      currentScreen = HOME_SCREEN;
      renderCurrentScreen();
      break;

    case 5:
      currentScreen = GAMES_MENU;
      gamesMenuIndex = 0;
      renderCurrentScreen();
      break;

    case 6:
      currentScreen = WIRELESS_SCREEN;
      renderCurrentScreen();
      break;
  }
}

// =====================================================
// 16. BUTTON ACTIONS
// =====================================================

void handleButtons() {
  bool upPressed = buttonPressed(buttonUp);
  bool downPressed = buttonPressed(buttonDown);
  bool selectPressed = buttonPressed(buttonSelect);
  bool backPressed = buttonPressed(buttonBack);
  bool recordPressed = buttonPressed(buttonRecord);
  bool playPressed = buttonPressed(buttonPlay);

  // ---------------------------------------------------
  // SPLASH SCREEN
  // UP opens the main menu.
  // ---------------------------------------------------

  if (currentScreen == SPLASH_SCREEN) {
    if (upPressed) {
      enterMainMenu();
    }
    return;
  }

  // ---------------------------------------------------
  // DEDICATED RECORD BUTTON
  // ---------------------------------------------------

  if (recordPressed) {
    if (isRecording) {
      stopRecording();
    } else {
      startRecording();
    }
  }

  // ---------------------------------------------------
  // DEDICATED PLAY BUTTON
  // ---------------------------------------------------

  if (playPressed) {
    if (isRecording) {
      // Finish recording before entering file selection.
      stopRecording();
      enterPlayFileSelection();
    } else if (playbackActive) {
      // Second PLAY press stops active playback.
      stopPlaybackAndReturnLive();
    } else if (currentScreen == PLAY_FILE_SELECT) {
      // Start the chosen file.
      startPlayback();
    } else {
      // From Live Mode or another screen, open selection.
      enterPlayFileSelection();
    }
  }

  // If a button action changed the screen, continue
  // processing navigation for the new screen below.

  // ---------------------------------------------------
  // MAIN MENU
  // ---------------------------------------------------

  if (currentScreen == MAIN_MENU) {
    if (upPressed) {
      if (mainMenuIndex > 0) {
        mainMenuIndex--;
      } else {
        mainMenuIndex = MAIN_MENU_COUNT - 1;
      }
      renderCurrentScreen();
    }

    if (downPressed) {
      mainMenuIndex = (mainMenuIndex + 1) % MAIN_MENU_COUNT;
      renderCurrentScreen();
    }

    if (selectPressed) {
      openSelectedMainMenuItem();
    }

    if (backPressed) {
      enterLiveMode();
    }

    return;
  }

  // ---------------------------------------------------
  // LIVE MODE
  // ---------------------------------------------------

  if (currentScreen == LIVE_MODE) {
    if (selectPressed) {
      enterMainMenu();
    }

    if (backPressed) {
      if (playbackActive) {
        stopPlaybackAndReturnLive();
      } else {
        enterMainMenu();
      }
    }

    return;
  }

  // ---------------------------------------------------
  // RECORDING SCREEN
  // ---------------------------------------------------

  if (currentScreen == RECORDING_SCREEN) {
    if (backPressed) {
      stopRecording();
    }

    return;
  }

  // ---------------------------------------------------
  // PLAY FILE SELECTION
  // ---------------------------------------------------

  if (currentScreen == PLAY_FILE_SELECT) {
    if (upPressed) {
      selectedFile--;

      if (selectedFile < 1) {
        selectedFile = 99;
      }

      renderCurrentScreen();
    }

    if (downPressed) {
      selectedFile++;

      if (selectedFile > 99) {
        selectedFile = 1;
      }

      renderCurrentScreen();
    }

    if (selectPressed) {
      // Return to Live Mode without starting playback.
      enterLiveMode();
    }

    if (backPressed) {
      enterLiveMode();
    }

    return;
  }

  // ---------------------------------------------------
  // RECORDED FILES BROWSER
  // This browses file numbers; it does not fetch an
  // actual directory listing from the receiver's SD card.
  // ---------------------------------------------------

  if (currentScreen == RECORDED_FILES) {
    if (upPressed) {
      selectedFile--;

      if (selectedFile < 1) {
        selectedFile = 99;
      }

      renderCurrentScreen();
    }

    if (downPressed) {
      selectedFile++;

      if (selectedFile > 99) {
        selectedFile = 1;
      }

      renderCurrentScreen();
    }

    if (selectPressed) {
      enterPlayFileSelection();
    }

    if (backPressed) {
      enterMainMenu();
    }

    return;
  }

  // ---------------------------------------------------
  // HOME POSITION
  // ---------------------------------------------------

  if (currentScreen == HOME_SCREEN) {
    if (selectPressed) {
      if (isRecording) {
        stopRecording();
      }

      playbackActive = false;
      requestCommand(CMD_HOME);
      enterLiveMode();
    }

    if (backPressed) {
      enterMainMenu();
    }

    return;
  }

  // ---------------------------------------------------
  // JOYSTICK GAMES MENU
  // ---------------------------------------------------

  if (currentScreen == GAMES_MENU) {
    if (upPressed) {
      gamesMenuIndex = (gamesMenuIndex == 0)
                      ? GAMES_MENU_COUNT - 1
                      : gamesMenuIndex - 1;

      renderCurrentScreen();
    }

    if (downPressed) {
      gamesMenuIndex =
        (gamesMenuIndex + 1) % GAMES_MENU_COUNT;

      renderCurrentScreen();
    }

    if (selectPressed) {
      if (gamesMenuIndex == 0) {
        currentScreen = JOYSTICK_TESTER;
      } else {
        currentScreen = ARM_CONTROLLER;
      }

      renderCurrentScreen();
    }

    if (backPressed) {
      enterMainMenu();
    }

    return;
  }

  // ---------------------------------------------------
  // JOYSTICK X/Y TESTER
  // ---------------------------------------------------

  if (currentScreen == JOYSTICK_TESTER) {
    if (backPressed || selectPressed) {
      currentScreen = GAMES_MENU;
      renderCurrentScreen();
    }

    return;
  }

  // ---------------------------------------------------
  // ARM CONTROLLER
  // ---------------------------------------------------

  if (currentScreen == ARM_CONTROLLER) {
    if (backPressed) {
      currentScreen = GAMES_MENU;
      renderCurrentScreen();
    }

    if (selectPressed) {
      enterMainMenu();
    }

    return;
  }

  // ---------------------------------------------------
  // WIRELESS STATUS
  // ---------------------------------------------------

  if (currentScreen == WIRELESS_SCREEN) {
    if (backPressed || selectPressed) {
      enterMainMenu();
    }

    return;
  }
}

// =====================================================
// 17. SETUP
// =====================================================

void setup() {
  Serial.begin(115200);
  delay(300);

  // Joystick ADC
  analogReadResolution(12);

  pinMode(J1_SW, INPUT_PULLUP);
  pinMode(J2_SW, INPUT_PULLUP);

  // Buttons
  initializeButton(buttonUp, BTN_UP);
  initializeButton(buttonDown, BTN_DOWN);
  initializeButton(buttonSelect, BTN_SELECT);
  initializeButton(buttonBack, BTN_BACK);
  initializeButton(buttonRecord, BTN_RECORD);
  initializeButton(buttonPlay, BTN_PLAY);

  // I2C / OLED
  Wire.begin(OLED_SDA, OLED_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("OLED initialization failed!");
    while (true) {
      delay(1000);
    }
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.display();

  // nRF24 SPI pins
  SPI.begin(
    NRF_SCK,
    NRF_MISO,
    NRF_MOSI,
    NRF_CSN
  );

  radioReady = radio.begin();

  if (radioReady) {
    radio.setAddressWidth(5);
    radio.setChannel(RADIO_CHANNEL);
    radio.setDataRate(RF24_250KBPS);
    radio.setPALevel(RF24_PA_MAX);
    radio.setAutoAck(true);
    radio.setRetries(5, 15);
    radio.setPayloadSize(sizeof(ControlData));

    radio.openWritingPipe(radioAddress);
    radio.stopListening();

    Serial.println("nRF24 transmitter ready.");
  } else {
    Serial.println("nRF24 initialization failed!");
  }

  Serial.println("Wireless robotic arm transmitter");
  Serial.println("Startup screen ready.");

  currentScreen = SPLASH_SCREEN;
  showSplashScreen();
}

// =====================================================
// 18. MAIN LOOP
// =====================================================

unsigned long lastScreenUpdate = 0;

void loop() {
  readJoysticks();

  handleButtons();

  // Send live directional commands only in the operating
  // screens. During playback, the receiver is expected to
  // ignore ordinary control packets.
  //
  // Sending packets while browsing menus also maintains
  // a rolling view of radio acknowledgement quality.
  sendRadioPacket();

  unsigned long now = millis();

  if (now - lastScreenUpdate >= SCREEN_INTERVAL) {
    lastScreenUpdate = now;
    renderCurrentScreen();
  }

  delay(1);
}

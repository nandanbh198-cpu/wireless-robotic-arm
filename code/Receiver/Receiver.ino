
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <nRF24L01.h>
#include <RF24.h>
#include <Adafruit_PWMServoDriver.h>

// ======================================================
// 1. PCA9685 - KEEP WORKING CONNECTIONS
// ======================================================

#define PCA_SDA 8
#define PCA_SCL 9

Adafruit_PWMServoDriver pwm(0x40);

// ======================================================
// 2. NRF24 - KEEP WORKING CONNECTIONS
// ======================================================

#define NRF_CE   15
#define NRF_CSN  16
#define NRF_SCK  17
#define NRF_MOSI 18
#define NRF_MISO 21

RF24 radio(NRF_CE, NRF_CSN);
const byte radioAddress[6] = "ARM01";

// ======================================================
// 3. SD CARD - SEPARATE SPI BUS
// Previously tested HSPI configuration
// ======================================================

#define SD_CS   14
#define SD_SCK  12
#define SD_MOSI 11
#define SD_MISO 13

SPIClass SD_SPI(HSPI);
bool sdReady = false;

// ======================================================
// 4. SERVO CHANNELS
// ======================================================

#define SERVO_BASE      0
#define SERVO_SHOULDER  1
#define SERVO_ELBOW     2
#define SERVO_GRIPPER   3

#define SERVO_MIN 500
#define SERVO_MAX 2400

float baseAngle = 90;
float shoulderAngle = 90;
float elbowAngle = 90;
float gripperAngle = 50;

#define BASE_MIN       60
#define BASE_MAX       120

#define SHOULDER_MIN   60
#define SHOULDER_MAX   120

#define ELBOW_MIN      60
#define ELBOW_MAX      120

#define GRIPPER_MIN    20
#define GRIPPER_MAX    80

// ======================================================
// 5. COMMANDS - MUST MATCH TRANSMITTER
// ======================================================

enum CommandType {
  CMD_CONTROL = 0,
  CMD_HOME    = 1,
  CMD_RECORD  = 2,
  CMD_PLAY    = 3,
  CMD_STOP    = 4
};

struct ControlData {
  int8_t baseDirection;
  int8_t shoulderDirection;
  int8_t elbowDirection;
  int8_t gripperDirection;
  uint8_t command;
  uint8_t fileNumber;
};

ControlData packet = {
  0, 0, 0, 0, CMD_CONTROL, 1
};

// ======================================================
// 6. TIMING
// ======================================================

#define CONNECTION_TIMEOUT       500
#define SERVO_UPDATE_INTERVAL     20
#define PLAYBACK_INTERVAL         20

unsigned long lastPacketTime = 0;
unsigned long lastServoUpdate = 0;
unsigned long lastPlaybackStep = 0;

bool recording = false;
bool playbackActive = false;

File motionFile;

char motionFilename[24];

unsigned long recordedSamples = 0;

// ======================================================
// 7. SERVO FUNCTIONS
// ======================================================

int angleToPulse(float angle) {
  angle = constrain(angle, 0, 180);

  return map(
    (int)angle,
    0,
    180,
    SERVO_MIN,
    SERVO_MAX
  );
}

void writeServo(uint8_t channel, float angle) {
  pwm.writeMicroseconds(
    channel,
    angleToPulse(angle)
  );
}

void updateServos() {
  writeServo(SERVO_BASE, baseAngle);
  writeServo(SERVO_SHOULDER, shoulderAngle);
  writeServo(SERVO_ELBOW, elbowAngle);
  writeServo(SERVO_GRIPPER, gripperAngle);
}

void moveHome() {
  baseAngle = 90;
  shoulderAngle = 90;
  elbowAngle = 90;
  gripperAngle = 50;

  updateServos();

  Serial.println("HOME: B90 S90 E90 G50");
}

// ======================================================
// 8. JOYSTICK CONTROL
// ======================================================

void applyControl() {
  if (packet.baseDirection > 0) {
    baseAngle += 1;
  } else if (packet.baseDirection < 0) {
    baseAngle -= 1;
  }

  if (packet.shoulderDirection > 0) {
    shoulderAngle += 1;
  } else if (packet.shoulderDirection < 0) {
    shoulderAngle -= 1;
  }

  if (packet.elbowDirection > 0) {
    elbowAngle += 1;
  } else if (packet.elbowDirection < 0) {
    elbowAngle -= 1;
  }

  if (packet.gripperDirection > 0) {
    gripperAngle += 1;
  } else if (packet.gripperDirection < 0) {
    gripperAngle -= 1;
  }

  baseAngle = constrain(
    baseAngle, BASE_MIN, BASE_MAX
  );

  shoulderAngle = constrain(
    shoulderAngle, SHOULDER_MIN, SHOULDER_MAX
  );

  elbowAngle = constrain(
    elbowAngle, ELBOW_MIN, ELBOW_MAX
  );

  gripperAngle = constrain(
    gripperAngle, GRIPPER_MIN, GRIPPER_MAX
  );
}

// ======================================================
// 9. MOTION FILE NAME
// /REC_01.csv ... /REC_99.csv
// ======================================================

void makeFilename(uint8_t number) {
  number = constrain(number, 1, 99);

  snprintf(
    motionFilename,
    sizeof(motionFilename),
    "/REC_%02u.csv",
    number
  );
}

// ======================================================
// 10. STOP RECORDING
// ======================================================

void stopRecording() {
  if (!recording) {
    return;
  }

  if (motionFile) {
    motionFile.flush();
    motionFile.close();
  }

  recording = false;

  Serial.print("RECORDING SAVED: ");
  Serial.println(motionFilename);

  Serial.print("Samples: ");
  Serial.println(recordedSamples);
}

// ======================================================
// 11. START RECORDING
// A new recording replaces the same-numbered file.
// ======================================================

void startRecording(uint8_t fileNumber) {
  if (!sdReady) {
    Serial.println("ERROR: SD card is not ready!");
    return;
  }

  if (playbackActive) {
    stopPlayback();
  }

  if (recording) {
    Serial.println("Recording already active!");
    return;
  }

  makeFilename(fileNumber);

  Serial.print("Starting recording: ");
  Serial.println(motionFilename);

  // Remove the previous file so old samples cannot
  // remain appended to the new recording.
  if (SD.exists(motionFilename)) {
    if (!SD.remove(motionFilename)) {
      Serial.println("ERROR: Cannot replace old file!");
      return;
    }
  }

  motionFile = SD.open(motionFilename, FILE_WRITE);

  if (!motionFile) {
    Serial.println("ERROR: Cannot create motion file!");
    return;
  }

  recordedSamples = 0;

  motionFile.println("base,shoulder,elbow,gripper");
  motionFile.flush();

  recording = true;

  Serial.println("RECORDING STARTED");
}

// ======================================================
// 12. RECORD ONE SERVO POSE
// ======================================================

void recordCurrentPose() {
  if (!recording || !motionFile) {
    return;
  }

  motionFile.print((int)baseAngle);
  motionFile.print(',');

  motionFile.print((int)shoulderAngle);
  motionFile.print(',');

  motionFile.print((int)elbowAngle);
  motionFile.print(',');

  motionFile.println((int)gripperAngle);

  recordedSamples++;

  // Flush periodically to reduce data loss.
  if (recordedSamples % 25 == 0) {
    motionFile.flush();
  }
}

// ======================================================
// 13. STOP PLAYBACK
// ======================================================

void stopPlayback() {
  if (motionFile) {
    motionFile.close();
  }

  if (playbackActive) {
    Serial.println("PLAYBACK STOPPED");
  }

  playbackActive = false;

  packet.command = CMD_CONTROL;

  packet.baseDirection = 0;
  packet.shoulderDirection = 0;
  packet.elbowDirection = 0;
  packet.gripperDirection = 0;
}

// ======================================================
// 14. START PLAYBACK
// ======================================================

void startPlayback(uint8_t number) {
  if (!sdReady) {
    Serial.println("Cannot play: SD card unavailable");
    return;
  }

  stopRecording();
  stopPlayback();

  makeFilename(number);

  if (!SD.exists(motionFilename)) {
    Serial.print("FILE NOT FOUND: ");
    Serial.println(motionFilename);
    return;
  }

  motionFile = SD.open(motionFilename, FILE_READ);

  if (!motionFile) {
    Serial.print("ERROR OPENING: ");
    Serial.println(motionFilename);
    return;
  }

  playbackActive = true;
  lastPlaybackStep = millis();

  Serial.print("PLAYBACK STARTED: ");
  Serial.println(motionFilename);

  Serial.println("Continuous playback enabled");
}

// ======================================================
// 15. READ AND PLAY ONE RECORDED POSE
//
// At EOF, seek to the start and play again.
// Header and invalid lines are skipped.
// ======================================================

void processPlayback() {
  if (!playbackActive || !motionFile) {
    return;
  }

  if (millis() - lastPlaybackStep < PLAYBACK_INTERVAL) {
    return;
  }

  lastPlaybackStep = millis();

  int b, s, e, g;
  bool poseFound = false;

  // Two passes allow the reader to wrap to the beginning
  // once when it reaches the end of the file.
  for (uint8_t pass = 0; pass < 2 && !poseFound; pass++) {

    while (motionFile.available()) {
      String line = motionFile.readStringUntil('\n');
      line.trim();

      if (line.length() == 0) {
        continue;
      }

      // The CSV header is skipped automatically because
      // it does not contain four integer values.
      if (sscanf(
            line.c_str(),
            "%d,%d,%d,%d",
            &b, &s, &e, &g
          ) != 4) {
        continue;
      }

      poseFound = true;
      break;
    }

    if (!poseFound && pass == 0) {
      // End of file: rewind and repeat.
      if (!motionFile.seek(0)) {
        Serial.println("ERROR: Could not rewind file!");
        stopPlayback();
        return;
      }

      Serial.println("Playback loop: restarting file");
    }
  }

  if (!poseFound) {
    // The file may be empty or contain no valid pose rows.
    Serial.println("ERROR: No valid poses in motion file");
    stopPlayback();
    return;
  }

  // Apply the same safe limits as manual control.
  baseAngle = constrain(b, BASE_MIN, BASE_MAX);

  shoulderAngle = constrain(
    s, SHOULDER_MIN, SHOULDER_MAX
  );

  elbowAngle = constrain(
    e, ELBOW_MIN, ELBOW_MAX
  );

  gripperAngle = constrain(
    g, GRIPPER_MIN, GRIPPER_MAX
  );

  updateServos();
}

// ======================================================
// 16. SETUP
// ======================================================

void setup() {
  Serial.begin(115200);
  delay(1500);

  Serial.println();
  Serial.println("=== ROBOTIC ARM RECEIVER ===");

  // ----------------------------------------------------
  // PCA9685
  // ----------------------------------------------------

  Wire.begin(PCA_SDA, PCA_SCL);

  Wire.beginTransmission(0x40);

  if (Wire.endTransmission() != 0) {
    Serial.println("PCA9685 NOT FOUND!");
    while (true) {
      delay(1000);
    }
  }

  pwm.begin();
  pwm.setPWMFreq(50);

  delay(100);

  moveHome();

  Serial.println("PCA9685 FOUND!");

  // ----------------------------------------------------
  // SD CARD - HSPI
  // ----------------------------------------------------

  Serial.println("Checking SD card...");

  SD_SPI.begin(
    SD_SCK,
    SD_MISO,
    SD_MOSI,
    SD_CS
  );

  sdReady = SD.begin(
    SD_CS,
    SD_SPI,
    4000000
  );

  if (sdReady) {
    Serial.println("SD CARD READY");
  } else {
    Serial.println(
      "SD CARD ERROR - wireless control can still work"
    );
  }

  // ----------------------------------------------------
  // NRF24
  // ----------------------------------------------------

  SPI.begin(
    NRF_SCK,
    NRF_MISO,
    NRF_MOSI,
    NRF_CSN
  );

  Serial.println("Checking NRF24...");

  if (!radio.begin()) {
    Serial.println("NRF NOT FOUND!");

    while (true) {
      delay(1000);
    }
  }

  radio.setAddressWidth(5);
  radio.setChannel(108);
  radio.setDataRate(RF24_250KBPS);
  radio.setPALevel(RF24_PA_MAX);
  radio.setAutoAck(true);
  radio.setRetries(5, 15);

  radio.setPayloadSize(sizeof(ControlData));

  radio.openReadingPipe(1, radioAddress);
  radio.startListening();

  Serial.println("NRF FOUND!");
  Serial.println("RECEIVER READY");
}

// ======================================================
// 17. MAIN LOOP
// ======================================================

void loop() {
  // ----------------------------------------------------
  // Receive radio packets
  // ----------------------------------------------------

  while (radio.available()) {
    ControlData incoming;

    radio.read(&incoming, sizeof(incoming));

    lastPacketTime = millis();

    // While playback is active, ignore joystick commands.
    // STOP and HOME remain available.
    if (playbackActive) {

      if (incoming.command == CMD_STOP) {
        stopPlayback();
      }
      else if (incoming.command == CMD_HOME) {
        stopPlayback();
        moveHome();
      }

      continue;
    }

    packet = incoming;

    if (packet.command == CMD_HOME) {
      stopRecording();
      moveHome();
    }
    else if (packet.command == CMD_STOP) {
      stopRecording();

      packet.baseDirection = 0;
      packet.shoulderDirection = 0;
      packet.elbowDirection = 0;
      packet.gripperDirection = 0;

      Serial.println("STOP COMMAND");
    }
    else if (packet.command == CMD_RECORD) {
      if (!recording) {
        startRecording(packet.fileNumber);
      }
    }
    else if (packet.command == CMD_PLAY) {
      stopRecording();
      startPlayback(packet.fileNumber);
    }
    else if (packet.command == CMD_CONTROL) {
      stopRecording();
    }
  }

  // ----------------------------------------------------
  // Radio connection timeout
  // ----------------------------------------------------

  if (lastPacketTime != 0 &&
      millis() - lastPacketTime > CONNECTION_TIMEOUT) {

    packet.baseDirection = 0;
    packet.shoulderDirection = 0;
    packet.elbowDirection = 0;
    packet.gripperDirection = 0;

    if (recording) {
      stopRecording();
      packet.command = CMD_CONTROL;
    }

    // Safety: stop playback if radio communication is lost.
    if (playbackActive) {
      stopPlayback();
      Serial.println("Playback stopped: radio timeout");
    }
  }

  // ----------------------------------------------------
  // Playback has priority over manual control
  // ----------------------------------------------------

  if (playbackActive) {
    processPlayback();
    return;
  }

  // ----------------------------------------------------
  // Manual movement and recording
  // ----------------------------------------------------

  if (millis() - lastServoUpdate >= SERVO_UPDATE_INTERVAL) {
    lastServoUpdate = millis();

    if (packet.command == CMD_CONTROL ||
        packet.command == CMD_RECORD) {

      applyControl();
      updateServos();

      if (recording &&
          packet.command == CMD_RECORD) {
        recordCurrentPose();
      }
    }
  }
}

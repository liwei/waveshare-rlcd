// Chromium dino game for the Waveshare ESP32-S3-RLCD-4.2.
//
//   KEY  (GPIO18) : jump  - hold longer to jump higher, tap for a short hop.
//                   Also starts the game and restarts after a crash.
//   BOOT (GPIO0)  : pause / resume.
//
// The simulation advances on a fixed 60 Hz tick so the ported Chromium
// constants behave exactly as they do in the browser; drawing is decoupled
// and simply renders whatever the current state is.
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "battery.h"
#include "config.h"
#include "game.h"
#include "rlcd.h"

static Rlcd lcd(RLCD_SCK_PIN, RLCD_MOSI_PIN, RLCD_DC_PIN, RLCD_CS_PIN,
                RLCD_RST_PIN, RLCD_SPI_HZ);

// ---------------------------------------------------------------------------
// Buttons: sampled once per 60 Hz tick, accepted after two agreeing samples.
// ---------------------------------------------------------------------------
#define BTN_AGREE_FRAMES 2

struct Button {
  uint8_t pin;
  bool stable;    // debounced level, true = pressed
  bool raw;       // last raw reading
  uint8_t agree;
  bool downEvent;
  bool upEvent;
};

static Button keyBtn = {BTN_KEY_PIN, false, false, 0, false, false};
static Button bootBtn = {BTN_BOOT_PIN, false, false, 0, false, false};

enum DiagnosticType : uint8_t { DIAG_STATUS, DIAG_FRAMEBUFFER };

struct DiagnosticRequest {
  DiagnosticType type;
  uint32_t frameBytes;
  uint8_t *frame;
  char text[160];
};

static QueueHandle_t diagnosticQueue;
static uint32_t s_diagFps;

static void diagnosticTask(void *) {
  DiagnosticRequest request;
  for (;;) {
    if (xQueueReceive(diagnosticQueue, &request, portMAX_DELAY) != pdTRUE) continue;
    if (request.type == DIAG_STATUS) {
      Serial.println(request.text);
    } else {
      const uint32_t size = request.frameBytes;
      const uint8_t header[8] = {'F', 'B', 'U', 'F',
                                 (uint8_t)(size & 0xFF), (uint8_t)((size >> 8) & 0xFF),
                                 (uint8_t)((size >> 16) & 0xFF), (uint8_t)((size >> 24) & 0xFF)};
      Serial.write(header, sizeof(header));
      Serial.write(request.frame, size);
      free(request.frame);
    }
  }
}

static void queueStatus(bool heartbeat) {
  DiagnosticRequest request = {};
  request.type = DIAG_STATUS;
  if (heartbeat) {
    snprintf(request.text, sizeof(request.text),
             "fps=%lu %s key=%d/%d boot=%d/%d", (unsigned long)s_diagFps,
             Game_StatusLine(), digitalRead(BTN_KEY_PIN), keyBtn.stable ? 0 : 1,
             digitalRead(BTN_BOOT_PIN), bootBtn.stable ? 0 : 1);
  } else {
    snprintf(request.text, sizeof(request.text), "%s key=%d/%d boot=%d/%d",
             Game_StatusLine(), digitalRead(BTN_KEY_PIN), keyBtn.stable ? 0 : 1,
             digitalRead(BTN_BOOT_PIN), bootBtn.stable ? 0 : 1);
  }
  xQueueSend(diagnosticQueue, &request, 0);
}

static void queueFramebuffer() {
  const uint32_t size = (uint32_t)lcd.bufferSize();
  uint8_t *copy = (uint8_t *)malloc(size);
  if (!copy) return;
  memcpy(copy, lcd.buffer(), size);
  DiagnosticRequest request = {};
  request.type = DIAG_FRAMEBUFFER;
  request.frameBytes = size;
  request.frame = copy;
  if (xQueueSend(diagnosticQueue, &request, 0) != pdTRUE) free(copy);
}

static void buttonInit(Button &b) {
  pinMode(b.pin, INPUT_PULLUP);
  b.raw = (digitalRead(b.pin) == LOW);
  b.stable = b.raw;
  b.agree = 0;
}

static void buttonPoll(Button &b) {
  b.downEvent = false;
  b.upEvent = false;

  const bool raw = (digitalRead(b.pin) == LOW);
  if (raw == b.raw) {
    if (b.agree < BTN_AGREE_FRAMES) b.agree++;
  } else {
    b.raw = raw;
    b.agree = 0;
  }

  if (b.agree >= BTN_AGREE_FRAMES && b.stable != b.raw) {
    b.stable = b.raw;
    if (b.stable) {
      b.downEvent = true;
    } else {
      b.upEvent = true;
    }
  }
}

// ---------------------------------------------------------------------------
static uint32_t s_lastUs;
static uint32_t s_accUs;

void setup() {
  Serial.begin(115200);
  buttonInit(keyBtn);
  buttonInit(bootBtn);

  if (!lcd.begin(U8G2_R1)) {
    Serial.println("RLCD framebuffer allocation failed");
    while (true) delay(1000);
  }
  Battery_Init();
  Game_Init();

  diagnosticQueue = xQueueCreate(4, sizeof(DiagnosticRequest));
  configASSERT(diagnosticQueue != nullptr);
  BaseType_t taskCreated = xTaskCreatePinnedToCore(diagnosticTask, "diagnostics", 4096,
                                                    nullptr, 1, nullptr, 0);
  configASSERT(taskCreated == pdPASS);

  Game_Draw(lcd);
  lcd.flush();

  s_lastUs = micros();
  Serial.println("dino_rlcd ready");
}

void loop() {
  const uint32_t now = micros();
  uint32_t dt = now - s_lastUs;
  s_lastUs = now;
  if (dt > 100000) dt = 100000;  // never replay more than 100 ms of catch-up
  s_accUs += dt;

  // Diagnostic console commands are bounded per pass; all output is queued for
  // the background task so USB serial backpressure cannot block game updates.
  bool dirty = false;
  uint8_t commandsRead = 0;
  while (Serial.available() && commandsRead++ < 4) {
    const int c = Serial.read();
    if (c == 'k') {
      Game_KeyDown();
      dirty = true;
    } else if (c == 'u') {
      Game_KeyUp();
      dirty = true;
    } else if (c == 'b') {
      Game_KeyTogglePause();
      dirty = true;
    } else if (c == 'd') {
      queueFramebuffer();
    } else if (c == 's') {
      queueStatus(false);
    }
  }

  int steps = 0;
  if (Battery_Update()) dirty = true;

  while (s_accUs >= TICK_US && steps < 4) {
    buttonPoll(keyBtn);
    buttonPoll(bootBtn);
    if (keyBtn.downEvent) {
      Game_KeyDown();
      dirty = true;
    }
    if (keyBtn.upEvent) {
      Game_KeyUp();
      dirty = true;
    }
    if (bootBtn.downEvent) {
      Game_KeyTogglePause();
      dirty = true;
    }

    Game_Tick();
    s_accUs -= TICK_US;
    steps++;
  }

  if (steps > 0 || dirty) {
    Game_Draw(lcd);
    lcd.flush();
  }

  static uint32_t lastStats = 0;
  static uint32_t frames = 0;
  static uint32_t lastFrames = 0;
  if (steps > 0) frames++;
  const uint32_t nowMs = millis();
  if (nowMs - lastStats >= 2000) {
    s_diagFps = (frames - lastFrames) / 2;
    queueStatus(true);
    lastStats = nowMs;
    lastFrames = frames;
  }
}

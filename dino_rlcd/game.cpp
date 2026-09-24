// Chromium dino game logic, ported to the ESP32-S3-RLCD-4.2.
//
// Every gameplay value and rule below is a direct port of Chromium's
// components/neterror/resources/dino_game/ (offline.ts, trex.ts, obstacle.ts,
// horizon.ts, distance_meter.ts, cloud.ts, generated_sound_fx.ts,
// offline_sprite_definitions.ts). Source constants are named in comments.
//
// The simulation runs in the original 600x150 canvas coordinate space at a
// fixed 60 Hz tick, so all the per-frame constants transfer unchanged. The
// only concession to our panel is the spawn edge (LCD_W instead of 600) and
// the ground line being shifted down via RENDER_DY.
#include "game.h"

#include <math.h>
#include <string.h>

#include <Preferences.h>

#include "audio.h"
#include "battery.h"
#include "config.h"
#include "sprites.h"

// ---------------------------------------------------------------------------
// Verbatim constants
// ---------------------------------------------------------------------------
static const float ACCELERATION = 0.001f;    // Runner.config.ACCELERATION
static const float SPEED_START = 6.0f;       // Runner.config.SPEED
static const float MAX_SPEED = 13.0f;        // Runner.config.MAX_SPEED
static const float GRAVITY = 0.6f;           // Trex.config.GRAVITY
static const float INITIAL_JUMP_V = -10.0f;  // Trex.config.INIITAL_JUMP_VELOCITY
static const float DROP_VELOCITY = -5.0f;    // Trex.config.DROP_VELOCITY
static const int MAX_JUMP_HEIGHT = 30;       // Trex.config.MAX_JUMP_HEIGHT
static const int MIN_JUMP_HEIGHT = 30;       // Trex.config.MIN_JUMP_HEIGHT
static const float SPEED_DROP_COEF = 3.0f;   // Trex.config.SPEED_DROP_COEFFICIENT
static const float GAP_COEFFICIENT = 0.6f;   // Runner.config.GAP_COEFFICIENT
static const float MAX_GAP_COEFFICIENT = 1.5f;  // Obstacle.maxGapCoefficient
static const int MAX_OBSTACLE_LENGTH = 3;       // Runner.config.MAX_OBSTACLE_LENGTH
static const int MAX_OBSTACLE_DUPLICATION = 2;  // Runner.config.MAX_OBSTACLE_DUPLICATION
static const float COEFFICIENT = 0.025f;        // DistanceMeter.config.COEFFICIENT
static const int MAX_DISTANCE_UNITS = 5;        // DistanceMeter.config.MAX_DISTANCE_UNITS
static const int ACHIEVEMENT_DISTANCE = 100;    // DistanceMeter.config.ACHIEVEMENT_DISTANCE
static const float FLASH_DURATION = 1000.0f / 4.0f;
static const int FLASH_ITERATIONS = 3;
static const int INVERT_DISTANCE = 700;         // Runner.config.INVERT_DISTANCE
static const float INVERT_FADE_DURATION = 12000.0f;  // Runner.config.INVERT_FADE_DURATION
static const int MAX_CLOUDS = 6;                // Runner.config.MAX_CLOUDS
static const float BG_CLOUD_SPEED = 0.2f;       // Runner.config.BG_CLOUD_SPEED
static const int CLOUD_MIN_GAP = 100;           // Cloud.config.MIN_CLOUD_GAP
static const int CLOUD_MAX_GAP = 400;           // Cloud.config.MAX_CLOUD_GAP
static const int CLOUD_MAX_SKY = 30;            // Cloud.config.MAX_SKY_LEVEL
static const int CLOUD_MIN_SKY = 71;            // Cloud.config.MIN_SKY_LEVEL
static const int GAMEOVER_CLEAR_TIME = 750;     // Runner.config.GAMEOVER_CLEAR_TIME
static const float START_X_POS = 50.0f;         // Trex.config.START_X_POS
static const int TREX_H = 47;                   // Trex.config.HEIGHT
static const int BOTTOM_PAD = 10;               // Runner.config.BOTTOM_PAD
static const int ORIG_H = 150;                  // Runner.defaultDimensions.HEIGHT

// Original ground: dino top when grounded, and its feet.
static const int GROUND_TOP = ORIG_H - TREX_H - BOTTOM_PAD;  // 93
static const int GROUND_FEET = GROUND_TOP + TREX_H;          // 140
static const int RENDER_DY = 250 - GROUND_FEET;              // -> feet at y=250
static const int GROUND_LINE_Y = 250;

// The sky above the ground line is 250px here but only 140px in the original
// canvas, so the cloud band is scaled to keep the same visual placement.
static const int SKY_TOP_SCREEN = (CLOUD_MAX_SKY * GROUND_LINE_Y) / GROUND_FEET;
static const int SKY_BOTTOM_SCREEN = (CLOUD_MIN_SKY * GROUND_LINE_Y) / GROUND_FEET;

// ---------------------------------------------------------------------------
// Sprite / obstacle definitions (offline_sprite_definitions.ts, ldpi)
// ---------------------------------------------------------------------------
struct CollisionBox {
  int8_t x, y, w, h;
};

struct ObstacleType {
  const Bitmap *sprites;   // 1 entry when clustering, 2 for the animated ptero
  bool animated;
  uint8_t width, height;
  const int16_t *yPos;     // one or three allowed heights
  uint8_t yPosCount;
  uint16_t multipleSpeed;
  uint16_t minGap;
  float minSpeed;
  const CollisionBox *boxes;
  uint8_t boxCount;
  float speedOffset;
};

static const CollisionBox BOXES_SMALL[] = {{0, 7, 5, 27}, {4, 0, 6, 34}, {10, 4, 7, 14}};
static const CollisionBox BOXES_LARGE[] = {{0, 12, 7, 38}, {8, 0, 7, 49}, {13, 10, 10, 38}};
static const CollisionBox BOXES_PTERO[] = {{15, 15, 16, 5}, {18, 21, 24, 6}, {2, 14, 4, 3},
                                           {6, 10, 4, 7}, {10, 8, 6, 9}};
// Trex.collisionBoxes.RUNNING
static const CollisionBox BOXES_TREX[] = {{22, 0, 17, 16}, {1, 18, 30, 9}, {10, 35, 14, 8},
                                          {1, 24, 29, 5}, {5, 30, 21, 4}, {9, 34, 15, 4}};

static const int16_t YPOS_SMALL[] = {105};
static const int16_t YPOS_LARGE[] = {90};
// yPosMobile: our board has no duck button, so we use the same two flight
// heights Chromium uses on touch devices that cannot duck either.
static const int16_t YPOS_PTERO[] = {100, 50};

static const ObstacleType OBSTACLE_TYPES[3] = {
    {CACTUS_SMALL, false, 17, 35, YPOS_SMALL, 1, 4, 120, 0.0f, BOXES_SMALL, 3, 0.0f},
    {CACTUS_LARGE, false, 25, 50, YPOS_LARGE, 1, 7, 120, 0.0f, BOXES_LARGE, 3, 0.0f},
    {PTERO, true, 46, 40, YPOS_PTERO, 2, 999, 150, 8.5f, BOXES_PTERO, 5, 0.8f},
};
#define PTERO_TYPE 2

static const uint8_t MAX_OBSTACLES = 8;

struct Obstacle {
  bool active;
  uint8_t type;
  uint8_t size;
  int16_t x;
  int16_t y;
  int16_t width;
  int16_t gap;
  bool followingCreated;
  float speedOffset;
  uint8_t frame;
  float frameTimer;
  uint32_t seq;      // spawn order; speedOffset means x alone cannot tell
  CollisionBox boxes[6];
  uint8_t boxCount;
};

static uint32_t s_spawnSeq = 0;

// ---------------------------------------------------------------------------
// Dino animation frames (Trex.animFrames: WAITING [44,0] / RUNNING [88,132] /
// JUMPING [0] / CRASHED [220])
// ---------------------------------------------------------------------------
enum DinoStatus { D_WAITING, D_RUNNING, D_JUMPING, D_CRASHED };
#define DINO_ANIM_LEN 4
static const uint8_t DINO_ANIM_COUNT[DINO_ANIM_LEN] = {2, 2, 1, 1};
static const float DINO_MS_PER_FRAME[DINO_ANIM_LEN] = {1000.0f / 3.0f, 1000.0f / 12.0f,
                                                       1000.0f / 60.0f, 1000.0f / 60.0f};

static const Bitmap *dinoFrame(uint8_t status, uint8_t idx) {
  switch (status) {
    case D_RUNNING:
      return &DINO_RUN[idx & 1];
    case D_CRASHED:
      return &DINO_CRASH;
    default:
      return &DINO_STAND;   // WAITING frame 44 is pixel-identical to frame 0
  }
}

// ---------------------------------------------------------------------------
// Game state
// ---------------------------------------------------------------------------
enum State { ST_IDLE, ST_PLAYING, ST_PAUSED, ST_OVER };

struct Cloud {
  int16_t x;
  int16_t y;
  int16_t gap;
  bool active;
};

static State s_state;
static float s_speed;
static float s_distanceRan;
static float s_y;               // dino top, in original canvas coords
static float s_jumpVelocity;
static bool s_jumping;
static bool s_reachedMinHeight;
static bool s_speedDrop;
static uint8_t s_dinoStatus;
static uint8_t s_dinoFrame;
static float s_dinoTimer;
static float s_groundScroll;
static float s_msSinceOver;

static Obstacle s_obstacles[MAX_OBSTACLES];
static Cloud s_clouds[MAX_CLOUDS];

static uint16_t s_highScore;    // persisted in NVS
static uint16_t s_score;

// night mode (Runner.prototype.update "Night mode." block)
static bool s_inverted;
static bool s_invertTrigger;
static float s_invertTimer;

// achievement flash (DistanceMeter.update)
static bool s_achievement;
static float s_flashTimer;
static uint8_t s_flashIterations;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline uint32_t rnd(uint32_t n) { return n ? (esp_random() % n) : 0; }

static inline int getRandomNum(int min, int max) {
  return min + (int)rnd((uint32_t)(max - min + 1));
}

static inline int origToScreenY(int origY) { return origY + RENDER_DY; }

// u8g2_DrawXBMP cannot take a negative x, so blit pixels ourselves: this also
// lets sprites scroll off the left edge naturally.
static void blit(u8g2_t *g, const Bitmap &b, int x, int y) {
  if (y + b.h <= 0 || y >= LCD_H) return;
  const int bpr = (b.w + 7) / 8;
  int sx0 = 0, sx1 = b.w;
  int dx = x;
  if (dx < 0) {
    sx0 = -dx;
    dx = 0;
  }
  if (dx + (sx1 - sx0) > LCD_W) sx1 = sx0 + (LCD_W - dx);
  if (sx1 <= sx0) return;

  for (int row = 0; row < b.h; row++) {
    const int py = y + row;
    if (py < 0 || py >= LCD_H) continue;
    const uint8_t *line = b.bits + (size_t)row * bpr;
    int i = sx0;
    while (i < sx1) {
      while (i < sx1 && !((line[i >> 3] >> (7 - (i & 7))) & 1)) i++;
      if (i >= sx1) break;
      int j = i;
      while (j < sx1 && ((line[j >> 3] >> (7 - (j & 7))) & 1)) j++;
      if (j > i) u8g2_DrawHLine(g, dx + (i - sx0), py, j - i);
      i = j;
    }
  }
}

static uint16_t NVS_HighScoreLoad() {
  Preferences prefs;
  prefs.begin("dino", true);
  uint16_t v = prefs.getUShort("hi", 0);
  prefs.end();
  return v;
}

static void NVS_HighScoreSave(uint16_t v) {
  Preferences prefs;
  prefs.begin("dino", false);
  prefs.putUShort("hi", v);
  prefs.end();
}

// ---------------------------------------------------------------------------
// Obstacles (obstacle.ts + Runner.addNewObstacle/updateObstacles)
// ---------------------------------------------------------------------------
static int obstacleSpriteIndex(const Obstacle &o) {
  const ObstacleType &t = OBSTACLE_TYPES[o.type];
  return t.animated ? o.frame : (o.size - 1);
}

static void obstacleWidth(Obstacle &o) {
  const ObstacleType &t = OBSTACLE_TYPES[o.type];
  o.width = (int16_t)(t.width * o.size);
}

static void spawnObstacle() {
  // Runner.addNewObstacle: re-roll while the type is gated by speed or has
  // already been duplicated MAX_OBSTACLE_DUPLICATION times.
  uint8_t type;
  for (int guard = 0; guard < 32; guard++) {
    type = (uint8_t)getRandomNum(0, 2);
    const ObstacleType &t = OBSTACLE_TYPES[type];
    if (s_speed < t.minSpeed) continue;
    int dup = 0;
    for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
      if (s_obstacles[i].active && s_obstacles[i].type == type) dup++;
    }
    if (dup >= MAX_OBSTACLE_DUPLICATION) continue;
    break;
  }

  for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
    if (s_obstacles[i].active) continue;
    Obstacle &o = s_obstacles[i];
    const ObstacleType &t = OBSTACLE_TYPES[type];
    memset(&o, 0, sizeof(o));
    o.active = true;
    o.type = type;
    o.seq = ++s_spawnSeq;
    o.boxCount = t.boxCount;
    memcpy(o.boxes, t.boxes, sizeof(CollisionBox) * t.boxCount);

    // size = getRandomNum(1, maxObstacleLength); clamped while slow.
    o.size = (uint8_t)getRandomNum(1, MAX_OBSTACLE_LENGTH);
    if (o.size > 1 && t.multipleSpeed > s_speed) o.size = 1;
    obstacleWidth(o);

    o.y = t.yPos[t.yPosCount == 1 ? 0 : getRandomNum(0, t.yPosCount - 1)];
    o.x = LCD_W;

    // Central box stretches between the outer two when clustered.
    if (o.size > 1) {
      o.boxes[1].w = (int8_t)(o.width - o.boxes[0].w - o.boxes[2].w);
      o.boxes[2].x = (int8_t)(o.width - o.boxes[2].w);
    }

    if (t.speedOffset != 0.0f) {
      o.speedOffset = (rnd(2) == 0) ? t.speedOffset : -t.speedOffset;
    }

    // getGap(): minGap widens with speed.
    const int minG = (int)lroundf(o.width * s_speed + t.minGap * GAP_COEFFICIENT);
    const int maxG = (int)lroundf(minG * MAX_GAP_COEFFICIENT);
    o.gap = (int16_t)getRandomNum(minG, maxG);
    return;
  }
}

static void updateObstacles() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
    Obstacle &o = s_obstacles[i];
    if (!o.active) continue;

    // Obstacle.update(): xPos -= floor((speed * FPS / 1000) * deltaTime)
    float sp = s_speed + o.speedOffset;
    o.x -= (int16_t)floorf(sp * 60.0f / 1000.0f * MS_PER_FRAME);

    if (OBSTACLE_TYPES[o.type].animated) {
      o.frameTimer += MS_PER_FRAME;
      if (o.frameTimer >= 1000.0f / 6.0f) {   // frameRate: 1000/6
        o.frame ^= 1;
        o.frameTimer = 0;
      }
    }
    if (!(o.x + o.width > 0)) o.active = false;   // isVisible()
    else count++;
  }

  // Runner.updateObstacles() looks at the most recently added obstacle.
  Obstacle *last = nullptr;
  for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
    if (s_obstacles[i].active && (!last || s_obstacles[i].seq > last->seq)) {
      last = &s_obstacles[i];
    }
  }
  if (!last) {
    spawnObstacle();
  } else if (!last->followingCreated && (last->x + last->width + last->gap) < LCD_W) {
    spawnObstacle();
    last->followingCreated = true;
  }
}

static bool hitDetected() {
  for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
    const Obstacle &o = s_obstacles[i];
    if (!o.active) continue;
    for (uint8_t a = 0; a < sizeof(BOXES_TREX) / sizeof(BOXES_TREX[0]); a++) {
      const int ax = (int)START_X_POS + BOXES_TREX[a].x;
      const int ay = (int)s_y + BOXES_TREX[a].y;
      for (uint8_t b = 0; b < o.boxCount; b++) {
        const int bx = o.x + o.boxes[b].x;
        const int by = o.y + o.boxes[b].y;
        if (ax < bx + o.boxes[b].w && ax + BOXES_TREX[a].w > bx &&
            ay < by + o.boxes[b].h && ay + BOXES_TREX[a].h > by) {
          return true;
        }
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Clouds (cloud.ts)
// ---------------------------------------------------------------------------
static void updateClouds() {
  uint8_t count = 0;
  for (uint8_t i = 0; i < MAX_CLOUDS; i++) {
    Cloud &c = s_clouds[i];
    if (!c.active) continue;
    // Cloud.update(): xPos -= ceil(elSpeed), elSpeed = cloudSpeed/1000*dt*speed
    c.x -= (int16_t)ceilf(BG_CLOUD_SPEED / 1000.0f * MS_PER_FRAME * s_speed);
    if (!(c.x + CLOUD_W > 0)) c.active = false;
    else count++;
  }
  if (count >= MAX_CLOUDS) return;

  // Runner.updateClouds() measures the gap from the most recently added cloud;
  // they all drift at the same rate, so that is simply the right-most one.
  const Cloud *newest = nullptr;
  for (uint8_t i = 0; i < MAX_CLOUDS; i++) {
    if (s_clouds[i].active && (!newest || s_clouds[i].x > newest->x)) newest = &s_clouds[i];
  }
  if (!newest || (LCD_W - newest->x) > newest->gap) {
    for (uint8_t i = 0; i < MAX_CLOUDS; i++) {
      if (s_clouds[i].active) continue;
      s_clouds[i].active = true;
      s_clouds[i].x = LCD_W;
      s_clouds[i].y = (int16_t)getRandomNum(SKY_TOP_SCREEN, SKY_BOTTOM_SCREEN);
      s_clouds[i].gap = (int16_t)getRandomNum(CLOUD_MIN_GAP, CLOUD_MAX_GAP);
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Dino (trex.ts)
// ---------------------------------------------------------------------------
static void dinoSetStatus(uint8_t status) {
  if (s_dinoStatus == status) return;
  s_dinoStatus = status;
  s_dinoFrame = 0;
  s_dinoTimer = 0;
}

static void dinoAnimate() {
  s_dinoTimer += MS_PER_FRAME;
  if (s_dinoTimer >= DINO_MS_PER_FRAME[s_dinoStatus]) {
    const uint8_t n = DINO_ANIM_COUNT[s_dinoStatus];
    s_dinoFrame = (uint8_t)((s_dinoFrame + 1) % n);
    s_dinoTimer = 0;
  }
}

static void dinoReset() {
  s_y = (float)GROUND_TOP;
  s_jumpVelocity = 0.0f;
  s_jumping = false;
  s_speedDrop = false;
  s_reachedMinHeight = false;
  dinoSetStatus(D_WAITING);
}

static void dinoStartJump() {
  if (s_jumping) return;
  dinoSetStatus(D_JUMPING);
  s_jumpVelocity = INITIAL_JUMP_V - (s_speed / 10.0f);   // startJump(speed)
  s_jumping = true;
  s_reachedMinHeight = false;
  s_speedDrop = false;
  Audio_Play(FX_JUMP);
}

static void dinoUpdateJump() {
  const float minJumpY = (float)(GROUND_TOP - MIN_JUMP_HEIGHT);   // 63

  if (s_speedDrop) {
    s_y += roundf(s_jumpVelocity * SPEED_DROP_COEF);
  } else {
    s_y += roundf(s_jumpVelocity);
  }
  s_jumpVelocity += GRAVITY;

  if (s_y < minJumpY || s_speedDrop) s_reachedMinHeight = true;

  if (s_y < (float)MAX_JUMP_HEIGHT || s_speedDrop) {
    // endJump()
    if (s_reachedMinHeight && s_jumpVelocity < DROP_VELOCITY) {
      s_jumpVelocity = DROP_VELOCITY;
    }
  }

  if (s_y > (float)GROUND_TOP) {   // back on the ground
    s_y = (float)GROUND_TOP;
    s_jumping = false;
    s_speedDrop = false;
    s_jumpVelocity = 0.0f;
    dinoSetStatus(D_RUNNING);
  }
}

static void gameOver() {
  s_state = ST_OVER;
  s_msSinceOver = 0.0f;
  dinoSetStatus(D_CRASHED);
  Audio_Play(FX_HIT);
  if (s_score > s_highScore) {
    s_highScore = s_score;
    NVS_HighScoreSave(s_highScore);
  }
}

// ---------------------------------------------------------------------------
// Simulation tick (Runner.prototype.update / Runner.updateJump)
// ---------------------------------------------------------------------------
void Game_Tick() {
  if (s_state == ST_IDLE) {
    dinoSetStatus(D_WAITING);
    dinoAnimate();
    return;
  }
  if (s_state == ST_PAUSED) return;
  if (s_state == ST_OVER) {
    s_msSinceOver += MS_PER_FRAME;
    return;
  }

  if (s_speed < MAX_SPEED) s_speed += ACCELERATION;

  if (s_jumping) {
    dinoUpdateJump();
  } else {
    dinoSetStatus(D_RUNNING);
  }

  // Runner.updateDistanceRan() + DistanceMeter.getActualDistance()
  s_distanceRan += s_speed;
  s_score = (uint16_t)floorf(s_distanceRan * COEFFICIENT);

  updateObstacles();
  updateClouds();
  s_groundScroll += s_speed;
  dinoAnimate();

  if (hitDetected()) {
    gameOver();
    return;
  }

  // ---- achievement flash + cue ----
  if (!s_achievement) {
    if (s_score > 0 && s_score % ACHIEVEMENT_DISTANCE == 0) {
      s_achievement = true;
      s_flashTimer = 0.0f;
      s_flashIterations = 0;
      Audio_Play(FX_SCORE);
    }
  } else {
    if (s_flashIterations <= FLASH_ITERATIONS) {
      s_flashTimer += MS_PER_FRAME;
      if (s_flashTimer > FLASH_DURATION * 2.0f) {
        s_flashTimer = 0.0f;
        s_flashIterations++;
      }
    } else {
      s_achievement = false;
      s_flashIterations = 0;
      s_flashTimer = 0.0f;
    }
  }

  // ---- night mode (Runner.update "Night mode.") ----
  if (s_invertTimer > INVERT_FADE_DURATION) {
    s_invertTimer = 0.0f;
    s_invertTrigger = false;
    s_inverted = !s_inverted;
  } else if (s_invertTimer > 0.0f) {
    s_invertTimer += MS_PER_FRAME;
  } else if (s_score > 0) {
    s_invertTrigger = (s_score % INVERT_DISTANCE) == 0;
    if (s_invertTrigger) {
      s_invertTimer += MS_PER_FRAME;
      s_inverted = !s_inverted;
    }
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
static void startRun() {
  s_speed = SPEED_START;
  s_distanceRan = 0.0f;
  s_score = 0;
  s_achievement = false;
  s_flashIterations = 0;
  s_flashTimer = 0.0f;
  s_invertTimer = 0.0f;
  s_invertTrigger = false;
  s_inverted = false;
  s_groundScroll = 0.0f;
  for (uint8_t i = 0; i < MAX_OBSTACLES; i++) s_obstacles[i].active = false;
  for (uint8_t i = 0; i < MAX_CLOUDS; i++) s_clouds[i].active = false;
  dinoReset();
  s_state = ST_PLAYING;
}

void Game_KeyDown() {
  switch (s_state) {
    case ST_IDLE:
      startRun();
      break;
    case ST_PLAYING:
      dinoStartJump();
      break;
    case ST_OVER:
      // Runner.restart() is gated by GAMEOVER_CLEAR_TIME.
      if (s_msSinceOver >= GAMEOVER_CLEAR_TIME) startRun();
      break;
    default:
      break;
  }
}

void Game_KeyUp() {
  // setSpeedDrop(): releasing early cuts the jump short.
  if (s_state == ST_PLAYING && s_jumping) s_speedDrop = true;
}

void Game_KeyTogglePause() {
  switch (s_state) {
    case ST_IDLE:
      startRun();
      break;
    case ST_PLAYING:
      s_state = ST_PAUSED;
      break;
    case ST_PAUSED:
      s_state = ST_PLAYING;
      break;
    case ST_OVER:
      if (s_msSinceOver >= GAMEOVER_CLEAR_TIME) startRun();
      break;
  }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void drawNumber(u8g2_t *g, uint32_t value, int x, int y, int digits) {
  // DistanceMeter.update() zero-pads the score to maxScoreUnits.
  static const uint32_t POW10[] = {1, 10, 100, 1000, 10000, 100000};
  if (digits > 0 && digits <= 5) value %= POW10[digits];
  char buf[8];
  snprintf(buf, sizeof(buf), "%0*u", digits, (unsigned)value);
  for (int i = 0; i < digits; i++) {
    blit(g, DIGITS[buf[i] - '0'], x + i * DIGIT_W, y);
  }
}

static void drawBattery(u8g2_t *g) {
  const BatteryStatus status = Battery_GetStatus();
  const int x = 8;
  const int y = 8;
  const int bodyW = 24;
  const int bodyH = 12;
  const int levelW = ((bodyW - 4) * status.percent + 50) / 100;

  u8g2_DrawFrame(g, x, y, bodyW, bodyH);
  u8g2_DrawBox(g, x + bodyW, y + 4, 3, 4);
  if (levelW > 0) u8g2_DrawBox(g, x + 2, y + 2, levelW, bodyH - 4);

  if (status.charging) {
    const uint8_t background = s_inverted ? 1 : 0;
    const uint8_t foreground = s_inverted ? 0 : 1;
    u8g2_SetDrawColor(g, background);
    u8g2_DrawLine(g, x + 14, y + 2, x + 10, y + 6);
    u8g2_DrawLine(g, x + 10, y + 6, x + 13, y + 6);
    u8g2_DrawLine(g, x + 13, y + 6, x + 10, y + 10);
    u8g2_SetDrawColor(g, foreground);
  }

  char label[6];
  snprintf(label, sizeof(label), "%u%%", status.percent);
  u8g2_SetFont(g, u8g2_font_6x13_tf);
  u8g2_DrawStr(g, x + bodyW + 8, y + 11, label);
}

static void drawScore(u8g2_t *g) {
  drawBattery(g);
  const int y = 10;
  // DistanceMeter.calcXPos(): this.x = canvasWidth - DEST_WIDTH*(units+1)
  const int scoreX = LCD_W - DIGIT_W * (MAX_DISTANCE_UNITS + 1);

  if (s_highScore > 0) {
    // DistanceMeter.drawHighScore(): glyphs 'H','I',' ' then the digits,
    // laid out to the left of the running score.
    const int hiX = scoreX - (MAX_DISTANCE_UNITS * 2) * DIGIT_W;
    blit(g, HI_H, hiX, y);
    blit(g, HI_I, hiX + DIGIT_W, y);
    drawNumber(g, s_highScore, hiX + 3 * DIGIT_W, y, MAX_DISTANCE_UNITS);
  }

  // The score flashes on every ACHIEVEMENT_DISTANCE points.
  const bool hidden = s_achievement && s_flashTimer < FLASH_DURATION;
  if (!hidden) drawNumber(g, s_score, scoreX, y, MAX_DISTANCE_UNITS);
}

void Game_Draw(Rlcd &lcd) {
  u8g2_t *g = lcd.gfx();
  const bool night = s_inverted;

  // The panel is 1 bpp, so "night mode" is a full inversion: fill the buffer
  // and draw sprites in the opposite colour.
  memset(lcd.buffer(), night ? 0xFF : 0x00, lcd.bufferSize());
  u8g2_SetDrawColor(g, night ? 0 : 1);

  // ---- clouds (already in screen coordinates) ----
  for (uint8_t i = 0; i < MAX_CLOUDS; i++) {
    if (s_clouds[i].active) blit(g, CLOUD, s_clouds[i].x, s_clouds[i].y);
  }

  // ---- horizon (two copies cover the 400px window out of a 600px tile) ----
  const int lineTop = GROUND_LINE_Y - HORIZON_LINE_ROW;
  const int off = (int)fmodf(s_groundScroll, (float)HORIZON_W);
  blit(g, HORIZON, -off, lineTop);
  blit(g, HORIZON, HORIZON_W - off, lineTop);

  // ---- obstacles ----
  for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
    const Obstacle &o = s_obstacles[i];
    if (!o.active) continue;
    blit(g, OBSTACLE_TYPES[o.type].sprites[obstacleSpriteIndex(o)], o.x,
         origToScreenY(o.y));
  }

  // ---- dino ----
  const Bitmap *d = dinoFrame(s_dinoStatus, s_dinoFrame);
  blit(g, *d, (int)START_X_POS, origToScreenY((int)s_y));

  // ---- score ----
  drawScore(g);

  // ---- overlays ----
  u8g2_SetFont(g, u8g2_font_6x13_tf);
  const int cx = LCD_W / 2;

  if (s_state == ST_IDLE) {
    const char *l1 = "PRESS KEY TO START";
    const char *l2 = "KEY = JUMP    BOOT = PAUSE";
    u8g2_DrawStr(g, cx - (int)u8g2_GetStrWidth(g, l1) / 2, 120, l1);
    u8g2_DrawStr(g, cx - (int)u8g2_GetStrWidth(g, l2) / 2, 142, l2);
  } else if (s_state == ST_PAUSED) {
    const char *l1 = "PAUSED";
    const char *l2 = "BOOT TO RESUME";
    u8g2_DrawBox(g, cx - 100, 108, 200, 46);
    u8g2_SetDrawColor(g, night ? 1 : 0);
    u8g2_DrawStr(g, cx - (int)u8g2_GetStrWidth(g, l1) / 2, 128, l1);
    u8g2_DrawStr(g, cx - (int)u8g2_GetStrWidth(g, l2) / 2, 148, l2);
    u8g2_SetDrawColor(g, night ? 0 : 1);
  } else if (s_state == ST_OVER) {
    const int gx = (LCD_W - GAME_OVER_W) / 2;
    for (int i = 0; i < 8; i++) blit(g, GAME_OVER[i], gx + GAME_OVER_X[i], 96);
    if (s_msSinceOver >= GAMEOVER_CLEAR_TIME) {
      blit(g, RESTART, cx - RESTART_W / 2, 128);
      const char *hint = "KEY TO RESTART";
      u8g2_DrawStr(g, cx - (int)u8g2_GetStrWidth(g, hint) / 2, 186, hint);
    }
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
const char *Game_StatusLine() {
  static const char *names[] = {"idle", "run", "pause", "over"};
  static char buf[128];
  uint8_t obs = 0;
  int near = -1;
  uint8_t nearType = 0;
  for (uint8_t i = 0; i < MAX_OBSTACLES; i++) {
    if (!s_obstacles[i].active) continue;
    obs++;
    const int right = s_obstacles[i].x + s_obstacles[i].width;
    if (right > (int)START_X_POS && (near < 0 || s_obstacles[i].x < near)) {
      near = s_obstacles[i].x;
      nearType = s_obstacles[i].type;
    }
  }
  const BatteryStatus battery = Battery_GetStatus();
  snprintf(buf, sizeof(buf),
           "state=%s score=%u hi=%u speed=%.1f obs=%u y=%.0f night=%d near=%d nt=%u audio=%s bat=%u%% chg=%d",
           names[s_state], s_score, s_highScore, s_speed, obs, s_y, s_inverted ? 1 : 0,
           near, nearType, Audio_Ready() ? "ok" : "off", battery.percent,
           battery.charging ? 1 : 0);
  return buf;
}

void Game_Init() {
  Audio_Init();
  s_highScore = NVS_HighScoreLoad();
  s_state = ST_IDLE;
  s_speed = SPEED_START;
  s_distanceRan = 0.0f;
  s_score = 0;
  s_groundScroll = 0.0f;
  s_inverted = false;
  s_invertTimer = 0.0f;
  s_achievement = false;
  memset(s_obstacles, 0, sizeof(s_obstacles));
  memset(s_clouds, 0, sizeof(s_clouds));
  dinoReset();
}

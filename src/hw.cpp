#include "app.h"

// ---------- RGB LED (active low) ----------
void ledSet(uint8_t r, uint8_t g, uint8_t b) {
  digitalWrite(PIN_LED_R, r ? LOW : HIGH);
  digitalWrite(PIN_LED_G, g ? LOW : HIGH);
  digitalWrite(PIN_LED_B, b ? LOW : HIGH);
}

void ledColorName(const String& c0) {
  String c = c0; c.toLowerCase();
  if (c == "red" || c == "vermelho") ledSet(1, 0, 0);
  else if (c == "green" || c == "verde") ledSet(0, 1, 0);
  else if (c == "blue" || c == "azul") ledSet(0, 0, 1);
  else if (c == "yellow" || c == "amarelo" || c == "orange" || c == "laranja") ledSet(1, 1, 0);
  else if (c == "purple" || c == "roxo" || c == "magenta") ledSet(1, 0, 1);
  else if (c == "cyan" || c == "ciano") ledSet(0, 1, 1);
  else if (c == "white" || c == "branco") ledSet(1, 1, 1);
  else ledSet(0, 0, 0);
}

// ---------- speaker (GPIO26 -> on-board amp) ----------
void beep(int freq, int ms) {
  ledcSetup(6, freq, 8);
  ledcAttachPin(PIN_SPEAKER, 6);
  ledcWrite(6, 96);
  delay(ms);
  ledcWrite(6, 0);
  ledcDetachPin(PIN_SPEAKER);
}

// ---------- light sensor ----------
int lightPct() {
  // The CYD LDR reads lower values in brighter light.
  int v = analogRead(PIN_LDR);
  int pct = 100 - (v * 100 / 4095);
  return constrain(pct, 0, 100);
}

// ---------- XPT2046 resistive touch, bit-banged on its own pins ----------
static uint16_t tpXfer(uint8_t cmd) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    digitalWrite(TP_CLK, HIGH); delayMicroseconds(1);
    digitalWrite(TP_CLK, LOW); delayMicroseconds(1);
  }
  // MOSI must be low while clocking the result out, or the XPT2046 sees a new start bit.
  digitalWrite(TP_MOSI, LOW);
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(TP_CLK, HIGH); delayMicroseconds(1);
    v = (v << 1) | digitalRead(TP_MISO);
    digitalWrite(TP_CLK, LOW); delayMicroseconds(1);
  }
  return (v >> 3) & 0x0FFF;
}

static bool tpInit = false;
static void touchSetup() {
  pinMode(TP_CLK, OUTPUT); pinMode(TP_MOSI, OUTPUT); pinMode(TP_CS, OUTPUT);
  pinMode(TP_MISO, INPUT); pinMode(TP_IRQ, INPUT);
  digitalWrite(TP_CS, HIGH); digitalWrite(TP_CLK, LOW);
  tpInit = true;
}

bool touchRaw(int& rx, int& ry) {
  if (!tpInit) touchSetup();
  if (digitalRead(TP_IRQ) == HIGH) return false;  // PENIRQ is low only while pressed
  digitalWrite(TP_CS, LOW);
  int z1 = tpXfer(0xB1), z2 = tpXfer(0xC1);
  int z = z1 + 4095 - z2;
  if (z < 300) { tpXfer(0x80); digitalWrite(TP_CS, HIGH); return false; }  // 300: accepts a light press
  long sx = 0, sy = 0;
  tpXfer(0xD1);  // dummy conversion to settle
  for (int i = 0; i < 4; i++) { sx += tpXfer(0xD1); sy += tpXfer(0x91); }
  tpXfer(0x80);  // power down
  digitalWrite(TP_CS, HIGH);
  rx = sx / 4; ry = sy / 4;
  return rx > 50 && rx < 4050 && ry > 50 && ry < 4050;
}

// Diagnostic: raw 16-bit words, sampled both on rising and on falling clock edge.
static uint16_t tpWord(uint8_t cmd, bool sampleAfterFall) {
  for (int i = 7; i >= 0; i--) {
    digitalWrite(TP_MOSI, (cmd >> i) & 1);
    digitalWrite(TP_CLK, HIGH); delayMicroseconds(2);
    digitalWrite(TP_CLK, LOW); delayMicroseconds(2);
  }
  digitalWrite(TP_MOSI, LOW);
  uint16_t v = 0;
  for (int i = 0; i < 16; i++) {
    digitalWrite(TP_CLK, HIGH); delayMicroseconds(2);
    if (!sampleAfterFall) v = (v << 1) | digitalRead(TP_MISO);
    digitalWrite(TP_CLK, LOW); delayMicroseconds(2);
    if (sampleAfterFall) v = (v << 1) | digitalRead(TP_MISO);
  }
  return v;
}

void touchDump(Stream& o) {
  if (!tpInit) touchSetup();
  o.printf("TPDUMP irq=%d miso_idle=%d", digitalRead(TP_IRQ), digitalRead(TP_MISO));
  const uint8_t cmds[4] = {0xB0, 0xC0, 0xD0, 0x90};
  for (int mode = 0; mode < 2; mode++) {
    digitalWrite(TP_CS, LOW);
    o.printf(" m%d:", mode);
    for (int k = 0; k < 4; k++) o.printf(" %02X=%04X", cmds[k], tpWord(cmds[k], mode));
    tpWord(0x80, mode);
    digitalWrite(TP_CS, HIGH);
  }
  o.println();
}

bool touchRead(int& x, int& y) {
  int rx, ry;
  if (!touchRaw(rx, ry)) return false;
  float a = cfg.calSwap ? ry : rx;
  float b = cfg.calSwap ? rx : ry;
  if (cfg.calOk) {
    x = (int)(cfg.calAx * a + cfg.calBx);
    y = (int)(cfg.calAy * b + cfg.calBy);
  } else {
    // Uncalibrated guess for this board in landscape (rotation 1)
    x = map(rx, 200, 3900, 0, 319);
    y = map(ry, 240, 3800, 0, 239);
  }
  x = constrain(x, 0, 319); y = constrain(y, 0, 239);
  return true;
}

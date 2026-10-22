// Seeed XIAO SAMD21 - adjustable low-side linear load regulator (default 12 V, 2.5-18 V via Serial)
// Q1 = IRL40SC228, gate driven directly from the DAC (A0): 100k from A0 to GND,
// then 10k in series to the gate (full 3.3 V on gate). Load between +Vbat and Q1 drain.
// ADC uses the internal 1.0 V reference, dividers 100k / 4.7k (max 20 V in).
//
// Serial commands (115200 baud, end with Enter / newline):
//   V<volts>   set load voltage, e.g. "V11.5" or "V 13"   (range V_SET_MIN..V_SET_MAX)
//   V          show current setpoint
//   ON / OFF   enable / disable the output
//   P1 / P0    periodic status print on / off
//   ?          show status and settings
// The setpoint is not stored: after power-up it is always V_DEFAULT (12.00 V).

#include <Arduino.h>

// ---------------- Pins ----------------
const uint8_t PIN_GATE_DAC = A0;   // PA02, true 10-bit DAC, 100k to GND -> 10k -> gate
const uint8_t PIN_VDRAIN   = A1;   // PA04, 100k/4.7k divider from Q1 drain
const uint8_t PIN_VBAT     = A2;   // PA10, 100k/4.7k divider from +Vbat
const uint8_t PIN_NTC      = A3;   // PA11, optional NTC on Q1 heatsink

#define USE_NTC 1                  // set to 0 if no NTC is fitted

// ---------------- Scaling ----------------
const float ADC_REF    = 1.000f;               // internal 1.0 V reference
const float ADC_COUNTS = 4096.0f;              // 12-bit
const float DIV_RATIO  = (100.0f + 4.7f) / 4.7f;  // = 22.28

// Calibration: apply a known voltage (e.g. 12.00 V) to both divider inputs,
// read the values on Serial, then set CAL = real / measured.
float CAL_VBAT   = 1.000f;
float CAL_VDRAIN = 1.060f;

// ---------------- Setpoint ----------------
const float V_DEFAULT     = 12.00f;  // setpoint after power-up
const float V_SET_MIN     = 2.50f;   // lowest setpoint accepted over Serial
const float V_SET_MAX     = 18.00f;  // highest setpoint accepted over Serial
const float V_OVER_MARGIN = 1.00f;   // overvoltage pull-back above setpoint + this

// ---------------- Control ----------------
// Fixed control period. Must be longer than one loop pass (see dt_max on Serial).
// Set to about dt_max x 1.2. If you change it a lot, scale KI_UP, KI_DOWN and KP
// by the same ratio (new period / old period) to keep the same regulation speed.
const uint32_t LOOP_US     = 1300;
const float    KI_UP       = 2.0f;    // DAC counts per volt per loop (gate up)
const float    KI_DOWN     = 2.0f;    // DAC counts per volt per loop (gate down)
const float    KP          = 8.0f;    // proportional, DAC counts per volt
const float    INTEG_START = 250.0f;  // ~0.8 V on DAC, below gate threshold
const float    VD_SAT      = 0.15f;   // drain below this = Q1 fully on
const int      DAC_MAX     = 1023;
const uint8_t  OVERSAMPLE  = 8;       // ADC samples averaged per reading

// ---------------- Protection ----------------
const float T_MAX_C     = 90.0f;   // over-temperature shutdown
const float T_RESUME_C  = 75.0f;   // restart below this

// NTC: 47k from 3V3 to A3, 10k NTC (B=3950) from A3 to GND
const float NTC_R_SERIES = 47000.0f;
const float NTC_R25      = 10000.0f;
const float NTC_BETA     = 3950.0f;
const float VCC          = 3.30f;

// ---------------- State ----------------
float vTarget   = V_DEFAULT;
bool  outputOn  = true;
bool  printOn   = true;
float integ     = INTEG_START;
bool  overTemp  = false;
float lastVbat = 0, lastVd = 0, lastVload = 0, lastTemp = NAN;
int   lastDac  = 0;

// Loop timing diagnostics
uint32_t dtMax    = 0;   // longest loop pass in us
uint32_t overruns = 0;   // passes that took longer than LOOP_US

// Serial command buffer
char    cmdBuf[24];
uint8_t cmdLen = 0;

// ---------------- Measurement ----------------
float readPinVolts(uint8_t pin) {
  uint32_t sum = 0;
  for (uint8_t i = 0; i < OVERSAMPLE; i++) sum += analogRead(pin);
  return (sum / (float)OVERSAMPLE) * (ADC_REF / ADC_COUNTS);
}

float readInputVolts(uint8_t pin, float cal) {
  return readPinVolts(pin) * DIV_RATIO * cal;
}

float readTempC() {
  float v = readPinVolts(PIN_NTC);
  if (v >= 0.99f * ADC_REF) return -40.0f;     // clipped: cold, treat as safe
  if (v <= 0.001f) return 200.0f;              // shorted NTC: treat as hot
  float r = NTC_R_SERIES * v / (VCC - v);
  float invT = 1.0f / 298.15f + log(r / NTC_R25) / NTC_BETA;
  return 1.0f / invT - 273.15f;
}

void setGate(int value) {
  value = constrain(value, 0, DAC_MAX);
  analogWrite(PIN_GATE_DAC, value);
  lastDac = value;
}

// ---------------- Serial interface ----------------
void printStatus() {
  Serial.print("Vset=");      Serial.print(vTarget, 2);
  Serial.print(outputOn ? " ON" : " OFF");
  Serial.print(" Vbat=");     Serial.print(lastVbat, 2);
  Serial.print(" Vd=");       Serial.print(lastVd, 2);
  Serial.print(" Vload=");    Serial.print(lastVload, 2);
  Serial.print(" DAC=");      Serial.print(lastDac);
  Serial.print(" T=");        Serial.print(lastTemp, 1);
  Serial.print(" dt_max=");   Serial.print(dtMax);
  Serial.print(" overruns="); Serial.print(overruns);
  Serial.println(overTemp ? " OVERTEMP" : "");
}

void printHelp() {
  Serial.println("Commands: V<volts>, V, ON, OFF, P1, P0, ?");
  Serial.print("Setpoint range: ");
  Serial.print(V_SET_MIN, 2); Serial.print(" .. ");
  Serial.print(V_SET_MAX, 2); Serial.println(" V");
}

void handleCommand(char *s) {
  while (*s == ' ') s++;
  for (char *p = s; *p; p++) *p = toupper(*p);
  if (*s == 0) return;

  if (s[0] == 'V') {
    char *arg = s + 1;
    while (*arg == ' ') arg++;
    if (*arg == 0) {
      Serial.print("Vset="); Serial.println(vTarget, 2);
      return;
    }
    float v = atof(arg);
    if (v >= V_SET_MIN && v <= V_SET_MAX) {
      vTarget = v;
      Serial.print("OK Vset="); Serial.println(vTarget, 2);
    } else {
      Serial.print("ERR range ");
      Serial.print(V_SET_MIN, 2); Serial.print(" .. ");
      Serial.print(V_SET_MAX, 2); Serial.println(" V");
    }
  } else if (strcmp(s, "ON") == 0) {
    outputOn = true;
    Serial.println("OK ON");
  } else if (strcmp(s, "OFF") == 0) {
    outputOn = false;
    Serial.println("OK OFF");
  } else if (strcmp(s, "P1") == 0) {
    printOn = true;
    Serial.println("OK P1");
  } else if (strcmp(s, "P0") == 0) {
    printOn = false;
    Serial.println("OK P0");
  } else if (s[0] == '?') {
    printStatus();
    printHelp();
  } else {
    Serial.print("ERR unknown: "); Serial.println(s);
    printHelp();
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen) {
        cmdBuf[cmdLen] = 0;
        handleCommand(cmdBuf);
        cmdLen = 0;
      }
    } else if (cmdLen < sizeof(cmdBuf) - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  analogWriteResolution(10);
  setGate(0);                                   // Q1 off immediately

  analogReadResolution(12);
  analogReference(AR_INTERNAL1V0);              // ADC only, DAC stays on AVCC

  // Faster ADC: 48 MHz / 64 = 750 kHz, short sampling time
  ADC->CTRLB.bit.PRESCALER = ADC_CTRLB_PRESCALER_DIV64_Val;
  while (ADC->STATUS.bit.SYNCBUSY);
  ADC->SAMPCTRL.reg = 5;
  while (ADC->STATUS.bit.SYNCBUSY);

  Serial.begin(115200);
  delay(100);                                   // let supplies and filters settle
}

void loop() {
  static uint32_t lastLoop  = micros();
  static uint32_t lastPrint = 0;

  pollSerial();                                 // commands are handled between control passes

  uint32_t now = micros();
  if (now - lastLoop < LOOP_US) return;
  lastLoop = now;                               // fixed period, no catch-up bursts

  uint32_t t0 = micros();                       // start of timed section

  float vbat  = readInputVolts(PIN_VBAT, CAL_VBAT);
  float vd    = readInputVolts(PIN_VDRAIN, CAL_VDRAIN);
  float vload = vbat - vd;
  float err   = vTarget - vload;                // > 0: load voltage too low

#if USE_NTC
  lastTemp = readTempC();
  if (lastTemp > T_MAX_C)    overTemp = true;
  if (lastTemp < T_RESUME_C) overTemp = false;
#endif

  if (overTemp || !outputOn) {
    integ = INTEG_START;
    setGate(0);
  } else {
    if (vload > vTarget + V_OVER_MARGIN) {
      integ -= 20.0f;                           // fast pull-back on overvoltage
    } else if (err > 0) {
      if (vd > VD_SAT) integ += KI_UP * err;    // anti-windup: hold if Q1 fully on
    } else {
      integ += KI_DOWN * err;
    }
    integ = constrain(integ, 0.0f, (float)DAC_MAX);
    setGate((int)(integ + KP * err));
  }

  lastVbat = vbat; lastVd = vd; lastVload = vload;

  uint32_t dt = micros() - t0;                  // end of timed section
  if (dt > dtMax) dtMax = dt;
  if (dt > LOOP_US) overruns++;

  if (printOn && millis() - lastPrint >= 250) {
    lastPrint = millis();
    printStatus();
  }
}

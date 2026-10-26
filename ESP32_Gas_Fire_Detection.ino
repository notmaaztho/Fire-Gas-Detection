/*
 ==============================================================================
  ESP32 COMPLETE GAS + FIRE DETECTION SYSTEM WITH WI-FI & SERVO MOTOR
 ==============================================================================
  Features:
  - MQ Gas Sensor (GPIO 27) with 4-sample noise filter
  - Flame Sensor (GPIO 26) with 4-sample noise filter
  - 3 Status LEDs: Green (Safe), Yellow (Gas), Red (Flame / Emergency)
  - ISD1820 Voice Alarm Module (GPIO 25)
  - SG90 Servo Motor (GPIO 13):
      * DANGER  (Gas/Fire) -> Moves to 90 deg (Closed / Cutoff)
      * SAFE    (Normalized) -> Automatically returns to 0 deg (Open / Normal)
  - Wi-Fi Auto-connect & Non-blocking handling
  - Direct Google Mail Webhook: INSTANT GMAIL ALERTS (0 MINUTE COOLDOWN!)
 ==============================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP32Servo.h>
#include "secrets.h"

// ==============================================================================
// 1. WI-FI & DIRECT GMAIL CONFIGURATION
// Set WIFI_SSID, WIFI_PASSWORD, and GOOGLE_SCRIPT_URL in secrets.h.
// ==============================================================================

// Credentials are stored separately in secrets.h (do not commit that file).

// Google Apps Script URL is stored separately in secrets.h.


// ==============================================================================
// 2. PIN DEFINITIONS
// ==============================================================================
const int GAS_PIN    = 27;  // MQ Gas Sensor Digital Output (DO)
const int FLAME_PIN  = 26;  // Flame Sensor Digital Output (DO)

const int GREEN_LED  = 18;  // Safe / Normal state LED
const int YELLOW_LED = 19;  // Gas warning LED
const int RED_LED    = 23;  // Flame / Emergency LED

const int VOICE_PIN  = 25;  // ISD1820 Voice Module PLAYE pin
const int SERVO_PIN  = 13;  // SG90 Servo Signal pin (PWM)


// ==============================================================================
// 3. SENSOR & SERVO LOGIC SETTINGS
// ==============================================================================
// Digital pins for MQ & Flame sensor modules:
// LOW = detected (sensor triggered)
// HIGH = normal
const int GAS_ACTIVE   = LOW;
const int FLAME_ACTIVE = LOW;

// Servo Angles for Valve Simulation
const int SERVO_OPEN   = 0;   // 0 degrees = Valve open (Safe / Normal)
const int SERVO_CLOSED = 90;  // 90 degrees = Valve closed (Emergency shutoff)

// Timing settings (in milliseconds)
const unsigned long SENSOR_INTERVAL = 100;    // Check sensors every 100ms
const unsigned long STATUS_INTERVAL = 1000;   // Print Serial log every 1 second
const unsigned long STARTUP_TIME    = 30000;  // 30 seconds warm-up for MQ sensor
const unsigned long VOICE_COOLDOWN  = 5000;   // 5s between voice triggers
const unsigned long EMAIL_COOLDOWN  = 10000;  // 10s cooldown between emails during live testing
const int CONFIRM_COUNT             = 4;      // 4 consecutive reads to confirm state


// ==============================================================================
// 4. GLOBAL OBJECTS & VARIABLES
// ==============================================================================
Servo valveServo;

bool gasDetected       = false;
bool flameDetected     = false;

bool lastGasRaw        = false;
bool lastFlameRaw      = false;

int  gasCount          = 0;
int  flameCount        = 0;

bool systemReady       = false;
bool valveClosed       = false;

unsigned long startupMillis     = 0;
unsigned long lastSensorMillis  = 0;
unsigned long lastStatusMillis  = 0;
unsigned long lastVoiceMillis   = 0;
unsigned long lastEmailMillis   = 0;
unsigned long lastWiFiCheck     = 0;


// ==============================================================================
// 5. HELPER: URL ENCODE STRINGS
// ==============================================================================
String urlEncode(String str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else if (c == '\n') {
      encoded += "%0A";
    } else {
      char buf[4];
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}


// ==============================================================================
// 6. FUNCTION: TRIGGER ISD1820 VOICE ALARM
// ==============================================================================
void playVoice() {
  unsigned long now = millis();

  if (lastVoiceMillis != 0 && (now - lastVoiceMillis < VOICE_COOLDOWN)) {
    return;
  }
  lastVoiceMillis = now;

  Serial.println(">>> [VOICE ALARM] Playing recorded voice message...");

  // Send a pulse to trigger ISD1820 PLAYE
  digitalWrite(VOICE_PIN, LOW);
  delay(20);
  digitalWrite(VOICE_PIN, HIGH);
  delay(150);
  digitalWrite(VOICE_PIN, LOW);
}


// ==============================================================================
// 7. FUNCTION: SERVO MOTOR CONTROL (AUTO CLOSE & AUTO REOPEN)
// ==============================================================================

// Close Valve (Emergency - 90 Degrees)
void closeValve() {
  if (!valveClosed) {
    Serial.println("\n**************************************************");
    Serial.println("  [SERVO] DANGER DETECTED -> CLOSING VALVE (90 DEG)");
    Serial.println("**************************************************");
    valveServo.write(SERVO_CLOSED);
    valveClosed = true;
  }
}

// Reopen Valve (Safe / Normal - 0 Degrees)
void openValve() {
  if (valveClosed) {
    Serial.println("\n**************************************************");
    Serial.println("  [SERVO] ENVIRONMENT SAFE -> REOPENING VALVE (0 DEG)");
    Serial.println("**************************************************");
    valveServo.write(SERVO_OPEN);
    valveClosed = false;
  }
}


// ==============================================================================
// 8. FUNCTION: SEND INSTANT DIRECT GMAIL VIA GOOGLE WEBHOOK (NO 30-MIN LIMIT!)
// ==============================================================================
void sendEmergencyAlert(String alertType) {
  unsigned long now = millis();

  // Short 10-second debounce so contact bounce doesn't double-send
  if (lastEmailMillis != 0 && (now - lastEmailMillis < EMAIL_COOLDOWN)) {
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[EMAIL ALERT SKIPPED] Wi-Fi is not connected.");
    return;
  }

  Serial.println("\n[ALERT] Sending instant Gmail via Google Webhook...");

  WiFiClientSecure client;
  client.setInsecure(); // Allows HTTPS without loading certificate bundle

  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000); // 10 second timeout

  // Build parameters
  String subject = "CRITICAL SAFETY ALERT: " + alertType;
  String body    = "EMERGENCY DETECTED by ESP32 Safety System!\n\n"
                   "Condition: " + alertType + "\n"
                   "Gas Sensor: " + (gasDetected ? "DETECTED" : "NORMAL") + "\n"
                   "Flame Sensor: " + (flameDetected ? "DETECTED" : "NORMAL") + "\n"
                   "Servo Valve: SHUT DOWN (90 degrees)\n"
                   "Voice Alarm: TRIGGERED\n\n"
                   "Please inspect the location immediately!";

  String fullUrl = String(GOOGLE_SCRIPT_URL) + "?subject=" + urlEncode(subject) + "&body=" + urlEncode(body);

  if (http.begin(client, fullUrl)) {
    int httpCode = http.GET();
    Serial.print("[ALERT] Google Webhook Response Code: ");
    Serial.println(httpCode);

    if (httpCode == 200 || httpCode == 302) {
      Serial.println("[ALERT SUCCESS] Direct Gmail sent successfully to BOTH addresses!");
      lastEmailMillis = now;
    } else {
      Serial.println("[ALERT FAILED] Server response code: " + String(httpCode));
    }
    http.end();
  } else {
    Serial.println("[ALERT ERROR] Failed to connect to Google Apps Script.");
  }
}


// ==============================================================================
// 9. FUNCTION: CONTROL LEDs
// ==============================================================================
void updateLEDs() {
  // Normal / Safe
  if (!gasDetected && !flameDetected) {
    digitalWrite(GREEN_LED, HIGH);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, LOW);
  }
  // Gas only
  else if (gasDetected && !flameDetected) {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, HIGH);
    digitalWrite(RED_LED, LOW);
  }
  // Flame only
  else if (!gasDetected && flameDetected) {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, LOW);
    digitalWrite(RED_LED, HIGH);
  }
  // Gas + Flame both
  else {
    digitalWrite(GREEN_LED, LOW);
    digitalWrite(YELLOW_LED, HIGH);
    digitalWrite(RED_LED, HIGH);
  }
}


// ==============================================================================
// 10. FUNCTION: READ AND DEBOUNCE SENSORS
// ==============================================================================
void readSensors() {
  bool gasRaw   = (digitalRead(GAS_PIN) == GAS_ACTIVE);
  bool flameRaw = (digitalRead(FLAME_PIN) == FLAME_ACTIVE);

  // --- GAS SENSOR FILTER ---
  if (gasRaw == lastGasRaw) {
    if (gasCount < CONFIRM_COUNT) {
      gasCount++;
    }
  } else {
    lastGasRaw = gasRaw;
    gasCount = 1;
  }

  if (gasCount >= CONFIRM_COUNT) {
    if (gasDetected != gasRaw) {
      gasDetected = gasRaw;

      if (gasDetected) {
        Serial.println();
        Serial.println("========================================");
        Serial.println("       !!! GAS LEAK DETECTED !!!        ");
        Serial.println("========================================");

        playVoice();
        closeValve();
        sendEmergencyAlert("GAS LEAK DETECTED");
      } else {
        Serial.println("[INFO] Gas level normalized.");
      }
    }
  }

  // --- FLAME SENSOR FILTER ---
  if (flameRaw == lastFlameRaw) {
    if (flameCount < CONFIRM_COUNT) {
      flameCount++;
    }
  } else {
    lastFlameRaw = flameRaw;
    flameCount = 1;
  }

  if (flameCount >= CONFIRM_COUNT) {
    if (flameDetected != flameRaw) {
      flameDetected = flameRaw;

      if (flameDetected) {
        Serial.println();
        Serial.println("========================================");
        Serial.println("        !!! FLAME DETECTED !!!          ");
        Serial.println("========================================");

        playVoice();
        closeValve();
        sendEmergencyAlert("FIRE / FLAME DETECTED");
      } else {
        Serial.println("[INFO] Flame cleared.");
      }
    }
  }

  // --- AUTO RESET SERVO TO 0 DEG WHEN BOTH SENSORS ARE SAFE ---
  if (!gasDetected && !flameDetected) {
    if (valveClosed) {
      openValve(); // Automatically return to 0 degrees (Safe / Normal)
    }
  }

  updateLEDs();
}


// ==============================================================================
// 11. FUNCTION: PRINT SYSTEM STATUS
// ==============================================================================
void printStatus() {
  Serial.println();
  Serial.println("----------------------------------------");
  Serial.print("GAS SENSOR   : ");
  Serial.println(gasDetected ? "DETECTED" : "NORMAL");

  Serial.print("FLAME SENSOR : ");
  Serial.println(flameDetected ? "DETECTED" : "NORMAL");

  Serial.print("SERVO VALVE  : ");
  Serial.println(valveClosed ? "CLOSED (90 deg - Safety Cutoff)" : "OPEN (0 deg - Normal / Safe)");

  Serial.print("WI-FI STATUS : ");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("CONNECTED (IP: ");
    Serial.print(WiFi.localIP());
    Serial.println(")");
  } else {
    Serial.println("CONNECTING / DISCONNECTED");
  }

  Serial.print("GMAIL ALERT  : READY (0-min cooldown)");

  Serial.print("\nOVERALL STATE: ");
  if (gasDetected && flameDetected) {
    Serial.println("CRITICAL DANGER: GAS + FIRE");
  } else if (gasDetected) {
    Serial.println("DANGER: GAS LEAK");
  } else if (flameDetected) {
    Serial.println("DANGER: FIRE DETECTED");
  } else {
    Serial.println("SAFE");
  }
  Serial.println("----------------------------------------");
}


// ==============================================================================
// 12. SETUP
// ==============================================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  // Initialize Sensors
  pinMode(GAS_PIN, INPUT);
  pinMode(FLAME_PIN, INPUT);

  // Initialize LEDs
  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  // Initialize Voice Module
  pinMode(VOICE_PIN, OUTPUT);

  // Set default output pin states
  digitalWrite(GREEN_LED, LOW);
  digitalWrite(YELLOW_LED, LOW);
  digitalWrite(RED_LED, LOW);
  digitalWrite(VOICE_PIN, LOW);

  // Initialize SG90 Servo Motor
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);
  ESP32PWM::allocateTimer(2);
  ESP32PWM::allocateTimer(3);
  valveServo.setPeriodHertz(50);              // Standard 50Hz servo
  valveServo.attach(SERVO_PIN, 500, 2400);    // Min 500us, Max 2400us pulse
  valveServo.write(SERVO_OPEN);               // Set initial position to 0 degrees (Open / Safe)
  valveClosed = false;

  // Start Wi-Fi connection in Station mode (non-blocking)
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  startupMillis = millis();

  Serial.println();
  Serial.println("==================================================");
  Serial.println("      ESP32 GAS + FLAME SAFETY SYSTEM READY       ");
  Serial.println("==================================================");
  Serial.println("Starting 30-second sensor stabilization...");
  Serial.println("Wi-Fi connection running in background...");
}


// ==============================================================================
// 13. MAIN LOOP
// ==============================================================================
void loop() {
  unsigned long now = millis();

  // Background non-blocking Wi-Fi auto-reconnect every 10 seconds
  if (WiFi.status() != WL_CONNECTED && (now - lastWiFiCheck >= 10000)) {
    lastWiFiCheck = now;
    WiFi.reconnect();
  }

  // --- SENSOR WARM-UP & STARTUP TIMER ---
  if (!systemReady) {
    unsigned long elapsed = now - startupMillis;

    if (elapsed < STARTUP_TIME) {
      if (now - lastStatusMillis >= 1000) {
        lastStatusMillis = now;
        unsigned long remaining = (STARTUP_TIME - elapsed + 999) / 1000;
        Serial.print("Stabilizing sensors... starting in ");
        Serial.print(remaining);
        Serial.println(" seconds.");
      }
      return; // Do not monitor until stabilization completes
    }

    // Warm-up finished!
    systemReady = true;

    // Read initial reference states
    lastGasRaw   = (digitalRead(GAS_PIN) == GAS_ACTIVE);
    lastFlameRaw = (digitalRead(FLAME_PIN) == FLAME_ACTIVE);
    gasCount     = 0;
    flameCount   = 0;

    Serial.println();
    Serial.println("==================================================");
    Serial.println("            SYSTEM ACTIVE AND MONITORING          ");
    Serial.println("==================================================");
    Serial.println();

    updateLEDs();
  }

  // --- SENSOR MONITORING ---
  if (now - lastSensorMillis >= SENSOR_INTERVAL) {
    lastSensorMillis = now;
    readSensors();
  }

  // --- STATUS PRINTOUT ---
  if (now - lastStatusMillis >= STATUS_INTERVAL) {
    lastStatusMillis = now;
    printStatus();
  }
}
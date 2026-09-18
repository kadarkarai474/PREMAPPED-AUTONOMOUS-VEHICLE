#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include "soc/soc.h"             
#include "soc/rtc_cntl_reg.h"    

// ================= Wi-Fi & UDP Settings =================
const char* ssid = "Robot_Net";
const char* password = "robotpassword";
WiFiUDP udp;
const int localUdpPort = 4210;
char incomingPacket[255]; 

// ================= Pin Assignments =================
const int PIN_ENA = 14; const int PIN_IN1 = 27; const int PIN_IN2 = 26;
const int PIN_ENB = 32; const int PIN_IN3 = 33; const int PIN_IN4 = 25;
const int PWM_FREQ = 1000; const int PWM_RESOLUTION = 8;    

// ================= STRICT ESP32 SAFETY MECHANISMS =================
const int DEFAULT_MEDIUM_SPEED = 165;  
const int TURN_SPEED_OFFSET    = 25;   
const int MIN_START_THRESHOLD  = 70;   
const unsigned long RAMP_INTERVAL_MS = 25;       // SAFETY 1: Soft-start prevents inrush current
const unsigned long REVERSAL_DEADTIME_MS = 200;  // SAFETY 2: 200ms deadband blocks flyback voltage

int targetLeftPWM = 0; int targetRightPWM = 0;
int currentLeftPWM = 0; int currentRightPWM = 0;
int userBaseSpeed = DEFAULT_MEDIUM_SPEED;

unsigned long lastRampUpdate = 0;
unsigned long deadbandStart = 0;

// ================= STRICT TIMING MEMORY ENGINE =================
struct RouteStep { 
  char command; 
  unsigned long duration; 
};

const int MAX_STEPS = 100;
RouteStep recordedRoute[MAX_STEPS];
int routeStepCount = 0;

bool isRecording = false; 
bool isPlaying = false;
unsigned long stepStartTime = 0; 
char currentRecordCmd = 'S'; 
int playbackIndex = 0; 
unsigned long playbackStepStartTime = 0;

enum DriveState { STATE_STOPPED, STATE_FORWARD, STATE_BACKWARD, STATE_PIVOT_LEFT, STATE_PIVOT_RIGHT, STATE_DEADBAND };
DriveState currentState = STATE_STOPPED;
DriveState pendingState = STATE_STOPPED;

// --- Low-Level Motor Control ---
void setHBridgeOutputs(DriveState state) {
  switch (state) {
    case STATE_FORWARD: digitalWrite(PIN_IN1, HIGH); digitalWrite(PIN_IN2, LOW); digitalWrite(PIN_IN3, HIGH); digitalWrite(PIN_IN4, LOW); break;
    case STATE_BACKWARD: digitalWrite(PIN_IN1, LOW); digitalWrite(PIN_IN2, HIGH); digitalWrite(PIN_IN3, LOW); digitalWrite(PIN_IN4, HIGH); break;
    case STATE_PIVOT_LEFT: digitalWrite(PIN_IN1, LOW); digitalWrite(PIN_IN2, HIGH); digitalWrite(PIN_IN3, HIGH); digitalWrite(PIN_IN4, LOW); break;
    case STATE_PIVOT_RIGHT: digitalWrite(PIN_IN1, HIGH); digitalWrite(PIN_IN2, LOW); digitalWrite(PIN_IN3, LOW); digitalWrite(PIN_IN4, HIGH); break;
    case STATE_STOPPED: case STATE_DEADBAND: default: digitalWrite(PIN_IN1, LOW); digitalWrite(PIN_IN2, LOW); digitalWrite(PIN_IN3, LOW); digitalWrite(PIN_IN4, LOW); break;
  }
}

// --- Safety Interlock Logic ---
void requestState(DriveState newState) {
  if (currentState == newState) return;
  
  // Trigger Safety Deadband if switching between active movements
  if (currentState != STATE_STOPPED && newState != STATE_STOPPED) {
    pendingState = newState; currentState = STATE_DEADBAND;
    targetLeftPWM = 0; targetRightPWM = 0; currentLeftPWM = 0; currentRightPWM = 0;
    ledcWrite(PIN_ENA, 0); ledcWrite(PIN_ENB, 0);
    setHBridgeOutputs(STATE_DEADBAND); 
    deadbandStart = millis(); // Start safety timer
    return;
  }
  
  currentState = newState; setHBridgeOutputs(currentState);

  if (currentState == STATE_FORWARD || currentState == STATE_BACKWARD) {
    targetLeftPWM = userBaseSpeed; targetRightPWM = userBaseSpeed;
  } else if (currentState == STATE_PIVOT_LEFT || currentState == STATE_PIVOT_RIGHT) {
    targetLeftPWM = max(MIN_START_THRESHOLD, userBaseSpeed - TURN_SPEED_OFFSET);
    targetRightPWM = max(MIN_START_THRESHOLD, userBaseSpeed - TURN_SPEED_OFFSET);
  } else {
    targetLeftPWM = 0; targetRightPWM = 0;
  }
}

void updateRampingEngine() {
  unsigned long now = millis();
  
  // Process Safety Deadband
  if (currentState == STATE_DEADBAND) {
    if (now - deadbandStart >= REVERSAL_DEADTIME_MS) { 
      currentState = pendingState; setHBridgeOutputs(currentState); requestState(pendingState); 
    }
    return;
  }
  
  // Process Soft-Start Acceleration
  if (now - lastRampUpdate >= RAMP_INTERVAL_MS) {
    lastRampUpdate = now;
    if (currentLeftPWM < targetLeftPWM) currentLeftPWM = (currentLeftPWM == 0) ? MIN_START_THRESHOLD : min(currentLeftPWM + 6, targetLeftPWM);
    else if (currentLeftPWM > targetLeftPWM) currentLeftPWM = max(currentLeftPWM - 10, targetLeftPWM);

    if (currentRightPWM < targetRightPWM) currentRightPWM = (currentRightPWM == 0) ? MIN_START_THRESHOLD : min(currentRightPWM + 6, targetRightPWM);
    else if (currentRightPWM > targetRightPWM) currentRightPWM = max(currentRightPWM - 10, targetRightPWM);

    ledcWrite(PIN_ENA, currentLeftPWM); ledcWrite(PIN_ENB, currentRightPWM);
    if (currentLeftPWM == 0 && currentRightPWM == 0 && currentState == STATE_STOPPED) setHBridgeOutputs(STATE_STOPPED);
  }
}

void executeMovement(char cmd) {
  switch (cmd) {
    case 'F': requestState(STATE_FORWARD); break;
    case 'B': requestState(STATE_BACKWARD); break;
    case 'L': requestState(STATE_PIVOT_LEFT); break;
    case 'R': requestState(STATE_PIVOT_RIGHT); break;
    case 'S': requestState(STATE_STOPPED); break;
  }
}

// ================= STRICT TIMING LOGGER =================
void saveRecordStep(char nextCmd) {
  // Uses hardware millis() to strictly lock the timing
  unsigned long duration = millis() - stepStartTime;
  
  if (duration > 25 && routeStepCount < MAX_STEPS) { 
    recordedRoute[routeStepCount].command = currentRecordCmd;
    recordedRoute[routeStepCount].duration = duration;
    routeStepCount++;
  }
  
  currentRecordCmd = nextCmd; 
  stepStartTime = millis();
}

void updatePlaybackEngine() {
  if (!isPlaying) return;
  
  // Strictly enforce the recorded millisecond duration
  if (millis() - playbackStepStartTime >= recordedRoute[playbackIndex].duration) {
    playbackIndex++;
    if (playbackIndex >= routeStepCount) {
      isPlaying = false; executeMovement('S'); Serial.println("--- PLAYBACK FINISHED ---");
    } else {
      executeMovement(recordedRoute[playbackIndex].command);
      playbackStepStartTime = millis(); // Reset stopwatch for the next step
    }
  }
}

void parseCommand(char cmd) {
  if (cmd == 'X') { 
    isRecording = true; isPlaying = false; routeStepCount = 0; 
    currentRecordCmd = 'S'; // Begin tracking the initial stopped state
    stepStartTime = millis(); 
    executeMovement('S'); 
    Serial.println("--- RECORDING STARTED ---"); 
    return;
  }
  if (cmd == 'x') { 
    if (isRecording) { 
      saveRecordStep('S'); // Lock in the final movement
      isRecording = false; 
      executeMovement('S'); 
      Serial.printf("--- SAVED %d STEPS ---\n", routeStepCount); 
    } 
    return;
  }
  if (cmd == 'Y') { 
    if (routeStepCount > 0 && !isRecording) {
      isPlaying = true; playbackIndex = 0; 
      playbackStepStartTime = millis();
      Serial.println("--- PLAYING REPEAT MODE ---");
      executeMovement(recordedRoute[0].command); 
    } 
    return;
  }

  // Real-time Driving (Triggered by Keyboard)
  if (cmd == 'F' || cmd == 'B' || cmd == 'L' || cmd == 'R' || cmd == 'S') {
    if (isPlaying) { isPlaying = false; Serial.println("Playback Interrupted!"); }
    
    // If the command changes, strictly record the exact time spent in the PREVIOUS command
    if (isRecording && cmd != currentRecordCmd) {
      saveRecordStep(cmd);
    }
    executeMovement(cmd); 
  }
}

void setup() {
  // ESP32 CORE SAFETY: Disables panic restarts if voltage drops slightly
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); 
  
  Serial.begin(115200);
  pinMode(PIN_IN1, OUTPUT); pinMode(PIN_IN2, OUTPUT); 
  pinMode(PIN_IN3, OUTPUT); pinMode(PIN_IN4, OUTPUT);
  setHBridgeOutputs(STATE_STOPPED);

  ledcAttach(PIN_ENA, PWM_FREQ, PWM_RESOLUTION); ledcAttach(PIN_ENB, PWM_FREQ, PWM_RESOLUTION);
  ledcWrite(PIN_ENA, 0); ledcWrite(PIN_ENB, 0);

  WiFi.softAP(ssid, password); 
  udp.begin(localUdpPort);
  Serial.println("AP Ready. Safe Pre-Mapping Engine Active.");
}

void loop() {
  if (udp.parsePacket()) {
    int len = udp.read(incomingPacket, 254);
    if (len > 0) {
      incomingPacket[len] = 0;
      if (len == 1) parseCommand(incomingPacket[0]); 
    }
  }
  updatePlaybackEngine(); 
  updateRampingEngine(); 
}
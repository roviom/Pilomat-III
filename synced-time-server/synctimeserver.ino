#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

// Access Point credentials
const char* ap_ssid = "ESP32-Timer";
const char* ap_password = "timer1234";

WebServer server(80);

// Runtime state structure
struct RuntimeState {
  // Display configuration
  String displayMode;        // "center", "left-right"
  int centerDisplay;
  int leftDisplay;
  int rightDisplay;
  bool centerCountDown;
  bool leftCountDown;
  bool rightCountDown;
  bool centerPaused;
  bool leftPaused;
  bool rightPaused;
  
  // Timer control
  unsigned long counterStartMillis;
  int counter;               // Master counter in seconds
  bool counterRunning;
  int maxCounter;
  
  // State machine
  int currentStateIndex;
  unsigned long stateStartMillis;
  String currentStateName;
  
  // Traffic light
  String trafficLight;       // "red", "green", "yellow" (for center mode)
  String trafficLightLeft;   // For left-right mode
  String trafficLightRight;  // For left-right mode
  
  // Line/End tracking
  String lineIdentifier;     // "ABcd", "abCD", etc.
  int currentEnd;
  int totalEnds;
  
  // Emergency stop
  bool emergencyStop;
};

RuntimeState runtime;

// Configuration structure
struct TimerConfig {
  String name;
  String displayMode;
  int maxCounter;
  // States and events loaded dynamically from JSON
};

TimerConfig config;
DynamicJsonDocument configDoc(8192);  // Holds full JSON config

// Buzzer control
const int BUZZER_PIN = 2;
bool buzzerEnabled = true;
unsigned long buzzerStopTime = 0;
int beepsRemaining = 0;
unsigned long lastBeepTime = 0;
int buzzerFrequency = 2000;

// SSE clients
WiFiClient sseClients[10];
int clientCount = 0;

void initRuntime() {
  runtime.displayMode = "center";
  runtime.centerDisplay = 0;
  runtime.leftDisplay = 0;
  runtime.rightDisplay = 0;
  runtime.centerCountDown = true;
  runtime.leftCountDown = true;
  runtime.rightCountDown = true;
  runtime.centerPaused = false;
  runtime.leftPaused = false;
  runtime.rightPaused = false;
  runtime.counterStartMillis = 0;
  runtime.counter = 0;
  runtime.counterRunning = false;
  runtime.maxCounter = 999;
  runtime.currentStateIndex = 0;
  runtime.stateStartMillis = 0;
  runtime.currentStateName = "Stopped";
  runtime.trafficLight = "red";
  runtime.trafficLightLeft = "red";
  runtime.trafficLightRight = "red";
  runtime.lineIdentifier = "A";
  runtime.currentEnd = 0;
  runtime.totalEnds = 1;
  runtime.emergencyStop = false;
}

void loadDefaultConfig() {
  // Load a simple default configuration
  configDoc.clear();
  
  configDoc["name"] = "Simple Timer";
  configDoc["displayMode"] = "center";
  configDoc["maxCounter"] = 999;
  
  JsonArray states = configDoc.createNestedArray("states");
  JsonObject state1 = states.createNestedObject();
  state1["name"] = "Preparation";
  state1["duration"] = 10;
  
  JsonObject state2 = states.createNestedObject();
  state2["name"] = "Shooting";
  state2["duration"] = 120;
  
  JsonArray events = configDoc.createNestedArray("events");
  
  JsonArray buttons = configDoc.createNestedArray("buttons");
  JsonObject btn1 = buttons.createNestedObject();
  btn1["label"] = "START";
  btn1["class"] = "start";
  JsonObject actions1 = btn1.createNestedObject("actions");
  actions1["resetCounter"] = true;
  actions1["trafficLight"] = "green";
  
  config.name = "Simple Timer";
  config.displayMode = "center";
  config.maxCounter = 999;
  
  runtime.displayMode = config.displayMode;
  runtime.maxCounter = config.maxCounter;
}

void executeAction(JsonObject actions) {
  // Traffic light control
  if (actions.containsKey("trafficLight")) {
    String light = actions["trafficLight"].as<String>();
    runtime.trafficLight = light;
    runtime.trafficLightLeft = light;   // Apply to both in center mode
    runtime.trafficLightRight = light;
  }
  
  if (actions.containsKey("trafficLightLeft")) {
    runtime.trafficLightLeft = actions["trafficLightLeft"].as<String>();
  }
  
  if (actions.containsKey("trafficLightRight")) {
    runtime.trafficLightRight = actions["trafficLightRight"].as<String>();
  }
  
  // Display control - center
  if (actions.containsKey("setDisplay")) {
    JsonObject displays = actions["setDisplay"];
    if (displays.containsKey("center")) {
      runtime.centerDisplay = displays["center"];
    }
    if (displays.containsKey("left")) {
      runtime.leftDisplay = displays["left"];
    }
    if (displays.containsKey("right")) {
      runtime.rightDisplay = displays["right"];
    }
  }
  
  // Countdown control
  if (actions.containsKey("setCountDown")) {
    JsonObject countdown = actions["setCountDown"];
    if (countdown.containsKey("center")) {
      runtime.centerCountDown = countdown["center"];
    }
    if (countdown.containsKey("left")) {
      runtime.leftCountDown = countdown["left"];
    }
    if (countdown.containsKey("right")) {
      runtime.rightCountDown = countdown["right"];
    }
  }
  
  // Pause/Resume displays
  if (actions.containsKey("pauseDisplay")) {
    JsonArray paused = actions["pauseDisplay"];
    for (JsonVariant v : paused) {
      String display = v.as<String>();
      if (display == "center") runtime.centerPaused = true;
      if (display == "left") runtime.leftPaused = true;
      if (display == "right") runtime.rightPaused = true;
    }
  }
  
  if (actions.containsKey("resumeDisplay")) {
    JsonArray resumed = actions["resumeDisplay"];
    for (JsonVariant v : resumed) {
      String display = v.as<String>();
      if (display == "center") runtime.centerPaused = false;
      if (display == "left") runtime.leftPaused = false;
      if (display == "right") runtime.rightPaused = false;
    }
  }
  
  // Counter control
  if (actions.containsKey("resetCounter")) {
    runtime.counter = 0;
    runtime.counterStartMillis = millis();
    runtime.counterRunning = true;
  }
  
  if (actions.containsKey("resetState")) {
    runtime.currentStateIndex = 0;
    runtime.stateStartMillis = millis();
    if (configDoc["states"].size() > 0) {
      runtime.currentStateName = configDoc["states"][0]["name"].as<String>();
    }
  }
  
  // Buzzer control
  if (actions.containsKey("buzzer")) {
    JsonObject buzzer = actions["buzzer"];
    if (buzzer.containsKey("cycles")) {
      playBeeps(buzzer["cycles"], buzzer.containsKey("frequency") ? buzzer["frequency"].as<int>() : 2);
    } else if (buzzer.containsKey("duration")) {
      playBeeps(1, 2);  // Single beep for duration-based
    }
  }
}

void checkEvents() {
  if (!configDoc.containsKey("events")) return;
  
  JsonArray events = configDoc["events"];
  static int lastCounter = -1;
  
  for (JsonObject event : events) {
    if (event["trigger"] == "counter") {
      int triggerValue = event["value"];
      
      // Trigger when counter REACHES this value (only once)
      if (runtime.counter >= triggerValue && lastCounter < triggerValue) {
        Serial.print("Event triggered at counter ");
        Serial.print(runtime.counter);
        Serial.print(": ");
        Serial.println(event["name"].as<String>());
        executeAction(event["actions"]);
      }
    }
  }
  
  lastCounter = runtime.counter;
}

void playBeeps(int count, int freqType) {
  if (!buzzerEnabled) return;
  beepsRemaining = count;
  lastBeepTime = 0;
  buzzerFrequency = (freqType == 2) ? 2000 : (freqType == 3) ? 2500 : 1500;
}

void updateBuzzer() {
  if (beepsRemaining <= 0) return;
  
  unsigned long now = millis();
  
  if (lastBeepTime == 0) {
    tone(BUZZER_PIN, buzzerFrequency);
    buzzerStopTime = now + 200;
    lastBeepTime = now;
  } else if (now >= buzzerStopTime && digitalRead(BUZZER_PIN)) {
    noTone(BUZZER_PIN);
    beepsRemaining--;
    if (beepsRemaining > 0) {
      lastBeepTime = now + 100;
    }
  } else if (beepsRemaining > 0 && now >= lastBeepTime && lastBeepTime > buzzerStopTime) {
    tone(BUZZER_PIN, buzzerFrequency);
    buzzerStopTime = now + 200;
    lastBeepTime = now;
  }
}

void updateDisplayTimers() {
  static unsigned long lastCenterUpdate = 0;
  static unsigned long lastLeftUpdate = 0;
  static unsigned long lastRightUpdate = 0;
  unsigned long now = millis();
  
  // Update center display
  if (!runtime.centerPaused && runtime.centerDisplay > 0) {
    if (lastCenterUpdate == 0) lastCenterUpdate = now;
    unsigned long elapsed = (now - lastCenterUpdate) / 1000;
    
    if (elapsed > 0) {
      if (runtime.centerCountDown) {
        runtime.centerDisplay = max(0, runtime.centerDisplay - (int)elapsed);
      } else {
        runtime.centerDisplay += elapsed;
      }
      lastCenterUpdate = now;
    }
  } else if (runtime.centerPaused) {
    lastCenterUpdate = now;
  }
  
  // Update left display (independent timer)
  if (!runtime.leftPaused && runtime.leftDisplay > 0) {
    if (lastLeftUpdate == 0) lastLeftUpdate = now;
    unsigned long elapsed = (now - lastLeftUpdate) / 1000;
    
    if (elapsed > 0) {
      if (runtime.leftCountDown) {
        runtime.leftDisplay = max(0, runtime.leftDisplay - (int)elapsed);
      } else {
        runtime.leftDisplay += elapsed;
      }
      lastLeftUpdate = now;
    }
  } else if (runtime.leftPaused) {
    lastLeftUpdate = now;
  }
  
  // Update right display (independent timer)
  if (!runtime.rightPaused && runtime.rightDisplay > 0) {
    if (lastRightUpdate == 0) lastRightUpdate = now;
    unsigned long elapsed = (now - lastRightUpdate) / 1000;
    
    if (elapsed > 0) {
      if (runtime.rightCountDown) {
        runtime.rightDisplay = max(0, runtime.rightDisplay - (int)elapsed);
      } else {
        runtime.rightDisplay += elapsed;
      }
      lastRightUpdate = now;
    }
  } else if (runtime.rightPaused) {
    lastRightUpdate = now;
  }
}

void handleRoot() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleControl() {
  File file = LittleFS.open("/control.html", "r");
  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleCSS() {
  File file = LittleFS.open("/style.css", "r");
  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  server.streamFile(file, "text/css");
  file.close();
}

void handleGetConfig() {
  String json;
  serializeJson(configDoc, json);
  server.send(200, "application/json", json);
}

void handleSetConfig() {
  if (server.hasArg("plain")) {
    DeserializationError error = deserializeJson(configDoc, server.arg("plain"));
    
    if (!error) {
      config.name = configDoc["name"].as<String>();
      config.displayMode = configDoc["displayMode"].as<String>();
      config.maxCounter = configDoc["maxCounter"];
      
      runtime.displayMode = config.displayMode;
      runtime.maxCounter = config.maxCounter;
      
      StaticJsonDocument<100> response;
      response["status"] = "ok";
      String json;
      serializeJson(response, json);
      server.send(200, "application/json", json);
    } else {
      server.send(400, "text/plain", "Invalid JSON");
    }
  } else {
    server.send(400, "text/plain", "No data");
  }
}

void handleGetStatus() {
  // JSON API for external modules (traffic lights, TV graphics, etc.)
  DynamicJsonDocument doc(2048);
  
  // Display state
  doc["displayMode"] = runtime.displayMode;
  JsonObject displays = doc.createNestedObject("displays");
  displays["center"] = runtime.centerDisplay;
  displays["left"] = runtime.leftDisplay;
  displays["right"] = runtime.rightDisplay;
  
  JsonObject paused = doc.createNestedObject("paused");
  paused["center"] = runtime.centerPaused;
  paused["left"] = runtime.leftPaused;
  paused["right"] = runtime.rightPaused;
  
  JsonObject countdown = doc.createNestedObject("countdown");
  countdown["center"] = runtime.centerCountDown;
  countdown["left"] = runtime.leftCountDown;
  countdown["right"] = runtime.rightCountDown;
  
  // Timer state
  doc["counter"] = runtime.counter;
  doc["counterRunning"] = runtime.counterRunning;
  doc["maxCounter"] = runtime.maxCounter;
  
  // State machine
  doc["currentState"] = runtime.currentStateName;
  doc["currentStateIndex"] = runtime.currentStateIndex;
  
  // Traffic light
  doc["trafficLight"] = runtime.trafficLight;
  doc["trafficLightLeft"] = runtime.trafficLightLeft;
  doc["trafficLightRight"] = runtime.trafficLightRight;
  
  // Line/End info
  doc["lineIdentifier"] = runtime.lineIdentifier;
  doc["currentEnd"] = runtime.currentEnd;
  doc["totalEnds"] = runtime.totalEnds;
  
  // Emergency
  doc["emergencyStop"] = runtime.emergencyStop;
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleTimeSync() {
  StaticJsonDocument<100> doc;
  doc["serverTime"] = millis();
  
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

void handleButtonAction() {
  if (server.hasArg("plain")) {
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, server.arg("plain"));
    
    if (!error && doc.containsKey("actions")) {
      executeAction(doc["actions"]);
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "Invalid action");
    }
  } else {
    server.send(400, "text/plain", "No data");
  }
}

void handleEvents() {
  WiFiClient client = server.client();
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: text/event-stream");
  client.println("Cache-Control: no-cache");
  client.println("Connection: keep-alive");
  client.println();
  
  if (clientCount < 10) {
    sseClients[clientCount] = client;
    clientCount++;
  }
  
  // Send current state immediately
  broadcastState();
}

void broadcastState() {
  DynamicJsonDocument doc(1024);
  
  doc["displayMode"] = runtime.displayMode;
  doc["center"] = runtime.centerDisplay;
  doc["left"] = runtime.leftDisplay;
  doc["right"] = runtime.rightDisplay;
  doc["trafficLight"] = runtime.trafficLight;
  doc["trafficLightLeft"] = runtime.trafficLightLeft;
  doc["trafficLightRight"] = runtime.trafficLightRight;
  doc["counter"] = runtime.counter;
  doc["state"] = runtime.currentStateName;
  doc["line"] = runtime.lineIdentifier;
  doc["end"] = runtime.currentEnd;
  doc["paused"] = runtime.centerPaused;
  doc["pausedLeft"] = runtime.leftPaused;
  doc["pausedRight"] = runtime.rightPaused;
  
  String json;
  serializeJson(doc, json);
  String msg = "data: " + json + "\n\n";
  
  for (int i = 0; i < clientCount; i++) {
    if (sseClients[i].connected()) {
      size_t written = sseClients[i].print(msg);
      if (written > 0) {
        sseClients[i].flush();
      } else {
        sseClients[i].stop();
      }
    }
    
    if (!sseClients[i].connected()) {
      for (int j = i; j < clientCount - 1; j++) {
        sseClients[j] = sseClients[j + 1];
      }
      clientCount--;
      i--;
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(BUZZER_PIN, OUTPUT);
  
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }
  Serial.println("LittleFS mounted successfully");
  
  // Initialize runtime state
  initRuntime();
  
  // Load default configuration
  loadDefaultConfig();
  
  // Set up Access Point
  Serial.println("\nSetting up Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.println("\nAccess Point started!");
  Serial.print("AP SSID: ");
  Serial.println(ap_ssid);
  Serial.print("AP IP: ");
  Serial.println(IP);
  Serial.println("Go to: http://192.168.4.1");
  
  // Setup web server
  server.on("/", handleRoot);
  server.on("/control", handleControl);
  server.on("/style.css", handleCSS);
  server.on("/config", HTTP_GET, handleGetConfig);
  server.on("/config", HTTP_POST, handleSetConfig);
  server.on("/api/status", handleGetStatus);
  server.on("/api/action", HTTP_POST, handleButtonAction);
  server.on("/time", handleTimeSync);
  server.on("/events", handleEvents);
  server.begin();
  
  Serial.println("Web server started");
}

void loop() {
  server.handleClient();
  updateBuzzer();
  
  static unsigned long lastBroadcast = 0;
  unsigned long currentMicros = micros();
  
  if (currentMicros - lastBroadcast >= 50000) {
    lastBroadcast = currentMicros;
    
    // Update master counter
    if (runtime.counterRunning) {
      unsigned long elapsed = (millis() - runtime.counterStartMillis) / 1000;
      runtime.counter = elapsed;
      
      if (runtime.counter >= runtime.maxCounter) {
        runtime.counterRunning = false;
      }
    }
    
    // Update display timers
    updateDisplayTimers();
    
    // Check for triggered events
    checkEvents();
    
    // Broadcast state to all clients
    broadcastState();
  }
  
  yield();
}

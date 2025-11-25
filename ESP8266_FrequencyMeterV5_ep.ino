//Serhii Bazavluk @ Electrical Projects [CreativeLab]
//https://www.youtube.com/c/ElectricalPro/videos

// url: 192.168.x.x:8089/
// file manager at 192.168.x.x:8089/littlefs content from data folder should be uploaded (format & upload)

// [NEW] 081125 tested with Core v3.1.2 and WebSocketsClient.h v2.7.1, bug fixed, rewritten, cleaned
//remove this line: #define USE_OLED_DISPLAY to use without display

#include <Arduino.h>
#include <ArduinoOTA.h> // for OTA
#include <ESP8266WebServer.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <FS.h>   
#include <LittleFS.h>
#include <ArduinoJson.h> // json 7.4.2
#include <StreamString.h>
#include <Hash.h>
#include "littlefs_file_manager.h" // gziped file maneger 

const char* ssid = "myName";
const char* password = "myPassword";

//#define SPECIFY_MAC_ADDRESS //(enable if needed)
#ifdef SPECIFY_MAC_ADDRESS
  // MAC address of the specific AP (BSSID), if defined, it will only connect if MAC matches MAC of AP
  uint8_t bssid[] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff}; // mac
#endif

// work together with distribution board
//#define FEED_FREQUNCY_VIA_WS //(enable if needed)
#ifdef FEED_FREQUNCY_VIA_WS
  #include <WebSocketsClient.h> // ws 2.7.1 https://github.com/Links2004/arduinoWebSockets
  WebSocketsClient webSocketPWRCTRL;
  bool isConnectedToPowerCtrl = false;
#endif

#define BLUE_LED_PIN 2
#define INTR_INPUT_PIN  D5 //interupt pin

#define blink_low_freq 49.91 //blink blue led when frequency is lover than XX Hz

uint32_t intrptArray[32]; // intturupt micros array, stores time when interrupts happened (store on every new cycle)
float freqHzCalcArray[32]; // array to store last XX freq calculations (store on every new cycle)

// just to send somewhere if you need last values
float historyFreqHzArray[16]; // array to store last 16 frequencies
float historyFreqHzArray_chng[16]; // store 16 changes in frequncy

uint32_t lastInterTime = 0;
bool isr_done_flag = false;

// debug statistic
uint32_t false_int = 0;
uint32_t falseInt_us = 0;
uint32_t total_int = 0;
uint32_t isr_time = 0;
String falseIntTime = "";

float freqHz=0; // last frequncy
float recordLowerstHz = 0;
float recordHighestHz = 0;

bool WifiOnlineStateFlag = false; //  false - offline, true - online
uint32_t wifiApCheckMillLast = 0; // for wifi in offline mode

ESP8266WebServer server(8089);

//-------------------------------------------------------------------------------------------
//=========ThingSpeak
String writeAPIKey = "---APIKEY----"; // write API key for your ThingSpeak Channel
bool SendDataToServer = false;   //  set to false to NOT send data to ThingSpeak
bool thingSpeakError = true; // stores result of the last try
//-------------------------------------------------------------------------------------------

#define USE_OLED_DISPLAY // coment out to disable
#ifdef USE_OLED_DISPLAY
  #include <Wire.h>
  #include <Adafruit_SSD1306.h>  // 2.5.0
  #include <Adafruit_GFX.h>
  // display config
  #define DISPLAY_ADDRESS 0x3C // 0x3D ??
  #define SCREEN_WIDTH 128 // OLED display width, in pixels
  #define SCREEN_HEIGHT 64 // OLED display height, in pixels
  #define OLED_RESET -1

  // SSD1306 Show stuff
  #define MIN_INPUT 49.91 // chart lowest (chart range)
  #define MAX_INPUT 50.08 // chart highest (chart range)
  #define CHART "LINE" // chart type > options: "BAR", "LINE", "FILL", FILL_C
  #define CHART_BAR_PX 5 // Chart bar line pixels, try 1, 3, 5, 7, 9
  #define CHART_LINE_PX 3 // try 1, 3, 5, 7, 9
  #define STATUS_BAR_SMALL true // show small or big status bar (freq at top left corner)
  #define SAVE_OLED true // move status bar a bit to save oled (help with "burn-in" OLED issue?)
  #define AUTO_DIM_DISPLAY false // make display very dark after X sec, by changing pre-changed period (true = auto dark, false = off), helps to save OLED from "burn-in"
  #define DISPLAY_BRIGHTNESS 130 // 0-255 range (255 max brightness)  //130
  #define BUTTON_MODE_PIN 0 // pin for the "flash" button (chat / text mode)
  bool SHOW_CHART = true; // Show chart or show big frequency value (flash button changes it, try pressing it)
  int32_t lastActivityMls = 0;
  Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
#endif


//-------------------------ISR----------------------------//
void ICACHE_RAM_ATTR handleInterrupt()  //This is called on interrupt
{
  // measures frequency in range 40Hz - 66.6Hz
  uint32_t micros_int = micros();

  // filter low limit 25ms = 40 Hz
  if ((uint32_t)(micros_int - lastInterTime) > 25000)
  {
    // something really wrong, interupt not detected in the right time, (too late), restart ESP
    if (millis() > 10000) {
      ESP.restart();
    }
  }

  // filter high 16ms = 62.5Hz
  // helps to ignore false interrupts that may happen
  if ((uint32_t)(micros_int - lastInterTime) > 16000)
  {
    lastInterTime = micros_int;
    const uint32_t INTRPT_ARRAY_SIZE = sizeof(intrptArray) / sizeof(intrptArray[0]);
    memcpy(&intrptArray[0], &intrptArray[1], (INTRPT_ARRAY_SIZE - 1) * sizeof(uint32_t)); // Shift array left by one element (remove oldest, make room for newest)
    intrptArray[INTRPT_ARRAY_SIZE - 1] = micros_int; // Add newest timestamp at end
  }
  else
  {
    // false interrupt, too soon
    false_int++;
    falseInt_us = micros() - micros_int;
  }
  total_int++;
  isr_time = micros() - micros_int;
  isr_done_flag = true;
}
//-------------------------ISR----------------------------//


// -------------------------sutup stuff -----------------//
void setup()
{
  Serial.begin(115200);
  delay(1000);

  pinMode(INTR_INPUT_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(INTR_INPUT_PIN), handleInterrupt, FALLING); // RISING or FALLING, or CHANGE

  pinMode(BLUE_LED_PIN, OUTPUT); // indication works as an output
  digitalWrite(BLUE_LED_PIN, LOW); // Indication pin set for low.

  #ifdef USE_OLED_DISPLAY
    pinMode(BUTTON_MODE_PIN, INPUT_PULLUP); // chat / text mode
    if (!display.begin(SSD1306_SWITCHCAPVCC, DISPLAY_ADDRESS)) { // Address 0x3D for 128x64
      Serial.println(F("SSD1306 allocation failed"));
      for (;;); // Don't proceed, loop forever
    }
    display.clearDisplay();
    display.setTextSize(3);             // Normal 1:1 pixel scale
    display.setTextColor(WHITE);        // Draw white text
    display.setCursor(22, 10);            // Start at top-left corner
    display.println("Hello");
    display.setTextSize(1);
    display.setCursor(5, 40);            // Start at top-left corner
    display.println("I'm Frequency Meter");
    display.display();
    delay(1700);
  #endif

  // Mount LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    delay(5000);
  }

  delay(1700);
  
  WiFi.hostname("FreqMeterESP");
  delay(500);
  WiFi.mode(WIFI_STA); // STA mode.
  delay(500);
  Serial.println("===============================");
  Serial.println("WIFI_STA mode");

  #ifdef SPECIFY_MAC_ADDRESS
    WiFi.begin(ssid, password, 0, bssid);
  #else
    WiFi.begin(ssid, password);
  #endif

  Serial.print("Connecting");
  
  while (WiFi.status() != WL_CONNECTED && millis() < 90000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nFailed to connect. Restarting...");
    ESP.restart();
  }

  // good blinking 
  digitalWrite(BLUE_LED_PIN, HIGH);
  delay(600);
  digitalWrite(BLUE_LED_PIN, LOW);
  delay(175);
  digitalWrite(BLUE_LED_PIN, HIGH);
  delay(175);
  digitalWrite(BLUE_LED_PIN, LOW);
  delay(175);
  digitalWrite(BLUE_LED_PIN, HIGH);

  delay(500);

  // time zone for time (not necessary)
  configTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org");


  // =========== over the air updates ==============
  ArduinoOTA.onEnd([]() { // do a fancy thing with our board led at end
    for (int i = 0; i < 30; i++)
    {
      analogWrite(BLUE_LED_PIN, (i * 100) % 1001);
      delay(25);
    }
    // restart after ota
    ESP.restart();
  });

  ArduinoOTA.setHostname("FreqMeterESP");
  ArduinoOTA.onStart([]() {
    detachInterrupt(digitalPinToInterrupt(INTR_INPUT_PIN)); // detach. Seems like interrupts interfere with update?
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
    ESP.restart();
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r\n", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
    delay(4000);
    ESP.restart();
  });
  ArduinoOTA.begin(); // for OTA
  // =========== over the air updates ============

  delay(500);

//========================FileServer======================================================//
  server.onNotFound([]() {
    server.sendHeader("Access-Control-Allow-Origin", "*");

    String path = server.uri();
    // if (path == "/") path = "/control_WS_css_V2_7.html"; // for root
    
    // Prefer gzipped version if available
    String gzPath = path + ".gz";
    if (LittleFS.exists(gzPath)) {
      path = gzPath;
    } else if (!LittleFS.exists(path)) {
      server.send(404, "text/plain", "404: Not Found");
      return;
    }
    
    // Determine content type
    String uri = path;
    if (uri.endsWith(".gz")) uri = uri.substring(0, uri.length() - 3); //remove .gz so we give correct contentType
    
    String contentType = "text/plain";
    if (uri.endsWith(".htm") || uri.endsWith(".html")) contentType = "text/html";
    else if (uri.endsWith(".css")) contentType = "text/css";
    else if (uri.endsWith(".js")) contentType = "application/javascript";
    else if (uri.endsWith(".png")) contentType = "image/png";
    else if (uri.endsWith(".gif")) contentType = "image/gif";
    else if (uri.endsWith(".jpg")) contentType = "image/jpeg";
    else if (uri.endsWith(".ico")) contentType = "image/x-icon";
    else if (uri.endsWith(".xml")) contentType = "text/xml";
    else if (uri.endsWith(".pdf")) contentType = "application/x-pdf";
    else if (uri.endsWith(".zip")) contentType = "application/x-zip";
    
    File file = LittleFS.open(path, "r");
    server.streamFile(file, contentType);
    file.close();
  });


  //========================FileManager======================================================//
  server.on("/littlefs", HTTP_GET, []() {
    server.sendHeader("Content-Encoding", "gzip");
    server.send_P(200, "text/html", (const char*)littleFS_html, sizeof(littleFS_html));
  });

  // List files endpoint
  server.on("/list", HTTP_GET, []() {
    Dir dir = LittleFS.openDir("/");

    String json = "{\"files\":[";
    bool first = true;

    while (dir.next()) {
      if (!first) json += ",";
      json += "{\"name\":\"" + dir.fileName() + "\",";
      json += "\"size\":" + String(dir.fileSize()) + "}";
      first = false;
      delay(1);
    }

    FSInfo fs_info;
    LittleFS.info(fs_info);

    json += "],";
    json += "\"totalBytes\":" + String(fs_info.totalBytes) + ",";
    json += "\"usedBytes\":" + String(fs_info.usedBytes) + "}";

    server.send(200, "application/json", json);
  });

  // Delete file endpoint
  server.on("/delete", HTTP_DELETE, []() {
    if (server.hasArg("file")) {
      String filename = "/" + server.arg("file");
      if (LittleFS.exists(filename)) {
        if (LittleFS.remove(filename)) {
          Serial.println("Deleted: " + filename);
          server.send(200, "text/plain", "OK");
        } else {
          server.send(500, "text/plain", "Delete failed");
        }
      } else {
        server.send(404, "text/plain", "File not found");
      }
    } else {
      server.send(400, "text/plain", "Missing file parameter");
    }
  });

  // Format LittleFS endpoint
  server.on("/format", HTTP_POST, []() {
    Serial.println("Formatting LittleFS...");

    LittleFS.end();  // Unmount first

    if (!LittleFS.format()) {
      Serial.println("Format failed");
      server.send(500, "text/plain", "Format failed");
      LittleFS.begin();
      return;
    }

    if (!LittleFS.begin()) {
      Serial.println("Failed to mount after format");
      server.send(500, "text/plain", "Mount failed after format");
      return;
    }

    Serial.println("LittleFS formatted successfully");
    server.send(200, "text/plain", "OK");
  });

  // Handle file upload
  server.on(
    "/upload", HTTP_POST,
    []() { server.send(200, "text/plain", "OK"); },
    []() {
      HTTPUpload& upload = server.upload();

      if (upload.status == UPLOAD_FILE_START) {
        String filename = "/" + upload.filename;
        Serial.printf("Upload Start: %s\n", filename.c_str());
        File file = LittleFS.open(filename, "w");
        if (!file) {
          Serial.println("Failed to open file for writing");
          return;
        }
        file.close();

      } else if (upload.status == UPLOAD_FILE_WRITE) {
        File file = LittleFS.open("/" + upload.filename, "a");
        if (file) {
          file.write(upload.buf, upload.currentSize);
          file.close();
        }

      } else if (upload.status == UPLOAD_FILE_END) {
        Serial.printf("Upload Complete: %s (%u bytes)\n",
                      upload.filename.c_str(), upload.totalSize);
      }
    });
  //========================FileManager======================================================//



  // =================== on index ===//
  server.on("/", []() {
    server.sendHeader("access-control-allow-origin", "*");
    
    String css = "body{background-color:#000;font-family:Arial;color:#fff}"
                "a:link,a:visited{color:#03fc9c;text-decoration:none}"
                "a:hover,a:active{color:red;text-decoration:underline}"
                "hr{max-width:380px;margin-left:0;border:0;border-top:1px solid #00a3a3}";
    
    String page = "<head><title>Frequency Meter</title>"
                  "<meta name='theme-color'content='#042B3C'>"
                  "<meta http-equiv='refresh'content='60'>"
                  "<style>" + css + "</style></head>"
                  "<h2>Hello, I'm Frequency Meter!</h2><hr>"
                  "Frequency now: <b>" + String(freqHz, 4) + " Hz</b><hr>"
                  "Highest detected: <b>" + String(recordHighestHz, 4) + " Hz</b><br>"
                  "Lowest detected: <b>" + String(recordLowerstHz, 4) + " Hz</b><hr>"
                  "> <a href='/Frequency_Live_150ms.html'>Chart 150 ms</a><br>"
                  "> <a href='/Frequency_Live_500ms.html'>Chart 500 ms</a><br>"
                  "> <a href='/Frequency_Live_1s.html'>Chart 1 s</a><br>"
                  "> <a href='/Frequency_Live_4s.html'>Chart 4 s</a><br>"
                  "> <a href='/Feed_JSON'>JSON feed</a><hr>"
                  "> <a href='/littlefs'>Littlefs file manager</a><hr>"
                  "> <a href='https://www.youtube.com/c/ElectricalPro/videos'>More information on YouTube</a><hr>";
    
    server.send(200, "text/html", page);
  });
  // =================== index ===//


  // =================== JSON Feed ===//
  server.on("/Feed_JSON", []() {
    server.sendHeader("access-control-allow-origin", "*");
    server.send(200, "application/json", jsonFeedGet());
  });
  // =================== JSON Feed ===//

  // =================== JSON Feed ===//
  server.on("/Feed_JSON_mini", []() {

    StaticJsonDocument<64> root;
    root["freqHz"] = freqHz;
    String output;
    serializeJson(root, output);

    server.sendHeader("access-control-allow-origin", "*");
    server.send(200, "application/json", output);
  });
  // =================== JSON Feed ===//

  server.begin();
  Serial.println("HTTP server started");


  #ifdef FEED_FREQUNCY_VIA_WS
    //============================WS==POWER MONITOR==================================//
    // server address, port and URL
    webSocketPWRCTRL.begin("192.168.4.215", 8092, "/");
    webSocketPWRCTRL.onEvent(webSocketEventPowerCtrl);
    webSocketPWRCTRL.setReconnectInterval(17000);
    // start heartbeat (optional)
    // ping server every 67000 ms
    // expect pong from server within 3000 ms
    // consider connection disconnected if pong is not received 2 times
    webSocketPWRCTRL.enableHeartbeat(12000, 4000, 1);
    //============================WS==POWER MONITOR==================================//
  #endif
}// end of setup loop


void loop()
{
  ArduinoOTA.handle(); // for OTA
  server.handleClient(); // server

  // =========== Lost connection with Wi-Fi========================
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("===============================");
    Serial.println("Lost connection to Wi-Fi");
    
    // Wait up to 60 seconds for automatic reconnection
    for (int i = 60; i > 0; i--) {
      Serial.print("Waiting for reconnection... Restarting in: ");
      Serial.print(i);
      Serial.println(" sec.");
      
      delay(1000); // Changed to 1000ms for accurate 1-second countdown
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Reconnected to Wi-Fi");
        break; // Exit loop cleanly
      }
    }
    
    // If still not connected after timeout, restart
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("Failed to reconnect. Restarting ESP...");
      ESP.restart();
    }
  }
  // =========== Lost connection with Wi-Fi========================


  // ================== track false ==============================
  static uint32_t false_int_last = false_int;
  if (false_int != false_int_last)
  {
    time_t TimeNow = time(nullptr);
    falseIntTime = ctime(&TimeNow);
    false_int_last = false_int;  // Update the last processed value
  }
  // ================== track false ==============================


  //===================================================================//
  // process inerrupt array, find frequncy, filter and awrage
  if (isr_done_flag)
  {
    isr_done_flag = false;
    //--------------- caclulate for all spans
    const uint32_t INTRPT_ARRAY_SIZE = sizeof(intrptArray) / sizeof(intrptArray[0]);
    const uint8_t spans = (INTRPT_ARRAY_SIZE / 2) - 2;

    // Make a safe copy of the array, to make sure interrupt will not ruin it
    uint32_t intrptArrayCopy[INTRPT_ARRAY_SIZE];
    // Disable interrupts only for the copy operation (very fast)
    noInterrupts();
    memcpy(intrptArrayCopy, intrptArray, sizeof(intrptArray));
    interrupts();

    float freqSamples[spans]; // samples for spans
    uint8_t sampleCount = 0;

    for (uint8_t i = 0; i < spans; i++) {
      uint32_t start = i;
      uint32_t end = INTRPT_ARRAY_SIZE - 1 - i;
      if (start >= end) break;

      uint32_t timeSpanUs = intrptArrayCopy[end] - intrptArrayCopy[start];
      uint32_t numCycles = end - start;

      if (timeSpanUs > 16000 * numCycles && timeSpanUs < 25000 * numCycles) {
        freqSamples[sampleCount++] = float(numCycles * 1000000) / float(timeSpanUs);
      }
    }
    //--------------- caclulate for all spans done ---------


    // -------------- filter spans -------------------------
    float freqHzSingleMeasure = 0;
    if (sampleCount > 0) {
      // Simple insertion sort (tiny array)
      for (uint8_t i = 1; i < sampleCount; i++) {
        float key = freqSamples[i];
        int8_t j = i - 1;
        while (j >= 0 && freqSamples[j] > key) {
          freqSamples[j + 1] = freqSamples[j];
          j--;
        }
        freqSamples[j + 1] = key;
      }

      uint8_t trim = 6; // how many extremes to cut off each end
      if (sampleCount > 2 * trim) {
        float sum = 0;
        for (uint8_t i = trim; i < sampleCount - trim; i++) sum += freqSamples[i];
        freqHzSingleMeasure = sum / float(sampleCount - 2 * trim);
      }
    }
    //----------- filter spans done, got freqHzSingleMeasure ------------

    // -------- do rolling average on freqHzSingleMeasure----------------
    const uint32_t FREQ_ARRAY_SIZE = sizeof(freqHzCalcArray) / sizeof(freqHzCalcArray[0]); // get size of array
    memcpy(&freqHzCalcArray[0], &freqHzCalcArray[1], (FREQ_ARRAY_SIZE - 1) * sizeof(float)); // Shift array left by one element
    freqHzCalcArray[FREQ_ARRAY_SIZE - 1] = freqHzSingleMeasure; // Add newest frequency

    // ===== Calculate average frequency =====
    float freqSum = 0;
    for (uint32_t i = 0; i < FREQ_ARRAY_SIZE; i++) {
      freqSum += freqHzCalcArray[i];
    }
    freqHz = freqSum / float(FREQ_ARRAY_SIZE);
    
    // ===== Track min/max records after 25 second after start =====
    if (millis() > 25000) {
      if (freqHz > recordHighestHz) {
        recordHighestHz = freqHz;
      }
      if (recordLowerstHz == 0 || freqHz < recordLowerstHz) {
        recordLowerstHz = freqHz;
      }
    }
    // -------- done rolling average on freqHzSingleMeasure got clean freqHz -------------
  }
  //===================================================================

  // ======================= update history array ============================//
  static int32_t updateArrayLast = 0;
  if (millis() > updateArrayLast + 1000) {
    //also here let's save value to array (not nessesery), we can later send this to telegram for example
    memcpy(&historyFreqHzArray[0], &historyFreqHzArray[1], sizeof(historyFreqHzArray) - sizeof(float)); // shift array
    memcpy(&historyFreqHzArray_chng[0], &historyFreqHzArray_chng[1], sizeof(historyFreqHzArray_chng) - sizeof(float)); // shift array
    historyFreqHzArray[sizeof(historyFreqHzArray) / sizeof(float) - 1] =  freqHz; // add last value
    historyFreqHzArray_chng[sizeof(historyFreqHzArray_chng) / sizeof(float) - 1] = historyFreqHzArray[sizeof(historyFreqHzArray) / sizeof(float) - 1] - historyFreqHzArray[sizeof(historyFreqHzArray) / sizeof(float) - 2]; // calculate change and add
    updateArrayLast = millis();
  }
  // ======================= update history array ============================//

  // ========================== blink led on freq ============================//
  if (freqHz < blink_low_freq) {
    digitalWrite(BLUE_LED_PIN, millis() >> 8 & 1);
  }
  else
  {
    digitalWrite(BLUE_LED_PIN, HIGH);
  }
  // ========================== blink led on freq ============================//


  // ============================= display ===================================//
  #ifdef USE_OLED_DISPLAY
    static uint32_t millisDispLast = 0; // status bar and other stuff
    if (millis() > millisDispLast + 2000)
    {
      // drow chart (I2C display)
      newChartValue(freqHz);
      drawDottedLines();
      drawStatusBar(freqHz);
      millisDispLast = millis();
    }

    // === status bar and other staff ===
    static uint32_t millisDispStatusBarLast = 0;
    if (millis() > millisDispStatusBarLast + 100)
    {
      if (!SHOW_CHART && !(freqHz > MAX_INPUT || freqHz < MIN_INPUT)) {
        static int saveScreen = 0;
        display.clearDisplay();
        display.setTextSize(3);
        display.setTextColor(WHITE);
        display.setCursor(0, saveScreen / 100);
        display.println(String(freqHz, 4));
        if (saveScreen > 4000) saveScreen = 0;
        saveScreen++;
      }

      if (SHOW_CHART && !(freqHz > MAX_INPUT || freqHz < MIN_INPUT)) drawStatusBar(freqHz);
      

      // change brightness on warnings (only if AUTO_DIM_DISPLAY is set to true)
      if (freqHz > MAX_INPUT || freqHz < MIN_INPUT) lastActivityMls = millis(); // for brightness

      if (millis() - lastActivityMls < 60000)
      {
        if (AUTO_DIM_DISPLAY) night(false);
      }
      else
      {
        if (AUTO_DIM_DISPLAY) night(true);
      }

      // === Warning to display ===
      if (freqHz < MIN_INPUT)
      {
        display.clearDisplay();
        display.setTextSize(3);
        display.setCursor(0, 12);
        display.println(String(freqHz, 4));
        display.setTextSize(2);
        display.setCursor(0, 42);
        display.println("VERY LOW!");
      }
      if (freqHz > MAX_INPUT)
      {
        display.clearDisplay();
        display.setTextSize(3);
        display.setCursor(0, 12);
        display.println(String(freqHz, 4));
        display.setTextSize(2);
        display.setCursor(0, 42);
        display.println("VERY HIGH!");
      }
      // === warnings ===
      display.display();
      millisDispStatusBarLast = millis();
    }
    // ============= chart and status bar start ================//

    //===========button==========================================//
    if (!digitalRead(BUTTON_MODE_PIN)) {
      delay(100);
      if (!digitalRead(BUTTON_MODE_PIN)) {
        while (!digitalRead(BUTTON_MODE_PIN)) {
          display.clearDisplay();
          display.setTextSize(2);
          display.setTextColor(WHITE);
          display.setCursor(0, 20);
          display.println(SHOW_CHART ? "Chart: OFF" : "Chart: ON");
          display.display();
          digitalWrite(BLUE_LED_PIN, LOW); delay(65);
          digitalWrite(BLUE_LED_PIN, HIGH); delay(65);
        }
        for (int i = 0; i < 16; i++) {
          digitalWrite(BLUE_LED_PIN, LOW); delay(65);
          digitalWrite(BLUE_LED_PIN, HIGH); delay(65);
        }
        SHOW_CHART = !SHOW_CHART;
        lastActivityMls = millis();
      }
    }
    //===========button==========================================//
  #endif
  // ============================= display ===================================//


  // ========================== ws send  ==============================//
  #ifdef FEED_FREQUNCY_VIA_WS
    if (WiFi.status() == WL_CONNECTED) {
      webSocketPWRCTRL.loop();
    }
    static int32_t wsSendLast = 0;
    if (millis() > wsSendLast + 2000) {
      wsSendLast = millis();
      // send to power control device (optional)
      String sendStr = "EXT_F:" + String(freqHz, 4);
      webSocketPWRCTRL.sendTXT(sendStr); // send freq to power ctrl server
    }
  #endif
  // ========================== ws send  ==============================//

  //////==========================ThingSpeakStart==============================///
  static uint32_t prevMilliThingSpeak = 0;
  if ((millis() - prevMilliThingSpeak >= 16000) && SendDataToServer && freqHz > 40 && freqHz < 65 && millis() > 60000 && WiFi.status() == WL_CONNECTED)
  {
    // save the last time was report to thingSpeak
    prevMilliThingSpeak = millis();
    Serial.println("==================================");
    Serial.print("Sending to thingSpeak ");
    Serial.println(String(freqHz, 4) + "Hz");

    String body = "";
    body += "field1=";
    body += String(freqHz, 4);

    int httpCode = ThingSpeakHttpReq(writeAPIKey, body);
    if (httpCode > 0) thingSpeakError = false;
    else thingSpeakError = true;
  }
  //////==========================ThingSpeakEnd==============================///


  ///==================================================================================================================
} // end of void loop
////=================================================================================================================


//===============================jsonFeedfun================================//
String jsonFeedGet() {
  StaticJsonDocument<2048> root;
  root["freqHz"] = freqHz;

  root["isr_time_us"] = isr_time;

  root["recordHighestHz"] = recordHighestHz;
  root["recordLowerstHz"] = recordLowerstHz;

  for (int i = 0; i < sizeof(freqHzCalcArray) / sizeof(float); i++) {
    root["freqHzCalcArray"]["raw"][i] = freqHzCalcArray[i];
  }

  for (int i = 0; i < sizeof(historyFreqHzArray) / sizeof(float); i++) {
    root["historyFreqHzArray"]["val"][i] = historyFreqHzArray[i];
    root["historyFreqHzArray"]["change"][i] = historyFreqHzArray_chng[i];
  }

  root["ESP_debug_int"]["false_int"] = false_int;
  root["ESP_debug_int"]["total_int"] = total_int;

  if(false_int){
  root["ESP_debug_int"]["falseInt_us"] = falseInt_us;
  root["ESP_debug_int"]["falseIntTime"] = falseIntTime; // last interupt false time
  root["ESP_debug_int"]["false_int_pct"] = String(float(false_int) / float(total_int) * float(100.0), 4);
  }
  

  root["ESP"]["Conn"]["thingSpeakErr"] =  thingSpeakError;
  time_t TimeNow = time(nullptr);
  root["ESP"]["Time"]["Stamp"] = String(TimeNow);
  root["ESP"]["Time"]["Now"] = ctime(&TimeNow);
  root["ESP"]["Time"]["Mls"] = millis();
  root["ESP"]["Time"]["Mcs"] = micros() ;
  root["ESP"]["Heap"] = ESP.getFreeHeap();
  root["ESP"]["RSSI"] = WiFi.RSSI();
  root["ESP"]["Connected_to"] = WiFi.BSSIDstr();
  root["ESP"]["CoreVer"] = ESP.getCoreVersion();
  root["ESP"]["SdkVer"] = ESP.getSdkVersion();
  root["ESP"]["CpuFreq"] = ESP.getCpuFreqMHz();
  root["ESP"]["ResetReason"] = ESP.getResetReason();
  root["ESP"]["SendData"] =  SendDataToServer;

  #ifdef FEED_FREQUNCY_VIA_WS
    root["ESP"]["isConnectedToPowerCtrl"] =  isConnectedToPowerCtrl;
  #endif
  
  String output;
  serializeJson(root, output);
  return output;
}
//===============================jsonFeedfun================================//



  
//======================websocketClientPowerCtrl====================================//
#ifdef FEED_FREQUNCY_VIA_WS
  void webSocketEventPowerCtrl(WStype_t type, uint8_t * payload, size_t length) {
    switch (type) {
      case WStype_DISCONNECTED:
        isConnectedToPowerCtrl = false;
        Serial.println("");
        Serial.println("===============================");
        Serial.println("[WSc] Disconnected from Power Control!");
        break;
      case WStype_CONNECTED: {
          isConnectedToPowerCtrl = true;
          Serial.println("===============================");
          Serial.println("[WSc] Connected to Power Control ...");
          Serial.println("Waiting for commands from Power Control! ...");
        }
        break;
      case WStype_TEXT: {
          Serial.println("===============================");
          Serial.println("[WSc] got payload text:");
          Serial.printf("%s\n", payload);
        }
        break;
    }
  }
#endif
//======================websocketClientPowerctrl===================================//


//==================================ThingSpeak============================================================//
int ThingSpeakHttpReq(String myApiKey, String body)
{
  if (body.isEmpty()) return -1;
  Serial.println("-----ThingSpeakHttpReq-----");
  HTTPClient http;
  WiFiClient client;
  http.setTimeout(1200);

  http.begin(client, "http://api.thingspeak.com/update");
  http.addHeader("X-THINGSPEAKAPIKEY", myApiKey);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  // if starts with "&" then remove it
  if (body.startsWith("&")) {
    body.remove(0, 1);
  }

  int httpCode = http.POST(body);

  Serial.print("Thingspeak POST with Key: ");
  Serial.println(myApiKey);
  
  if (httpCode > 0) {
    Serial.print("Thingspeak POST success! HTTP Code: ");
    Serial.println(httpCode);
  } else {
    Serial.print("POST failed, HTTP Code: ");
    Serial.print(httpCode);
    Serial.print(" | error: ");
    Serial.println(http.errorToString(httpCode));
  }

  http.end();
  return httpCode;
}
//==================================ThingSpeak============================================================//



//----------------------------------------DISAPLAY--------------------------------------------------------//
// all below is for the display
#ifdef USE_OLED_DISPLAY
  //==================== pre charge ======================================//
  void night(bool In) {
    if (In)
    {
      display.ssd1306_command(0xD9);
      display.ssd1306_command(0x08);
    }
    else
    {
      display.ssd1306_command(0xD9);
      display.ssd1306_command(0x22);
    }
  }//==================== pre charge =====================================//

  //======================================================================//
  void newChartValue(float input)
  {
    static float _circularBuffer[SCREEN_WIDTH]; //fast way to store values
    static int _curWriteIndex = 0; // tracks where we are in the circular buffer

    // Clear the display on each frame. We draw from the _circularBuffer
    display.clearDisplay();
    _circularBuffer[_curWriteIndex++] = input;

    // Set the circular buffer index back to zero when it reaches the
    // right of the screen
    if (_curWriteIndex >= display.width()) {
      _curWriteIndex = 0;
    }

    // Draw the line graph based on data in _circularBuffer
    int xPos = 0;
    for (int i = _curWriteIndex; i < display.width(); i++) {
      float Val = _circularBuffer[i];
      drawLineV(xPos, Val);
      xPos++;
    }
    for (int i = 0; i < _curWriteIndex; i++) {
      float Val = _circularBuffer[i];
      drawLineV(xPos, Val);
      xPos++;
    }
  }
  //======================================================================//

  //======================================================================//
  void drawLineV(int xPos, float inputVal)
  {
    int lineHeight = map(int(inputVal * 10000.0), int(MIN_INPUT * 10000.0), int(MAX_INPUT * 10000.0), 0, SCREEN_HEIGHT);
    int yPos = display.height() - lineHeight;

    if (CHART == "FILL") display.drawFastVLine(xPos, yPos, lineHeight, SSD1306_WHITE);
    if (CHART == "FILL_C" && inputVal != 0) display.drawLine(xPos, display.height() / 2, xPos, yPos, SSD1306_WHITE);
    if (CHART == "BAR") display.drawFastVLine(xPos, yPos - CHART_BAR_PX / 2, CHART_BAR_PX, SSD1306_WHITE);
  
    if (CHART == "LINE")
    {
      static int yPosPrev = display.height() / 2;
      if (xPos > 0 && inputVal != 0) {
        for (int i = 0; i < CHART_LINE_PX; i++) {
          display.drawLine(xPos - 1, yPosPrev - i + CHART_LINE_PX / 2, xPos, yPos - i + CHART_LINE_PX / 2, SSD1306_WHITE);
        }
      }
      yPosPrev = yPos;
      if (inputVal == 0) yPosPrev = display.height() / 2;
    }
  }
  //======================================================================//

  //======================================================================//
  void drawStatusBar(float inVal) {
    // save oled, helps with "burn-in"?
    static uint32_t timeToChange = 0;
    static int text_move = 4;
    if (text_move < 0) text_move = 4;
    if (!SAVE_OLED) text_move = 0; // if off

    // erase status bar by drawing all black
    if (STATUS_BAR_SMALL) {
      display.fillRect(0, 0, (display.width() / 2) - 21 + text_move, 9, SSD1306_BLACK);
      display.setTextSize(1);
      display.setCursor(text_move, 0);
      display.print(String(inVal, 4));
    }
    else
    {
      display.fillRect(0, 0, (display.width() / 2) + 8 + text_move, 16, SSD1306_BLACK);
      display.setTextSize(2);
      display.setCursor(text_move, 0);
      display.print(String(inVal, 3));
    }

    if (millis() > timeToChange + 30000) { // oled save
      text_move--;
      timeToChange = millis();
    }
  }
  //======================================================================//

  //======================================================================//
  void drawDottedLines() {
    static int dots_move = 4;
    if (dots_move < 0) dots_move = 4;
    for (int i = dots_move; i < display.width(); i = i + 5) {
      display.drawPixel(i, display.height() - 1, SSD1306_WHITE); // drow dotted line at the bottom of the screen
      display.drawPixel(i, display.height() / 2, SSD1306_WHITE); // in the middle
      display.drawPixel(i, 0, SSD1306_WHITE); // upper dotted line
    }
    dots_move--;
  }
  //======================================================================//
#endif
//----------------------------------------DISAPLAY--------------------------------------------------------//
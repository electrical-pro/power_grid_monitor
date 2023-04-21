//NODEMCU ESP8266 tested with core 3.0.2 and 2.7.4
//Core 2.7.4 is more stable.
//Serhii Bazavluk @ Electrical Projects [CreativeLab]
//https://www.youtube.com/c/ElectricalPro/videos
#include <Arduino.h>
#include <ArduinoOTA.h> // for OTA
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <FS.h>   // Include the SPIFFS library
#include <ArduinoJson.h> // json 6.17.2
#include <StreamString.h>
#include <Hash.h>
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Adafruit_SSD1306.h>  // 2.5.0
#include <Adafruit_GFX.h>

WiFiManager wm;

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
#define DISPLAY_BRIGHTNESS 255 // 0-255 range (255 max brightness)  //try 130
bool SHOW_CHART = true; // Show chart or show big frequency value (flash button changes it, try pressing it)

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define intrInputPIN  D5 //interupt pin
#define blink_low_freq 49.91 //blink blue led when frequency is lover than XX Hz
#define telegram_on_freq 49.90 //send telegram message when frequency is lover than XX Hz
#define telegram_on_change 0.04 //send telegram message on fast frequncy change (difference between last two measurments)

#define buttonPin 0 // pin for the flash button

float freqHz=0;

uint32_t intrptArray[50]; // intturupt micros array, stores time when interrupts happened (store on every new cycle)
float freqHzCalcArray[50]; // array to store last XX freq calculations (store on every new cycle)

// just to send something to telegram (can be removed)
float freqHzArray[16]; // array to store last 16 frequencies
float freqHzArray_chng[16]; // store 16 changes in frequncy

float recordLowerstHz = 0;
float recordHighestHz = 0;

uint32_t lastInterTime = 0;
uint32_t falseIntTimeUs = 0;

uint32_t lastUsedMls = 0;

const uint8_t indicPinBLUE = 2;  // pin for indication (led blinking)
bool WifiOnlineStateFlag = false; //  false - offline, true - online
uint32_t prevMillTelgmUpdt_on_freq = 0; // telegram last time
uint32_t prevMilliTelgmUpdt_on_change = 0;
uint32_t wifiApCheckMillLast = 0; // for wifi in offline mode

uint32_t millisDispLast = 0; // display update

// debug statistic
uint32_t false_int;
uint32_t total_int;

WiFiClient client;
WiFiClientSecure clientSec;
ESP8266WebServer server(8089);

//=========ThingSpeak
String writeAPIKey = "=MyApiKeyWrite="; // write API key for your ThingSpeak Channel
bool SendDataToServer = false;   //  set to false to NOT send data to ThingSpeak (plese set to true if you use ThingSpeak)
bool thingSpeakError = true; // stores result of the last try
const uint32_t ThingSpkPostInt = 16000; // set ms intervels between posts to ThingSpeak
uint32_t prevMilliThingSpeak = 0;


void ICACHE_RAM_ATTR handleInterrupt()  //This is the function called by the interrupt
{
  // measures frequency in range 40Hz - 66.6Hz
  uint32_t micros_int = micros();
  // filter low limit 25ms = 40 Hz
  if (micros_int - lastInterTime > 25000)
  {
    // something really wrong, interupt not detected in the right time, (too late), restart ESP
    if (millis() > 5000) {
      ESP.restart();
    }
  }

  // filter high 15ms = 66.6Hz
  // helps to ignore false interrupts that may happen
  if (micros_int - lastInterTime > 15000)
  {
    lastInterTime = micros_int;
    uint32_t sizeOfmyArray = sizeof(intrptArray);
    // array to store interrupts time
    memcpy(&intrptArray[0], &intrptArray[1], sizeOfmyArray - sizeof(uint32_t)); // shift array
    intrptArray[sizeOfmyArray / sizeof(uint32_t) - 1] =  micros_int; // add last value
    //Calculate frequency
    float cycleTimeMs = float(intrptArray[sizeOfmyArray / sizeof(uint32_t) - 1] - intrptArray[0]) / float((sizeOfmyArray / sizeof(uint32_t) - 1) * 1000);
    float freqHzSingleMeasure = float(1000) / float(cycleTimeMs); // frequency

    sizeOfmyArray = sizeof(freqHzCalcArray);
    // array to store freq calculations, updates on every cycle
    memcpy(&freqHzCalcArray[0], &freqHzCalcArray[1], sizeOfmyArray - sizeof(float)); // shift array
    freqHzCalcArray[sizeOfmyArray / sizeof(float) - 1] =  freqHzSingleMeasure; // add last value

    // calculate avrage
    float FreqAvrCalc = 0;
    for (int i = 0; i < sizeof(freqHzCalcArray) / sizeof(float); i++) {
      FreqAvrCalc = FreqAvrCalc + freqHzCalcArray[i]; //add all
    }
    freqHz = FreqAvrCalc / (sizeof(freqHzCalcArray) / sizeof(float));  //avrage (divide)
  
    // save records
    if (millis() > 5000 && freqHz > recordHighestHz){ recordHighestHz = freqHz;}    
    if (millis() > 5000 && (recordLowerstHz == 0 || freqHz < recordLowerstHz)){ recordLowerstHz = freqHz;}
    // ISR above takes about 62us at 80MHz speed or 31us at 160MHz, so I think we are fine :)
  }
  else
  {
    // false interrupt, too soon
    false_int++;
    falseIntTimeUs = micros_int - lastInterTime; // for debug
  }
  total_int++;
}


void setup()
{
  Serial.begin(115200);
  pinMode(intrInputPIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(intrInputPIN), handleInterrupt, FALLING); // RISING or FALLING

  pinMode(buttonPin, INPUT_PULLUP); // flash button to change display mode

  pinMode(indicPinBLUE, OUTPUT); // indication works as an output
  digitalWrite(indicPinBLUE, LOW); // Indication pin set for low.

  ArduinoOTA.onEnd([]() { // do a fancy thing with our board led at end
    for (int i = 0; i < 30; i++)
    {
      analogWrite(indicPinBLUE, (i * 100) % 1001);
      delay(25);
    }
    // restart after ota
    ESP.restart();
  });

  if (!SPIFFS.begin()) {
    Serial.println(F("SPIFFS: Failed to mount file system!"));
  }
  else
  {
    Serial.println(F("SPIFFS: Ok, File system mouted!"));
  }

  //SSD1306_SWITCHCAPVCC = generate display voltage from 3.3V internally
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
  
  WiFi.hostname("FreqMeterESP");
  delay(500);
  WiFi.mode(WIFI_STA); // STA mode.
  delay(500);
  Serial.println("===============================");
  Serial.println(F("WIFI_STA mode"));

  wm.setConfigPortalBlocking(false);

  //automatically connect using saved credentials if they exist
  //If connection fails it starts an access point with the specified name
  if (wm.autoConnect("FreqMeterESP | Offline", "231019787511")) {
    Serial.println("connected...yeey :)");
    WifiOnlineStateFlag = true; // wifi in online state

    // Indicate the start of connectrion to AP
    digitalWrite(indicPinBLUE, HIGH);
    delay(600);
    digitalWrite(indicPinBLUE, LOW);
    delay(175);
    digitalWrite(indicPinBLUE, HIGH);
    delay(175);
    digitalWrite(indicPinBLUE, LOW);
    delay(175);
    digitalWrite(indicPinBLUE, HIGH);
    //
  }
  else {
    Serial.println("Configportal running");
    WifiOnlineStateFlag = false;
    Serial.println("===============================");
    Serial.println(F("OFFLINE MODE !!!"));
  }

  // time zone for time (not necessary)
  configTime("EET-2EEST,M3.5.0/3,M10.5.0/4", "pool.ntp.org");



  // =========== over the air updates ==============
  ArduinoOTA.setHostname("FreqMeterESP");
  ArduinoOTA.onStart([]() {
    detachInterrupt(digitalPinToInterrupt(intrInputPIN)); // detach. Seems like interrupts interfere with update?
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
  server.onNotFound([]() {                              // If the client requests any URI
    server.sendHeader("access-control-allow-origin", "*");
    if (SPIFFS.exists(server.uri()))
    {
      String ContentType = "text/plain";
      if (server.uri().endsWith(".htm")) {
        ContentType = "text/html";
        server.sendHeader("Content-Encoding", "gzip");  // gziped header if html
      }
      else if (server.uri().endsWith(".html")) {
        ContentType = "text/html";
        server.sendHeader("Content-Encoding", "gzip");  // gziped header if html
      }
      else if (server.uri().endsWith(".css"))  ContentType = "text/css";
      else if (server.uri().endsWith(".js"))   ContentType = "application/javascript";
      else if (server.uri().endsWith(".png"))  ContentType = "image/png";
      else if (server.uri().endsWith(".gif"))  ContentType = "image/gif";
      else if (server.uri().endsWith(".jpg"))  ContentType = "image/jpeg";
      else if (server.uri().endsWith(".ico"))  ContentType = "image/x-icon";
      else if (server.uri().endsWith(".xml"))  ContentType = "text/xml";
      else if (server.uri().endsWith(".pdf"))  ContentType = "application/x-pdf";
      else if (server.uri().endsWith(".zip"))  ContentType = "application/x-zip";
      else if (server.uri().endsWith(".gz"))   ContentType = "application/x-gzip";


      File file = SPIFFS.open(server.uri(), "r");     // Open it
      server.streamFile(file, ContentType);    // And send it to the client
      file.close();   // Then close the file again
    }
    else
    {
      server.send(404, "text/plain", "404: Not Found"); // otherwise, respond with a 404 (Not Found) error
    }
  });
  //========================FileServer======================================================//

  // =================== on index ===//
  server.on("/", []() {
    server.sendHeader("access-control-allow-origin", "*");
      String page = "<head>\r\n<title>Frequency Meter</title>\r\n</head><meta name=\"theme-color\"content=\"#042B3C\"><meta http-equiv=\"refresh\" content=\"60\">";
      page +="<style>body{background-color:#000;font-family:Arial;Color:#fff}";
      page +="a:link{color:#03fc9c;background-color:transparent;text-decoration:none;}a:visited{color:#03fc9c;background-color:transparent;text-decoration:none;}";
      page +="a:hover{color:red;background-color:transparent;text-decoration:underline;}a:active{color:red;background-color:transparent;text-decoration:underline;}";
      page +="hr{max-width:380px;margin-left:0;border:0;border-top:1px solid #00a3a3;padding:0}</style>";
      page +="<h2>Hello, I\'m Frequency Meter!</h2>\r\n<hr>Frequency now: <b>";
      page += String(freqHz, 4);
      page +=" Hz</b>";
      page +="<hr>Highest detected: <b>";
      page += String(recordHighestHz, 4);
      page +=" Hz</b>";
      page += "<br>\r\nLowest detected: <b>";
      page += String(recordLowerstHz, 4);
      page +=" Hz</b>";
      page +="<br>\r\n<hr>";
      page +="\r\n> <a href=\"/Frequency_Live_150ms.html\">Chart 150 ms</a><br>\r\n> <a href=\"/Frequency_Live_500ms.html\">Chart 500 ms</a><br>";
      page +="\r\n> <a href=\"/Frequency_Live_1s.html\">Chart 1 s</a><br>\r\n> <a href=\"/Frequency_Live_4s.html\">Chart 4 s</a><br>\r\n> <a href=\"/Feed_JSON\">JSON feed</a>";
      page +="<br>\r\n<hr>\r\n> <a href=\"https://www.youtube.com/c/ElectricalPro/videos\">More information available on YouTube channel</a>\r\n<hr>\r\n";
    server.send(200, "text/html", page);
  });
  // =================== index ===//


  // =================== JSON Feed ===//
  server.on("/Feed_JSON", []() {
    server.sendHeader("access-control-allow-origin", "*");
    server.send(200, "application/json", jsonFeedGet());
  });
  // =================== JSON Feed ===//

  server.begin();
  Serial.println(F("HTTP server started"));

  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(DISPLAY_BRIGHTNESS); //0-255

}// end of setup loop


void loop()
{
  wm.process();
  ArduinoOTA.handle(); // for OTA
  server.handleClient();

    
  //===========button==========================================//
  if (!digitalRead(buttonPin))
  {
    delay(100);
    if (!digitalRead(buttonPin))
    {
      while (!digitalRead(buttonPin)) {
        display.clearDisplay();
        display.setTextSize(2);             // Normal 1:1 pixel scale
        display.setTextColor(WHITE);        // Draw white text
        display.setCursor(0, 20);
        if (SHOW_CHART)
        {
          display.println("Chart: OFF");
        }
        else
        {
          display.println("Chart: ON");
        }
        display.display();

        digitalWrite(indicPinBLUE, LOW);
        delay(65);
        digitalWrite(indicPinBLUE, HIGH);
        delay(65);
      }
      for(int i=0; i<16; i++){
        digitalWrite(indicPinBLUE, LOW);
        delay(65);
        digitalWrite(indicPinBLUE, HIGH);
        delay(65);
      }
      SHOW_CHART = !SHOW_CHART;
      lastUsedMls = millis(); // for brightness
    }
  }
  //===========button==========================================//



  // ============= chart and status bar start ================//
  if (millis() > millisDispLast + 2000) {
    // drow chart (I2C display)
    newChartValue(freqHz);
    drawDottedLines();
    drawStatusBar(freqHz);

    //also here let's save value to array (not nessesery), we can later send this to telegram for example
    memcpy(&freqHzArray[0], &freqHzArray[1], sizeof(freqHzArray) - sizeof(float)); // shift array
    memcpy(&freqHzArray_chng[0], &freqHzArray_chng[1], sizeof(freqHzArray_chng) - sizeof(float)); // shift array
    freqHzArray[sizeof(freqHzArray) / sizeof(float) - 1] =  freqHz; // add last value
    freqHzArray_chng[sizeof(freqHzArray_chng) / sizeof(float) - 1] = freqHzArray[sizeof(freqHzArray) / sizeof(float) - 1] - freqHzArray[sizeof(freqHzArray) / sizeof(float) - 2]; // calculate change and add

    millisDispLast = millis();
  }

  // === status bar and other staff ===
  static uint32_t millisDispStatusBarLast = 0; // status bar and other stuff
  if (millis() > millisDispStatusBarLast + 100) {
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

    if (SHOW_CHART && !(freqHz > MAX_INPUT || freqHz < MIN_INPUT))
    {
      drawStatusBar(freqHz);
    }

    // change brightness on warnings (only if AUTO_DIM_DISPLAY is set to true)
    if (freqHz > MAX_INPUT || freqHz < MIN_INPUT)
    {
      lastUsedMls = millis(); // for brightness
    }
    if (millis() - lastUsedMls < 60000)
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


  //=============================WIFI-HANDDLE=======================================//
  // in offline mode check for AP peridiodicly, if available then reboot.
  if (WifiOnlineStateFlag == false && millis() - wifiApCheckMillLast > 120000)
  {
    wifiApCheckMillLast = millis();
    Serial.println("===============================");
    Serial.print("Checking if "); // if AP available
    Serial.print(WiFi.SSID());
    Serial.println(" available");

    int n = WiFi.scanNetworks(); // n of networks
    bool foundCorrectSSID = false;
    Serial.println("Found " + String(n) + " networks");
    for (int x = 0; x != n; x++)
    {
      Serial.println(WiFi.SSID(x));
      if (WiFi.SSID(x) == WiFi.SSID())
      {
        foundCorrectSSID = true;
      }
    }
    Serial.println("----");
    if (foundCorrectSSID)
    {
      Serial.println("Found " + String(WiFi.SSID()));
      Serial.println("Restarting ESP!!!");
      delay(1000);
      ESP.restart();
    }
    else
    {
      Serial.println("Not Found " + String(WiFi.SSID()));
    }
  }


  // Lost connection with wi-fi
  if ((WiFi.status() != WL_CONNECTED) && (WifiOnlineStateFlag == true)) {
    Serial.println("===============================");
    Serial.println(F("Lost connection to wi-fi"));
    // if wi-fi is not reconnected within 60 second then reboot ESP
    for (int i = 60; i >= 0; i--) {

      Serial.print("Restarting in: ");
      Serial.print(i);
      Serial.println(" sec.");

      delay(750);


      if (WiFi.status() == WL_CONNECTED) {
        i = -1;
        Serial.println(F("Reconnected to wi-fi"));
      }
    }
    if ((WiFi.status() != WL_CONNECTED) && (WifiOnlineStateFlag == true)) {
      ESP.restart();
    }
  }
  ///==========AP State==========//
  //=============================WIFI-HANDDLE=======================================//


  // ============= blink led on freq =================//
  if (freqHz < blink_low_freq) {
    digitalWrite(indicPinBLUE, millis() >> 8 & 1);
  }
  else
  {
    digitalWrite(indicPinBLUE, HIGH);
  }
  // ============= blink led on freq =================//


  //======= telegram send message ======//
  // on low frequency
  if (millis() - prevMillTelgmUpdt_on_freq >= 120000 && freqHz < telegram_on_freq && millis() > 45000 && WiFi.status() == WL_CONNECTED) {
    prevMillTelgmUpdt_on_freq = millis();
    telegramPrivateMsg("Device at: " + WiFi.localIP().toString() + "\nWarning!\nLow Frequency in power grid: " + String(freqHz, 4) + " Hz" + LastValues());
  }
  // on fast change of frequency
  if (millis() - prevMilliTelgmUpdt_on_change >= 4100 && fabs(freqHzArray_chng[sizeof(freqHzArray_chng) / sizeof(float) - 1]) > telegram_on_change && millis() > 45000) {
    telegramPrivateMsg("Device at: " + WiFi.localIP().toString() + "\nWarning!\nFast Frequency Change in power grid: " + String(freqHz, 4) + " Hz" + LastValues());
    prevMilliTelgmUpdt_on_change = millis();
  }
  //=============


  //////==========================ThingSpeakStart==============================///
  if (millis() - prevMilliThingSpeak >= ThingSpkPostInt && SendDataToServer == true && freqHz > 40 && freqHz < 65 && millis() > 60000 && WiFi.status() == WL_CONNECTED)
  {
    // save the last time was report to thingSpeak
    prevMilliThingSpeak = millis();
    Serial.println("==================================");
    Serial.print("Sending to thingSpeak ");
    Serial.println(String(freqHz, 4) + "Hz");
    // thingSpeak
    HTTPClient http;
    //http.setTimeout(3000);

    http.begin(client, "http://api.thingspeak.com/update");
    http.addHeader("X-THINGSPEAKAPIKEY", writeAPIKey);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    // Construct API request body
    String body = "";
    body += "field1=";
    body += String(freqHz, 4);

    int httpCode = http.POST(body);
    http.end();

    delay(5);

    Serial.print("HTTP Code: ");
    Serial.println(httpCode);
    if (httpCode > 0)
    {
      //everything good
      thingSpeakError = false;
    }
    else
    {
      // Error
      thingSpeakError = true;
    }
  }
  //////==========================ThingSpeakEnd==============================///


  ///==================================================================================================================
} // end of void loop
////=================================================================================================================


//===============================jsonFeedfun================================//
String jsonFeedGet() {
  StaticJsonDocument<2048> root;
  root["freqHz"] = freqHz;

  root["recordHighestHz"] = recordHighestHz;
  root["recordLowerstHz"] = recordLowerstHz;

  //root["freqHz_change"] = freqHzArray[sizeof(freqHzArray)/sizeof(float)-1]-freqHzArray[sizeof(freqHzArray)/sizeof(float)-2];

  for (int i = 0; i < sizeof(freqHzArray) / sizeof(float); i++) {
    root["freqHzArray"]["val"][i] = freqHzArray[i];
    root["freqHzArray"]["change"][i] = freqHzArray_chng[i];
  }

  root["ESP_debug_int"]["false_int"] = false_int;
  root["ESP_debug_int"]["total_int"] = total_int;
  root["ESP_debug_int"]["falseIntTimeUs"] = falseIntTimeUs; // last interupt false time
  root["ESP_debug_int"]["false_int_pct"] = String(float(false_int) / float(total_int) * float(100.0), 4);

  root["ESP"]["Conn"]["thingSpeakErr"] =  thingSpeakError;
  time_t TimeNow = time(nullptr);
  root["ESP"]["Time"]["Stamp"] = String(TimeNow);
  root["ESP"]["Time"]["Now"] = ctime(&TimeNow);
  root["ESP"]["Time"]["Mls"] = millis();
  root["ESP"]["Time"]["Mcs"] = micros() ;
  root["ESP"]["Heap"] = ESP.getFreeHeap();
  root["ESP"]["RSSI"] = WiFi.RSSI();
  root["ESP"]["CoreVer"] = ESP.getCoreVersion();
  root["ESP"]["SdkVer"] = ESP.getSdkVersion();
  root["ESP"]["CpuFreq"] = ESP.getCpuFreqMHz();
  root["ESP"]["ResetReason"] = ESP.getResetReason();
  root["ESP"]["SendData"] =  SendDataToServer;

  String output;
  serializeJson(root, output);
  return output;
}
//===============================jsonFeedfun================================//



//===============================Telegram_Message======================//
void telegramPrivateMsg(String msg) {
//  if (WiFi.status() == WL_CONNECTED) {
//    digitalWrite(indicPinBLUE, LOW);
//    Serial.println("\nStarting connection to Telegram server...");
//    clientSec.setInsecure();//skip verification
//    if (!clientSec.connect("https://api.telegram.org", 443))
//      Serial.println("Connection failed!");
//    else {
//      Serial.println("Connected to server! Sending info...");
//      // Make a HTTP request:
//      String sendDataMsg = "GET https://api.telegram.org/botXXXXXXXX:XXXXX-XXXXXXXXXXXXXXXXXXX/sendMessage?chat_id=XXXXXXXXX&text="; // update for your telegram bot
//      sendDataMsg = sendDataMsg + urlencode(msg);
//      clientSec.println(sendDataMsg);
//      Serial.println(sendDataMsg);
//      clientSec.println("Host: api.telegram.org");
//      clientSec.println("Connection: close");
//      clientSec.println();
//    }
//    clientSec.stop();
//    digitalWrite(indicPinBLUE, HIGH);
//  }
}
//===============================Telegram_Message======================//

//===============================URL_encodeFun========================//
String urlencode(String str)
{
  String encodedString = "";
  char c;
  char code0;
  char code1;
  char code2;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encodedString += '+';
    } else if (isalnum(c)) {
      encodedString += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) {
        code1 = (c & 0xf) - 10 + 'A';
      }
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) {
        code0 = c - 10 + 'A';
      }
      code2 = '\0';
      encodedString += '%';
      encodedString += code0;
      encodedString += code1;
      //encodedString+=code2;
    }
    yield();
  }
  return encodedString;
}
//===============================URL_encodeFun========================//

//===============================last values get =======================//
String LastValues()
{
  String telTextAdd = "\nLast " + String(sizeof(freqHzArray) / sizeof(float)) + " values:\n"; // text with array values
  for (int i = 0; i < sizeof(freqHzArray) / sizeof(float); i++) {
    telTextAdd += String(freqHzArray[i], 4);
    telTextAdd += " | ";
    telTextAdd += String(freqHzArray_chng[i], 4);
    telTextAdd += "\n";
  }

  telTextAdd += "== Records ==";
  telTextAdd += "\n";
  telTextAdd += "H: ";
  telTextAdd += String(recordHighestHz, 4);
  telTextAdd += " | ";
  telTextAdd += "L: ";
  telTextAdd += String(recordLowerstHz, 4);
  telTextAdd += "\n";

  time_t TimeNow = time(nullptr);
  telTextAdd += ctime(&TimeNow);
  telTextAdd += "\n";
  return telTextAdd;
}
//===============================last values get =======================//


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




/**
   Draw chart
*/
void newChartValue(float input) {
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
}//=========================================================


/**
   Draw the line at the given x position
*/
void drawLineV(int xPos, float inputVal) {
  int lineHeight = map(int(inputVal * 10000.0), int(MIN_INPUT * 10000.0), int(MAX_INPUT * 10000.0), 0, SCREEN_HEIGHT);
  int yPos = display.height() - lineHeight;

  if (CHART == "FILL")
  {
    display.drawFastVLine(xPos, yPos, lineHeight, SSD1306_WHITE);
  }

  if (CHART == "FILL_C")
  {
    if (inputVal != 0) display.drawLine(xPos, display.height() / 2, xPos, yPos, SSD1306_WHITE);
  }

  if (CHART == "BAR")
  {
    display.drawFastVLine(xPos, yPos - CHART_BAR_PX / 2, CHART_BAR_PX, SSD1306_WHITE);
  }

  if (CHART == "LINE")
  {
    static int yPosPrev = display.height() / 2;
    if (xPos > 0 && inputVal != 0) {
      for (int i = 0; i < CHART_LINE_PX; i++) {
        display.drawLine(xPos - 1, yPosPrev - i + CHART_LINE_PX / 2, xPos, yPos - i + CHART_LINE_PX / 2, SSD1306_WHITE);
      }
    }
    yPosPrev = yPos;
    if (inputVal == 0)
      yPosPrev = display.height() / 2;
  }
}//=========================================================


/**
   Draws the status bar at top of screen
*/
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
}//=========================================================


/**
   Draws dotted lines for chart
*/
void drawDottedLines() {
  static int dots_move = 4;
  if (dots_move < 0) dots_move = 4;
  for (int i = dots_move; i < display.width(); i = i + 5) {
    display.drawPixel(i, display.height() - 1, SSD1306_WHITE); // drow dotted line at the bottom of the screen
    display.drawPixel(i, display.height() / 2, SSD1306_WHITE); // in the middle
    display.drawPixel(i, 0, SSD1306_WHITE); // upper dotted line
  }
  dots_move--;
}//=========================================================

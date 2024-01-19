/*
  xdrv_121_gpioviewer.ino - GPIOViewer for Tasmota

  SPDX-FileCopyrightText: 2024 Theo Arends, Stephan Hadinger and Charles Giguere

  SPDX-License-Identifier: GPL-3.0-only
*/

#ifdef USE_GPIO_VIEWER
/*********************************************************************************************\
 * GPIOViewer support based on work by Charles Giguere
 * 
 * Resources:
 *       GPIO Viewer: https://github.com/thelastoutpostworkshop/gpio_viewer
 * Server Sent Event: https://github.com/esp8266/Arduino/issues/7008
\*********************************************************************************************/

#define XDRV_121              121

#ifndef GV_PORT
#define GV_PORT               5557         // SSE webserver port
#endif
#ifndef GV_SAMPLING_INTERVAL
#define GV_SAMPLING_INTERVAL  100          // [GvSampling] milliseconds - Use Tasmota Scheduler (100) or Ticker (20..99,101..1000)
#endif

#define GV_KEEP_ALIVE         1000         // milliseconds - If no activity after this do a heap size event anyway

#ifndef GV_BASE_URL
#define GV_BASE_URL           "https://thelastoutpostworkshop.github.io/microcontroller_devkit/gpio_viewer/assets/"
#endif
/*
#ifdef ESP8266
#ifndef GV_BASE_URL
#undef GV_BASE_URL                         // Fix compiler warning
#define GV_BASE_URL           "https://ota.tasmota.com/tasmota/gpio_viewer/assets/"
#endif
#endif  // ESP8266
#ifdef ESP32
#ifndef GV_BASE_URL
#undef GV_BASE_URL                         // Fix compiler warning
#define GV_BASE_URL           "https://ota.tasmota.com/tasmota32/gpio_viewer/assets/"
#endif
#endif  // ESP32
*/

const char *GVRelease = "1.0.7";

#ifdef USE_UNISHOX_COMPRESSION
  #include "./html_compressed/HTTP_GV_PAGE.h"
#else
  #include "./html_uncompressed/HTTP_GV_PAGE.h"
#endif

const char HTTP_GV_EVENT[] PROGMEM =
  "HTTP/1.1 200 OK\n"
  "Content-Type: text/event-stream;\n"     // Server Sent Event protocol
  "Connection: keep-alive\n"               // Permanent connection
  "Cache-Control: no-cache\n"              // Do not store data into local cache
  "Access-Control-Allow-Origin: *\n\n";    // Enable CORS

enum GVPinTypes {
  GV_DigitalPin = 0,
  GV_PWMPin = 1,
  GV_AnalogPin = 2
};

struct {
  WiFiClient WebClient;
  ESP8266WebServer *WebServer;
  Ticker ticker;
  int lastPinStates[MAX_GPIO_PIN];
  uint32_t lastSentWithNoActivity;
  uint32_t freeHeap;
  uint32_t freePSRAM;
  uint32_t sampling;
  bool active;
  bool sse_ready;
} GV;

void GVStop(void) {
  GV.sse_ready = false;
  GV.ticker.detach();
  GV.active = false;

  GV.WebServer->stop();
  GV.WebServer = nullptr;
}

void GVBegin(void) {
  if (0 == GV.sampling) {
    GV.sampling = (GV_SAMPLING_INTERVAL < 20) ? 20 : GV_SAMPLING_INTERVAL;
  }

  GV.WebServer = new ESP8266WebServer(GV_PORT);
  // Set CORS headers for global responses
//  GV.WebServer->sendHeader(F("Access-Control-Allow-Origin"), F("*"));
//  GV.WebServer->sendHeader(F("Access-Control-Allow-Methods"), F("GET, POST, OPTIONS"));
//  GV.WebServer->sendHeader(F("Access-Control-Allow-Headers"), F("Content-Type"));
  GV.WebServer->on("/", GVHandleRoot);
  GV.WebServer->on("/release", GVHandleRelease);
  GV.WebServer->on("/free_psram", GVHandleFreePSRam);
  GV.WebServer->on("/events", GVHandleEvents);
  GV.WebServer->begin();

  GV.active = true;
}

void GVHandleRoot(void) {
  GVCloseEvent();

  char* content = ext_snprintf_malloc_P(HTTP_GV_PAGE, 
                                        SettingsTextEscaped(SET_DEVICENAME).c_str(),
                                        GV_BASE_URL,
                                        GV_PORT,
                                        WiFi.localIP().toString().c_str(),
                                        WiFi.localIP().toString().c_str(), GV_PORT,
                                        GV.sampling,
#ifdef ESP32
                                        ESP.getPsramSize() / 1024,
#else
                                        0,                                        
#endif  // ESP32
                                        ESP_getFreeSketchSpace() / 1024);
  if (content == nullptr) { return; }      // Avoid crash
  GV.WebServer->send_P(200, "text/html", content);
  free(content);
}

void GVHandleRelease(void) {
  String jsonResponse = "{\"release\":\"" + String(GVRelease) + "\"}";
  GV.WebServer->send(200, "application/json", jsonResponse);
}

void GVHandleFreePSRam(void) {
  String jsonResponse = "{\"freePSRAM\":\"";
#ifdef ESP32
  if (UsePSRAM()) {
    jsonResponse += String(ESP.getFreePsram() / 1024) + " KB\"}";
  } else
#endif
    jsonResponse += "No PSRAM\"}";
  GV.WebServer->send(200, "application/json", jsonResponse);
}

void GVHandleEvents(void) {
  GV.WebClient = GV.WebServer->client();
  GV.WebClient.setNoDelay(true);
//  GV.WebClient.setSync(true);

  GV.WebServer->setContentLength(CONTENT_LENGTH_UNKNOWN);  // The payload can go on forever
  GV.WebServer->sendContent_P(HTTP_GV_EVENT);

  GV.sse_ready = true;                                     // Ready for async updates
  if (GV.sampling != 100) {
    GV.ticker.attach_ms(GV.sampling, GVMonitorTask);       // Use Tasmota Scheduler (100) or Ticker (20..99,101..1000)
  }
  AddLog(LOG_LEVEL_DEBUG, PSTR("IOV: Connected"));
}

void GVEventDisconnected(void) {
  if (GV.sse_ready) {
    AddLog(LOG_LEVEL_DEBUG, PSTR("IOV: Disconnected"));
  }
  GV.sse_ready = false;                                    // This just stops the event to be restarted by opening root page again
  GV.ticker.detach();
}

void GVCloseEvent(void) {
  GVEventSend("{}", "close", millis());                    // Closes web page
  GVEventDisconnected();
}

//void GVEventSend(const char *message, const char *event=NULL, uint32_t id=0, uint32_t reconnect=0);
void GVEventSend(const char *message, const char *event, uint32_t id) {
  if (GV.WebClient.connected()) {
    // generateEventMessage() in AsyncEventSource.cpp
//    GV.WebClient.printf_P(PSTR("retry:1000\nid:%u\nevent:%s\ndata:%s\n\n"), id, event, message);
    GV.WebClient.printf_P(PSTR("id:%u\nevent:%s\ndata:%s\n\n"), id, event, message);
  } else {
    GVEventDisconnected();
  }
}

void GVMonitorTask(void) {
  // Monitor GPIO Values
  uint32_t originalValue;
  uint32_t pintype;
  bool hasChanges = false;

  String jsonMessage = "{";
  for (uint32_t pin = 0; pin < MAX_GPIO_PIN; pin++) {
    int currentState = 0;
/*  
    // Skip unconfigured GPIO
    uint32_t pin_type = GetPin(pin) / 32;
    if (GPIO_NONE == pin_type) {
      pintype = GV_DigitalPin;
      originalValue = 0;
//      currentState = 0;
    }
*/
#ifdef ESP32
    // Read PWM GPIO
    int pwm_resolution = ledcReadDutyResolution(pin);
    if (pwm_resolution > 0) {
      pintype = GV_PWMPin;
      originalValue = ledcRead2(pin);
      currentState = changeUIntScale(originalValue, 0, pwm_resolution, 0, 255);   // Bring back to 0..255
    }
#endif  // ESP32

#ifdef ESP8266
    // Read PWM GPIO
    int pwm_value = AnalogRead(pin);
    if (pwm_value > -1) {
      pintype = GV_PWMPin;
      originalValue = pwm_value;
      currentState = changeUIntScale(originalValue, 0, Settings->pwm_range, 0, 255);  // Bring back to 0..255
    }
#endif  // ESP8266

#ifdef USE_ADC
    else if (AdcPin(pin)) {
      // Read Analog (ADC) GPIO
      pintype = GV_AnalogPin;
/*
#ifdef ESP32
      originalValue = AdcRead(pin, 2);
#endif  // ESP32
#ifdef ESP8266
      // Fix exception 9 if using ticker - GV.sampling != 100 caused by delay(1) in AdcRead() (CallChain: (phy)pm_wakeup_init, (adc)test_tout, ets_timer_arm_new, delay, AdcRead, String6concat, MonitorTask)
      originalValue = (GV.sampling != 100) ? analogRead(pin) : AdcRead(pin, 1);
#endif  // ESP8266
*/
      originalValue = AdcRead1(pin);
      currentState = changeUIntScale(originalValue, 0, AdcRange(), 0, 255);   // Bring back to 0..255
    }
#endif  // USE_ADC

    else {
      // Read digital GPIO
      pintype = GV_DigitalPin;
      int value = digitalRead(pin);
      originalValue = value;
      if (value == 1) {
        currentState = 256;
//      } else {
//        currentState = 0;
      }
    }

    if (originalValue != GV.lastPinStates[pin]) { 
      if (hasChanges) { jsonMessage += ","; }
      jsonMessage += "\"" + String(pin) + "\":{\"s\":" + currentState + ",\"v\":" + originalValue + ",\"t\":" + pintype + "}";
      GV.lastPinStates[pin] = originalValue;
      hasChanges = true;
    }
  }
  jsonMessage += "}";
  if (hasChanges) {
    GVEventSend(jsonMessage.c_str(), "gpio-state", millis());
  }

  uint32_t heap = ESP_getFreeHeap();
  if (heap != GV.freeHeap) {
    // Send freeHeap
    GV.freeHeap = heap;
    char temp[20];
    snprintf_P(temp, sizeof(temp), PSTR("%d KB"), heap / 1024);
    GVEventSend(temp, "free_heap", millis());
    hasChanges = true;
  }

#ifdef ESP32
  if (UsePSRAM()) {
    // Send freePsram
    uint32_t psram = ESP.getFreePsram();
    if (psram != GV.freePSRAM) {
      GV.freePSRAM = psram;
      char temp[20];
      snprintf_P(temp, sizeof(temp), PSTR("%d KB"), psram / 1024);
      GVEventSend(temp, "free_psram", millis());
      hasChanges = true;
    }
  }
#endif  // ESP32

  if (!hasChanges) {
    // Send freeHeap as keepAlive
    uint32_t last_sent = millis() - GV.lastSentWithNoActivity;
    if (last_sent > GV_KEEP_ALIVE) {
      // No activity, resending for pulse
      char temp[20];
      snprintf_P(temp, sizeof(temp), PSTR("%d KB"), heap / 1024);
      GVEventSend(temp, "free_heap", millis());
      GV.lastSentWithNoActivity = millis();
    }
  } else {
    GV.lastSentWithNoActivity = millis();
  }
}

/*********************************************************************************************\
 * Commands
\*********************************************************************************************/

const char kGVCommands[] PROGMEM = "GV|"   // Prefix
  "Viewer|Sampling";

void (* const GVCommand[])(void) PROGMEM = {
  &CmndGvViewer, &CmndGvSampling };

void CmndGvViewer(void) {
  /* GvViewer    - Show current viewer state
     GvViewer 0  - Turn viewer off
     GvViewer 1  - Turn viewer On
     GvViewer 2  - Toggle viewer state
  */
  if ((XdrvMailbox.payload >= 0) && (XdrvMailbox.payload <= 2)) {
    uint32_t state = XdrvMailbox.payload;
    if (2 == state) {                      // Toggle
      state = GV.active ^1;
    }
    if (state) {                           // On
      GVBegin();
    } else {                               // Off
      GVCloseEvent();                      // Stop current updates
      GVStop();
    }
  }
  if (GV.active) {
    Response_P(PSTR("{\"%s\":\"Active on http://%s:" STR(GV_PORT) "/\"}"), XdrvMailbox.command, WiFi.localIP().toString().c_str());
  } else {
    ResponseCmndChar_P(PSTR("Stopped"));
  }
}

void CmndGvSampling(void) {
  /* GvSampling             - Show current sampling interval
     GvSampling 20 .. 1000  - Set sampling interval
  */
  if ((XdrvMailbox.payload >= 20) && (XdrvMailbox.payload <= 1000)) {
    GVCloseEvent();                        // Stop current updates
    GV.sampling = XdrvMailbox.payload;     // 20 - 1000 milliseconds
  }
  ResponseCmndNumber(GV.sampling);
}

/*********************************************************************************************\
 * GUI
\*********************************************************************************************/
#ifdef USE_WEBSERVER
#define WEB_HANDLE_GV "gv"

const char HTTP_BTN_MENU_GV[] PROGMEM =
  "<p><form action='" WEB_HANDLE_GV "' method='get' target='_blank'><button>" D_GPIO_VIEWER "</button></form></p>";

void GVSetupAndStart(void) {
  if (!HttpCheckPriviledgedAccess()) { return; }

  AddLog(LOG_LEVEL_DEBUG, PSTR(D_LOG_HTTP D_GPIO_VIEWER));

  if (!GV.active) {          // WebServer not started
    GVBegin();
  }

  char redirect[100];
  snprintf_P(redirect, sizeof(redirect), PSTR("http://%s:" STR(GV_PORT) "/"), WiFi.localIP().toString().c_str());
  Webserver->sendHeader(F("Location"), redirect);
  Webserver->send(303);
}
#endif  // USE_WEBSERVER

/*********************************************************************************************\
 * Interface
\*********************************************************************************************/

bool Xdrv121(uint32_t function) {
  bool result = false;

  switch (function) {
    case FUNC_COMMAND:
      result = DecodeCommand(kGVCommands, GVCommand);
      break;
#ifdef USE_WEBSERVER
    case FUNC_WEB_ADD_MANAGEMENT_BUTTON:
      if (XdrvMailbox.index) {
        XdrvMailbox.index++;
      } else {
        WSContentSend_PD(HTTP_BTN_MENU_GV);
      }
      break;
    case FUNC_WEB_ADD_HANDLER:
      WebServer_on(PSTR("/" WEB_HANDLE_GV), GVSetupAndStart);
      break;
#endif // USE_WEBSERVER
  }
  if (GV.active) {
    switch (function) {
      case FUNC_LOOP:
        if (GV.WebServer) { GV.WebServer->handleClient(); }
        break;
      case FUNC_EVERY_100_MSECOND:
        if (GV.sse_ready && (100 == GV.sampling)) {
          GVMonitorTask();
        }
        break;
      case FUNC_SAVE_BEFORE_RESTART:
        GVCloseEvent();                        // Stop current updates
        break;
      case FUNC_ACTIVE:
        result = true;
        break;
    }
  }
  return result;
}

#endif // USE_GPIO_VIEWER

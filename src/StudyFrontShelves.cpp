// This #include statement was automatically added by the Particle IDE.
#include <Arduino.h>
#include <neopixel.h>
#include "papertrail.h"
#include "secrets.h"
#include "mqtt.h"
#include "DiagnosticsHelperRK.h"


// Stubs
void mqttCallback(char* topic, byte* payload, unsigned int length);
uint32_t Wheel(byte WheelPos);
void RainbowCycleUpdate(Adafruit_NeoPixel *pixels, uint8_t index);

const bool isDebug = false;

const uint8_t fps = 60;
uint32_t nextRun = 0;

typedef enum {
    LIGHT_EFFECT_NONE = 0,
    LIGHT_EFFECT_RAINBOW = 1
} LightEffect;

struct ShelfData {
    uint8_t numLeds;
    bool enabled;
    bool active;
    uint8_t effect;
    uint8_t targetBrightness;
    uint8_t brightness;
    uint8_t color[3];
};

uint8_t shelfCount = 4;

ShelfData shelfData[4] = {
    { 95, false, false, LIGHT_EFFECT_NONE, 255, 0, {255, 255, 255} },
    { 95, false, false, LIGHT_EFFECT_NONE, 255, 0, {255, 255, 255} },
    { 96, false, false, LIGHT_EFFECT_NONE, 255, 0, {255, 255, 255} },
    { 82, false, false, LIGHT_EFFECT_NONE, 255, 0, {255, 255, 255} }
};

Adafruit_NeoPixel shelves[] = {
  Adafruit_NeoPixel(shelfData[0].numLeds, D1, WS2813),
  Adafruit_NeoPixel(shelfData[1].numLeds, D2, WS2813),
  Adafruit_NeoPixel(shelfData[2].numLeds, D3, WS2813),
  Adafruit_NeoPixel(shelfData[3].numLeds, D4, WS2813)
};

uint32_t resetTime = 0;
retained uint32_t lastHardResetTime;
retained int resetCount;

bool psuShouldBeEnabled = false;
bool psuEnabled = false;
bool psuReady = false;
uint32_t psuActionableTime = 0;
const uint16_t psuShutdownBuffer = 10000;
const uint16_t psuStartupBuffer = 100;

MQTT mqttClient(mqttServer, 1883, mqttCallback);
unsigned long lastMqttConnectAttempt;
const int mqttConnectAtemptTimeout = 5000;

PapertrailLogHandler papertrailHandler(papertrailAddress, papertrailPort,
  deviceName, System.deviceID(),
  LOG_LEVEL_NONE, {
  { "app", LOG_LEVEL_ALL }
  // TOO MUCH!!! { “system”, LOG_LEVEL_ALL },
  // TOO MUCH!!! { “comm”, LOG_LEVEL_ALL }
});

uint32_t nextMetricsUpdate = 0;
void sendTelegrafMetrics() {
    if (millis() > nextMetricsUpdate) {
        nextMetricsUpdate = millis() + 30000;

        char buffer[150];
        snprintf(buffer, sizeof(buffer),
            "status,device=%s uptime=%d,resetReason=%d,firmware=\"%s\",memTotal=%ld,memFree=%ld,ipv4=\"%s\"",
            deviceName,
            System.uptime(),
            System.resetReason(),
            System.version().c_str(),
            DiagnosticsHelper::getValue(DIAG_ID_SYSTEM_TOTAL_RAM),
            DiagnosticsHelper::getValue(DIAG_ID_SYSTEM_USED_RAM),
            WiFi.localIP().toString().c_str()
            );
        mqttClient.publish("telegraf/particle", buffer);
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {

    char p[length + 1];
    memcpy(p, payload, length);
    p[length] = '\0';

    if (isDebug)
        Log.info("%s - %s", topic, p);

    uint8_t topicLen = strlen(topic);
    
    char isSet[4];
    memcpy(isSet, &topic[topicLen-3], 3);
    isSet[3] = '\0';

    if (strcmp(isSet, "set") != 0)
        return;

    char command[10];
    memcpy(command, &topic[31], topicLen-35);
    command[topicLen-35] = '\0';


    int8_t light = -1;
    if (strlen(topic) > 29 && strncmp(topic, "home/study/light/front-shelf-", 29) == 0) 
    {
        light = topic[29] - '0' - 1;
    }

    if (light < 0)
        return;

    if (strcmp(command, "switch") == 0)
    {
        bool state = strcmp(p, "ON") == 0;

        if (shelfData[light].enabled == state)
            return;
        else
            shelfData[light].enabled = state;
    
        bool psuEnabledCheck = false;
        for (uint8_t i = 0; i < shelfCount; i++) {
            if (shelfData[i].enabled)
                psuEnabledCheck = true;
        }
        psuShouldBeEnabled = psuEnabledCheck;
    }
    else if (strcmp(command, "brightness") == 0)
    {
        uint8_t brightness;
        sscanf(p, "%d", &brightness);
        shelfData[light].targetBrightness = brightness;
    }
    else if (strcmp(command, "rgb") == 0)
    {
        char * token = strtok(p, ",");

        for (int i = 0; i < 3; i++) {
            shelfData[light].color[i] = atoi(token);
            token = strtok(NULL, ",");
        }
    }
    else if (strcmp(command, "effect") == 0)
    {
        if (strcmp(p, "Rainbow") == 0)
            shelfData[light].effect = LIGHT_EFFECT_RAINBOW;
        else
            shelfData[light].effect = LIGHT_EFFECT_NONE;
    }

    char bufferTopic[50];
    char bufferPayload[15];
    snprintf(bufferTopic, sizeof(bufferTopic), "home/study/light/front-shelf-%d/switch", light+1);
    mqttClient.publish(bufferTopic, shelfData[light].enabled ? "ON" : "OFF");
    snprintf(bufferTopic, sizeof(bufferTopic), "home/study/light/front-shelf-%d/brightness", light+1);
    snprintf(bufferPayload, sizeof(bufferPayload), "%d", shelfData[light].targetBrightness);
    mqttClient.publish(bufferTopic, bufferPayload);
    snprintf(bufferTopic, sizeof(bufferTopic), "home/study/light/front-shelf-%d/rgb", light+1);
    snprintf(bufferPayload, sizeof(bufferPayload), "%d,%d,%d", shelfData[light].color[0], shelfData[light].color[1], shelfData[light].color[2]);
    mqttClient.publish(bufferTopic, bufferPayload);
    snprintf(bufferTopic, sizeof(bufferTopic), "home/study/light/front-shelf-%d/effect", light+1);
    mqttClient.publish(bufferTopic, shelfData[light].effect == LIGHT_EFFECT_RAINBOW ? "Rainbow" : "None");
}

void connectToMQTT()
{
    lastMqttConnectAttempt = millis();
    bool mqttConnected = mqttClient.connect(System.deviceID(), mqttUsername, mqttPassword);
    if (mqttConnected) {
        Log.info("MQTT Connected");
        
        char buffer[40];
        for (int i = 0; i < shelfCount; i++) {
            snprintf(buffer, sizeof(buffer), "home/study/light/front-shelf-%d/#", i+1);
            mqttClient.subscribe(buffer);

            snprintf(buffer, sizeof(buffer), "home/study/light/front-shelf-%d/switch", i+1);
            mqttClient.publish(buffer, shelfData[i].enabled ? "ON" : "OFF");
        }
    }
    else
        Log.info("MQTT failed to connect");
}

SYSTEM_MODE(AUTOMATIC);
SYSTEM_THREAD(ENABLED);
STARTUP(System.enableFeature(FEATURE_RESET_INFO));

void setup()
{

    if (System.resetReason() == RESET_REASON_PANIC)
    {
        if ((Time.now() - lastHardResetTime) < 120) {
            resetCount++;
        } else {
            resetCount = 1;
        }

        lastHardResetTime = Time.now();

        if (resetCount > 2) {
            System.enterSafeMode();
        }
    } else if (System.resetReason() == RESET_REASON_WATCHDOG) {
        Particle.publish("LOG", "RESET BY WATCHDOG");
    } else {
        resetCount = 0;
    }

    pinMode(D0, OUTPUT); // Relay
    pinMode(D1, OUTPUT); // Shelf 1
    pinMode(D2, OUTPUT); // Shelf 2
    pinMode(D3, OUTPUT); // Shelf 3
  
    digitalWrite(D0, LOW);
  
    waitFor(Particle.connected, 30000);

    do {
        resetTime = Time.now();
        Particle.process();
    } while (resetTime < 1500000000 || millis() < 10000);
  
    for (uint8_t i = 0; i < shelfCount; i++) {
        shelves[i].begin();
        shelves[i].clear();
        shelves[i].show();
    }
}

void loop()
{
    static uint8_t index;
    
    if (psuShouldBeEnabled && !psuReady)
    {
        if (!psuEnabled)
        {
            digitalWrite(D0, HIGH);
            psuEnabled = true;
            psuReady = false;
            psuActionableTime = millis() + psuStartupBuffer;
        }
        else if (millis() > psuActionableTime)
        {
            psuReady = true;
            psuActionableTime = 0;
        }
    }

    if (!psuShouldBeEnabled && psuEnabled)
    {
        if (psuActionableTime == 0)
        {
            psuActionableTime = millis() + psuShutdownBuffer;
        }
        else if (millis() > psuActionableTime)
        {
            psuActionableTime = 0;
            digitalWrite(D0, LOW);
            psuReady = false;
            psuEnabled = false;
        }
    }

    if (psuShouldBeEnabled && psuReady && psuActionableTime != 0)
        psuActionableTime = 0;

    if (psuReady && millis() > nextRun)
    {
        index--; // Keep it moving
        for (int i = 0; i < shelfCount; i++)
        {
            if (!shelfData[i].enabled && !shelfData[i].active && shelfData[i].brightness == 0)
                continue;

            if (shelfData[i].enabled && !shelfData[i].active)
            {
                shelfData[i].active = true;
                shelves[i].begin();
                shelfData[i].brightness = 0;
                shelves[i].clear();
            }

            if (shelfData[i].enabled && shelfData[i].brightness != shelfData[i].targetBrightness)
            {
                if (shelfData[i].brightness <= (shelfData[i].targetBrightness-5))
                    shelfData[i].brightness += 5;
                else if (shelfData[i].targetBrightness <= (shelfData[i].brightness-5))
                    shelfData[i].brightness -= 5;
                else
                    shelfData[i].brightness = shelfData[i].targetBrightness;
                
                shelves[i].setBrightness(shelfData[i].brightness);
            }
            else if (!shelfData[i].enabled && shelfData[i].brightness != 0)
            {
                if (shelfData[i].brightness >= 5)
                    shelfData[i].brightness -= 5;
                else
                    shelfData[i].brightness = 0;
                shelves[i].setBrightness(shelfData[i].brightness);
            }
            else if (!shelfData[i].enabled && shelfData[i].brightness == 0 && shelfData[i].active)
            {
                shelfData[i].active = false;
            }

            if (shelfData[i].effect == LIGHT_EFFECT_RAINBOW)
            {
                RainbowCycleUpdate(&shelves[i], index);
            }
            else if (shelfData[i].effect == LIGHT_EFFECT_NONE)
            {
                for (uint8_t j = 0; j < shelves[i].numPixels(); j++) {
                    shelves[i].setPixelColor(j, shelfData[i].color[1], shelfData[i].color[0], shelfData[i].color[2]);
                }
            }
            shelves[i].show();
        }
        nextRun = millis() + (1000/fps);
    }

    if (mqttClient.isConnected())
    {
        mqttClient.loop();
        sendTelegrafMetrics();
    }
    else if (millis() > (lastMqttConnectAttempt + mqttConnectAtemptTimeout))
    {
        Log.info("MQTT Disconnected");
        connectToMQTT();
    }
}

// Update the Rainbow Cycle Pattern
void RainbowCycleUpdate(Adafruit_NeoPixel *pixels, uint8_t index)
{
    for(int i=0; i< pixels->numPixels(); i++)
    {
        pixels->setPixelColor(i, Wheel(((i * 256 / pixels->numPixels()) + index) & 255));
    }
}

uint32_t Wheel(byte WheelPos)
{
    WheelPos = 255 - WheelPos;
    if(WheelPos < 85)
    {
        return shelves[0].Color(255 - WheelPos * 3, 0, WheelPos * 3);
    }
    else if(WheelPos < 170)
    {
        WheelPos -= 85;
        return shelves[0].Color(0, WheelPos * 3, 255 - WheelPos * 3);
    }
    else
    {
        WheelPos -= 170;
        return shelves[0].Color(WheelPos * 3, 255 - WheelPos * 3, 0);
    }
}
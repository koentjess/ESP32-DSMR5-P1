#include <FS.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <PubSubClient.h>
#include "esp32-hal-cpu.h"
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

// * Include settings
#include "settings.h"

#define LED_BUILTIN 2

AsyncWebServer server(80);

// * Initiate WIFI client 
WiFiClient espClient;

const char* ssid     = "InsertWifiName";
const char* password = "InserWifiPassword";

// * Initiate MQTT client
PubSubClient mqtt_client(espClient);



// **********************************
// * MQTT                           *
// **********************************

// * Send a message to a broker topic
void send_mqtt_message(const char *topic, char *payload)
{
    Serial.printf("MQTT Outgoing on %s: ", topic);
    Serial.println(payload);

    bool result = mqtt_client.publish(topic, payload, false);

    if (!result)
    {
        Serial.printf("MQTT publish to topic %s failed\n", topic);
    }
}

// * Reconnect to MQTT server and subscribe to in and out topics
bool mqtt_reconnect()
{
    // * Loop until we're reconnected
    int MQTT_RECONNECT_RETRIES = 0;

    while (!mqtt_client.connected() && MQTT_RECONNECT_RETRIES < MQTT_MAX_RECONNECT_TRIES)
    {
        MQTT_RECONNECT_RETRIES++;
        Serial.printf("MQTT connection attempt %d / %d ...\n", MQTT_RECONNECT_RETRIES, MQTT_MAX_RECONNECT_TRIES);
        Serial.println(WiFi.localIP());
        // * Attempt to connect
        if (mqtt_client.connect(HOSTNAME, MQTT_USER, MQTT_PASS))
        {
            Serial.println(F("MQTT connected!"));

            // * Once connected, publish an announcement...
            char *message = new char[16 + strlen(HOSTNAME) + 1];
            strcpy(message, "p1 meter alive: ");
            strcat(message, HOSTNAME);
            mqtt_client.publish("hass/status", message);

            Serial.printf("MQTT root topic: %s\n", MQTT_ROOT_TOPIC);
        }
        else
        {
            Serial.print(F("MQTT Connection failed: rc="));
            Serial.println(mqtt_client.state());
            Serial.println(F(" Retrying in 5 seconds"));
            Serial.println("");

            // * Wait 5 seconds before retrying
            delay(5000);
            if (MQTT_RECONNECT_RETRIES >= 3)
            {
                 WiFi.reconnect();
            }
        }
    }

    if (MQTT_RECONNECT_RETRIES >= MQTT_MAX_RECONNECT_TRIES)
    {
        Serial.printf("*** MQTT connection failed, giving up after %d tries ...\n", MQTT_RECONNECT_RETRIES);
        return false;
    }

    return true;
}

void send_metric(String name, long metric)
{
    Serial.print(F("Sending metric to broker: "));
    Serial.print(name);
    Serial.print(F("="));
    Serial.println(metric);

    char output[10];
    ltoa(metric, output, sizeof(output));

    String topic = String(MQTT_ROOT_TOPIC) + "/" + name;
    send_mqtt_message(topic.c_str(), output);
}

void send_data_to_broker()
{
    send_metric("consumption_low_tarif", CONSUMPTION_LOW_TARIF);
    send_metric("consumption_high_tarif", CONSUMPTION_HIGH_TARIF);
    send_metric("returndelivery_low_tarif", RETURNDELIVERY_LOW_TARIF);
    send_metric("returndelivery_high_tarif", RETURNDELIVERY_HIGH_TARIF);
    send_metric("actual_consumption", ACTUAL_CONSUMPTION);
    send_metric("actual_returndelivery", ACTUAL_RETURNDELIVERY);

    send_metric("l1_instant_power_usage", L1_INSTANT_POWER_USAGE);
    send_metric("l2_instant_power_usage", L2_INSTANT_POWER_USAGE);
    send_metric("l3_instant_power_usage", L3_INSTANT_POWER_USAGE);
    send_metric("l1_instant_power_current", L1_INSTANT_POWER_CURRENT);
    send_metric("l2_instant_power_current", L2_INSTANT_POWER_CURRENT);
    send_metric("l3_instant_power_current", L3_INSTANT_POWER_CURRENT);
    send_metric("l1_voltage", L1_VOLTAGE);
    send_metric("l2_voltage", L2_VOLTAGE);
    send_metric("l3_voltage", L3_VOLTAGE);
    
    send_metric("gas_meter_m3", GAS_METER_M3);

    send_metric("actual_tarif_group", ACTUAL_TARIF);
    send_metric("short_power_outages", SHORT_POWER_OUTAGES);
    send_metric("long_power_outages", LONG_POWER_OUTAGES);
    send_metric("short_power_drops", SHORT_POWER_DROPS);
    send_metric("short_power_peaks", SHORT_POWER_PEAKS);
}

// **********************************
// * P1                             *
// **********************************

unsigned int CRC16(unsigned int crc, unsigned char *buf, int len)
{
  for (int pos = 0; pos < len; pos++)
    {
    crc ^= (unsigned int)buf[pos];    // * XOR byte into least sig. byte of crc
                                          // * Loop over each bit
        for (int i = 8; i != 0; i--)
        {
            // * If the LSB is set
            if ((crc & 0x0001) != 0)
            {
                // * Shift right and XOR 0xA001
                crc >>= 1;
        crc ^= 0xA001;
      }
            // * Else LSB is not set
            else
                // * Just shift right
                crc >>= 1;
    }
  }
  return crc;
}

bool isNumber(char *res, int len)
{
    for (int i = 0; i < len; i++)
    {
        if (((res[i] < '0') || (res[i] > '9')) && (res[i] != '.' && res[i] != 0))
            return false;
    }
    return true;
}

int FindCharInArrayRev(char array[], char c, int len)
{
    for (int i = len - 1; i >= 0; i--)
    {
        if (array[i] == c)
            return i;
    }
    return -1;
}

long getValue(char *buffer, int maxlen, char startchar, char endchar)
{
    int s = FindCharInArrayRev(buffer, startchar, maxlen - 2);
    int l = FindCharInArrayRev(buffer, endchar, maxlen - 2) - s - 1;

    char res[16];
    memset(res, 0, sizeof(res));

    if (strncpy(res, buffer + s + 1, l))
    {
        if (endchar == '*')
        {
            if (isNumber(res, l))
                // * Lazy convert float to long
                return (1000 * atof(res));
        }
        else if (endchar == ')')
        {
            if (isNumber(res, l))
                return atof(res);
        }
    }
    return 0;
}

bool decode_telegram(int len)
{
    int startChar = FindCharInArrayRev(telegram, '/', len);
    int endChar = FindCharInArrayRev(telegram, '!', len);
    bool validCRCFound = false;

    for (int cnt = 0; cnt < len; cnt++) {
        Serial.print(telegram[cnt]);
    }
    Serial.print("\n");

    if (startChar >= 0)
    {
        // * Start found. Reset CRC calculation
        currentCRC = CRC16(0x0000,(unsigned char *) telegram+startChar, len-startChar);
    }
    else if (endChar >= 0)
    {
        // * Add to crc calc
        currentCRC = CRC16(currentCRC,(unsigned char*)telegram+endChar, 1);

        char messageCRC[5];
        strncpy(messageCRC, telegram + endChar + 1, 4);

        messageCRC[4] = 0;   // * Thanks to HarmOtten (issue 5)
        validCRCFound = (strtol(messageCRC, NULL, 16) == currentCRC);

        if (validCRCFound)
            Serial.println(F("CRC Valid!"));
        else
            Serial.println(F("CRC Invalid!"));

        currentCRC = 0;
    }
    else
    {
        currentCRC = CRC16(currentCRC, (unsigned char*) telegram, len);
    }

    // 1-0:1.8.1(000992.992*kWh)
    // 1-0:1.8.1 = Elektra verbruik laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.1", strlen("1-0:1.8.1")) == 0)
    {
        CONSUMPTION_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.8.2(000560.157*kWh)
    // 1-0:1.8.2 = Elektra verbruik hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.8.2", strlen("1-0:1.8.2")) == 0)
    {
        CONSUMPTION_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }
  
    // 1-0:2.8.1(000560.157*kWh)
    // 1-0:2.8.1 = Elektra teruglevering laag tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.1", strlen("1-0:2.8.1")) == 0)
    {
        RETURNDELIVERY_LOW_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.8.2(000560.157*kWh)
    // 1-0:2.8.2 = Elektra teruglevering hoog tarief (DSMR v4.0)
    if (strncmp(telegram, "1-0:2.8.2", strlen("1-0:2.8.2")) == 0)
    {
        RETURNDELIVERY_HIGH_TARIF = getValue(telegram, len, '(', '*');
    }

    // 1-0:1.7.0(00.424*kW) Actueel verbruik
    // 1-0:1.7.x = Electricity consumption actual usage (DSMR v4.0)
    if (strncmp(telegram, "1-0:1.7.0", strlen("1-0:1.7.0")) == 0)
    {
        ACTUAL_CONSUMPTION = getValue(telegram, len, '(', '*');
    }

    // 1-0:2.7.0(00.000*kW) Actuele teruglevering (-P) in 1 Watt resolution
    if (strncmp(telegram, "1-0:2.7.0", strlen("1-0:2.7.0")) == 0)
    {
        ACTUAL_RETURNDELIVERY = getValue(telegram, len, '(', '*');
    }

    // 1-0:21.7.0(00.378*kW)
    // 1-0:21.7.0 = Instantaan vermogen Elektriciteit levering L1
    if (strncmp(telegram, "1-0:21.7.0", strlen("1-0:21.7.0")) == 0)
    {
        L1_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:41.7.0(00.378*kW)
    // 1-0:41.7.0 = Instantaan vermogen Elektriciteit levering L2
    if (strncmp(telegram, "1-0:41.7.0", strlen("1-0:41.7.0")) == 0)
    {
        L2_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:61.7.0(00.378*kW)
    // 1-0:61.7.0 = Instantaan vermogen Elektriciteit levering L3
    if (strncmp(telegram, "1-0:61.7.0", strlen("1-0:61.7.0")) == 0)
    {
        L3_INSTANT_POWER_USAGE = getValue(telegram, len, '(', '*');
    }

    // 1-0:31.7.0(002*A)
    // 1-0:31.7.0 = Instantane stroom Elektriciteit L1
    if (strncmp(telegram, "1-0:31.7.0", strlen("1-0:31.7.0")) == 0)
    {
        L1_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }
    // 1-0:51.7.0(002*A)
    // 1-0:51.7.0 = Instantane stroom Elektriciteit L2
    if (strncmp(telegram, "1-0:51.7.0", strlen("1-0:51.7.0")) == 0)
    {
        L2_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }
    // 1-0:71.7.0(002*A)
    // 1-0:71.7.0 = Instantane stroom Elektriciteit L3
    if (strncmp(telegram, "1-0:71.7.0", strlen("1-0:71.7.0")) == 0)
    {
        L3_INSTANT_POWER_CURRENT = getValue(telegram, len, '(', '*');
    }

    // 1-0:32.7.0(232.0*V)
    // 1-0:32.7.0 = Voltage L1
    if (strncmp(telegram, "1-0:32.7.0", strlen("1-0:32.7.0")) == 0)
    {
        L1_VOLTAGE = getValue(telegram, len, '(', '*');
    }
    // 1-0:52.7.0(232.0*V)
    // 1-0:52.7.0 = Voltage L2
    if (strncmp(telegram, "1-0:52.7.0", strlen("1-0:52.7.0")) == 0)
    {
        L2_VOLTAGE = getValue(telegram, len, '(', '*');
    }   
    // 1-0:72.7.0(232.0*V)
    // 1-0:72.7.0 = Voltage L3
    if (strncmp(telegram, "1-0:72.7.0", strlen("1-0:72.7.0")) == 0)
    {
        L3_VOLTAGE = getValue(telegram, len, '(', '*');
    }

    // 0-1:24.2.1(150531200000S)(00811.923*m3)
    // 0-1:24.2.1 = Gas (DSMR v4.0) on Kaifa MA105 meter
    if (strncmp(telegram, "0-1:24.2.1", strlen("0-1:24.2.1")) == 0)
    {
        GAS_METER_M3 = getValue(telegram, len, '(', '*');
    }

    // 0-0:96.14.0(0001)
    // 0-0:96.14.0 = Actual Tarif
    if (strncmp(telegram, "0-0:96.14.0", strlen("0-0:96.14.0")) == 0)
    {
        ACTUAL_TARIF = getValue(telegram, len, '(', ')');
    }

    // 0-0:96.7.21(00003)
    // 0-0:96.7.21 = Aantal onderbrekingen Elektriciteit
    if (strncmp(telegram, "0-0:96.7.21", strlen("0-0:96.7.21")) == 0)
    {
        SHORT_POWER_OUTAGES = getValue(telegram, len, '(', ')');
    }

    // 0-0:96.7.9(00001)
    // 0-0:96.7.9 = Aantal lange onderbrekingen Elektriciteit
    if (strncmp(telegram, "0-0:96.7.9", strlen("0-0:96.7.9")) == 0)
    {
        LONG_POWER_OUTAGES = getValue(telegram, len, '(', ')');
    }

    // 1-0:32.32.0(00000)
    // 1-0:32.32.0 = Aantal korte spanningsdalingen Elektriciteit in fase 1
    if (strncmp(telegram, "1-0:32.32.0", strlen("1-0:32.32.0")) == 0)
    {
        SHORT_POWER_DROPS = getValue(telegram, len, '(', ')');
    }

    // 1-0:32.36.0(00000)
    // 1-0:32.36.0 = Aantal korte spanningsstijgingen Elektriciteit in fase 1
    if (strncmp(telegram, "1-0:32.36.0", strlen("1-0:32.36.0")) == 0)
    {
        SHORT_POWER_PEAKS = getValue(telegram, len, '(', ')');
    }

    return validCRCFound;
}

void read_p1_hardwareserial()
{
  
    if (Serial2.available())
    {
        memset(telegram, 0, sizeof(telegram));

        while (Serial2.available())
        {
            int len = Serial2.readBytesUntil('\n', telegram, P1_MAXLINELENGTH);

            processLine(len);
            
        }
    }
}

void processLine(int len) {
    telegram[len] = '\n';
    telegram[len + 1] = 0;
    yield();

    bool result = decode_telegram(len + 1);
    if (result) {
        send_data_to_broker();
        LAST_UPDATE_SENT = millis();
    }
}






// **********************************
// * Setup Main                     *
// **********************************

void setup()
{
    Serial.begin(BAUD_RATE, SERIAL_8N1, 115200);
    Serial.println("");
    Serial.println("Swapping UART0 RX to inverted");
    Serial.flush();
    Serial2.begin(115200, SERIAL_8N1, 16, 17,true);
    Serial.println("Serial port is ready to recieve.");

    // * Set led pin as output
    pinMode(LED_BUILTIN, OUTPUT);

    delay(5000);

    WiFi.begin(ssid, password);

    // * If you get here you have connected to the WiFi
    Serial.println(F("Connected to WIFI..."));


    // * Setup MQTT
    Serial.printf("MQTT connecting to: %s:%s\n", MQTT_HOST, MQTT_PORT);
    mqtt_client.setServer(MQTT_HOST, atoi(MQTT_PORT));



    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
      request->send(200, "text/plain", "Hi! This is a sample response.");
    });

    AsyncElegantOTA.begin(&server);    // Start AsyncElegantOTA
    server.begin();
    Serial.println("HTTP server started");

  
}

// **********************************
// * Loop                           *
// **********************************

void loop()
{
    long now = millis();

    if (!mqtt_client.connected())
    {
        if (now - LAST_RECONNECT_ATTEMPT > 5000)
        {
            LAST_RECONNECT_ATTEMPT = now;

            if (mqtt_reconnect())
            {
                LAST_RECONNECT_ATTEMPT = 0;
            }
        }
    }
    else
    {
        mqtt_client.loop();
    }
    
    if (now - LAST_UPDATE_SENT > UPDATE_INTERVAL) {
        read_p1_hardwareserial();
        
    }
}

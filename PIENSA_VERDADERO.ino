#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#define SENSOR_PIN 14  // Pin del sensor de flujo
#define RELAY_PIN 5     // Pin del relé que controla la válvula

const char* ssid = "Suda";
const char* password = "";
const char* serverName = "http://10.0.1.197:3000/sensor";
const char* toggleValveEndpoint = "http://10.0.1.197:3000/sensor/toggle-valve";

volatile int pulseCount = 0;
float flowRate = 0.0;
float totalLiters = 0.0;
unsigned long lastTime = 0;
unsigned long lowFlowStart = 0; // Tiempo en el que inicia flujo bajo
unsigned long highFlowStart = 0; // Tiempo en el que inicia flujo alto
bool isValveClosed = false;

void IRAM_ATTR countPulses() {
    pulseCount++;
}

void setup() {
    Serial.begin(115200);
    pinMode(SENSOR_PIN, INPUT_PULLUP);
    pinMode(RELAY_PIN, OUTPUT);

    attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), countPulses, FALLING);

    // Iniciar la válvula abierta
    digitalWrite(RELAY_PIN, HIGH);
    Serial.println("Electroválvula: ABIERTA");

    // Conectar a WiFi
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(1000);
        Serial.println("Connecting to WiFi...");
    }
    Serial.println("Connected to WiFi");

    // Configurar el tiempo
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
}

String getFormattedTime() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return "";
    }
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(timeStringBuff);
}

void checkValveStatus() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(toggleValveEndpoint);
        int httpResponseCode = http.GET();

        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.println(httpResponseCode);
            Serial.println(response);

            if (response.indexOf("\"status\":true") != -1) {
                digitalWrite(RELAY_PIN, HIGH); // Abrir válvula
                Serial.println("Electroválvula: ABIERTA ✅");
                isValveClosed = false;
            } else if (response.indexOf("\"status\":false") != -1) {
                digitalWrite(RELAY_PIN, LOW); // Cerrar válvula
                Serial.println("Electroválvula: CERRADA ❌");
                isValveClosed = true;
            }
        } else {
            Serial.print("Error on sending GET: ");
            Serial.println(httpResponseCode);
        }

        http.end();
    }
}

void loop() {
    if (millis() - lastTime >= 1000) { // Calcular cada 1 segundo
        detachInterrupt(SENSOR_PIN);

        // Conversión de pulsos a flujo (YF-S201: 1 pulso = 2.25 ml)
        flowRate = (pulseCount / 4.5); // ml/s
        float liters = flowRate / 1000.0; // Convertir a litros
        totalLiters += liters;

        Serial.print("Flujo actual: ");
        Serial.print(flowRate);
        Serial.print(" mL/s | ");
        Serial.print(liters);
        Serial.print(" L/s | Total: ");
        Serial.print(totalLiters);
        Serial.println(" L");

        // --- Lógica para el control de la válvula ---
        if (flowRate >= 20) {  // Si el flujo supera 0.02 L/min (20 mL/s)
            if (highFlowStart == 0) {
                highFlowStart = millis();  // Guardar el tiempo de inicio del flujo alto
            } else if (millis() - highFlowStart >= 5000) {  // Si han pasado 5 segundos seguidos
                digitalWrite(RELAY_PIN, LOW);  // Cerrar válvula
                Serial.println("❌ ¡ALERTA! Consumo excesivo detectado. CERRANDO VÁLVULA ❌");
                isValveClosed = true;
            }
        } else {
            highFlowStart = 0;  // Reiniciar contador de flujo alto
        }

        if (flowRate > 0 && flowRate < 83.33) {  // Flujo bajo detectado (posible fuga 5 L/min)
            if (lowFlowStart == 0) {
                lowFlowStart = millis();  // Guardar el tiempo de inicio del flujo bajo
            } else if (millis() - lowFlowStart >= 15000) {  // Si han pasado 15 segundos seguidos
                digitalWrite(RELAY_PIN, LOW); // Cerrar válvula
                Serial.println("⚠️ ¡ALERTA! Posible fuga detectada. CERRANDO VÁLVULA ⚠️");
                isValveClosed = true;
            }
        } else {
            lowFlowStart = 0; // Reiniciar contador de flujo bajo
        }

        if (flowRate == 0 || (flowRate >= 83.33 && flowRate < 100)) {  // Flujo normal (sin flujo o mayor a 5L/min y menor a 6L/min)
            if (!isValveClosed) {
                digitalWrite(RELAY_PIN, HIGH);  // Mantener válvula abierta
                Serial.println("Electroválvula: ABIERTA ✅");
            }
        }

        // Enviar datos al backend
        if (WiFi.status() == WL_CONNECTED) {
            HTTPClient http;
            http.begin(serverName);
            http.addHeader("Content-Type", "application/json");

            String timestamp = getFormattedTime();
            
            bool leak_status = (flowRate > 0 && flowRate < 20); // Reducimos el umbral para detectar fugas
            bool high_consumption = (flowRate >= 50); // Ajustamos el umbral de consumo alto

            String jsonData = "{\"timestamp\":\"" + timestamp + "\",\"leak_status\":" + (leak_status ? "true" : "false") + ",\"high_consumption\":" + (high_consumption ? "true" : "false") + ",\"valve_status\":" + (!isValveClosed ? "true" : "false") + ",\"location\":\"baño\",\"user_id\":1,\"flowRate\":" + String(flowRate) + ",\"totalLiters\":" + String(totalLiters) + "}";


            int httpResponseCode = http.POST(jsonData);

            if (httpResponseCode > 0) {
                String response = http.getString();
                Serial.println(httpResponseCode);
                Serial.println(response);
            } else {
                Serial.print("Error on sending POST: ");
                Serial.println(httpResponseCode);
            }

            http.end();
        }

        pulseCount = 0;
        lastTime = millis();
        attachInterrupt(digitalPinToInterrupt(SENSOR_PIN), countPulses, FALLING);
    }

    // Verificar el estado de la válvula cada 5 segundos
    if (millis() - lastTime >= 1000) {
        checkValveStatus();
    }
}
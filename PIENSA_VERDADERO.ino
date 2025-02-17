#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

#define SENSOR_PIN 14  // Pin del sensor de flujo
#define RELAY_PIN 5     // Pin del relé que controla la válvula

const char* ssid = "TP-Link_8EBD";
const char* password = "68399658";
const char* serverName = "http://192.168.0.103:3000/sensor";
const char* valveStatusEndpoint = "http://192.168.0.103:3000/sensor/valve-status";

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
    configTime(-5 * 3600, 0, "pool.ntp.org", "time.nist.gov");
}

String getFormattedTime() {
    time_t now;
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Failed to obtain time");
        return "";
    }
    char timeStringBuff[50];
    strftime(timeStringBuff, sizeof(timeStringBuff), "%Y-%m-%dT%H:%M:%S-05:00", &timeinfo);
    return String(timeStringBuff);
}

void checkValveStatus() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        http.begin(valveStatusEndpoint);
        int httpResponseCode = http.GET();

          
        if (httpResponseCode > 0) {
            String response = http.getString();
            Serial.println("Respuesta del servidor: " + response);

            // Parsear la respuesta de manera más robusta
            if (response.indexOf("true") != -1) {
                // Forzar la apertura de la válvula independientemente del estado anterior
                digitalWrite(RELAY_PIN, HIGH);
                isValveClosed = false;
                lowFlowStart = 0;  // Reiniciar detectores de flujo
                highFlowStart = 0;
                Serial.println("✅ Válvula ABIERTA por comando del servidor");
            } else if (response.indexOf("false") != -1) {
                digitalWrite(RELAY_PIN, LOW);
                isValveClosed = true;
                Serial.println("❌ Válvula CERRADA por comando del servidor");
            }
        }
        http.end();
    } else {
        Serial.println("Error: No hay conexión WiFi");
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
if (flowRate >= 20) {  // Si el flujo supera 20 mL/s
    if (highFlowStart == 0) {
        highFlowStart = millis();  // Guardar el tiempo de inicio del flujo alto
        Serial.println("⚠️ Detectando flujo alto... iniciando conteo");
    } else if (millis() - highFlowStart >= 5000) {  // Si han pasado 5 segundos seguidos
        digitalWrite(RELAY_PIN, LOW);  // Cerrar válvula
        isValveClosed = true;
        Serial.println("❌ ¡ALERTA! Consumo excesivo detectado. CERRANDO VÁLVULA ❌");
        
        // Enviar actualización al servidor
        HTTPClient http;
        http.begin(serverName);
        http.addHeader("Content-Type", "application/json");
        String jsonData = "{\"timestamp\":\"" + getFormattedTime() + 
                         "\",\"leak_status\":false,\"high_consumption\":true," +
                         "\"valve_status\":false,\"location\":\"baño\",\"user_id\":1," +
                         "\"flowRate\":" + String(flowRate) + 
                         ",\"totalLiters\":" + String(totalLiters) + "}";
        http.POST(jsonData);
        http.end();
    }
} else {
    if (highFlowStart != 0) {
        Serial.println("Flujo alto normalizado, reiniciando conteo");
    }
    highFlowStart = 0;  // Reiniciar contador de flujo alto
}

// Detección de fugas (flujo bajo constante)
if (flowRate > 0 && flowRate < 83.33) {  // Flujo bajo detectado
    if (lowFlowStart == 0) {
        lowFlowStart = millis();  // Guardar el tiempo de inicio del flujo bajo
        Serial.println("⚠️ Detectando posible fuga... iniciando conteo");
    } else if (millis() - lowFlowStart >= 15000) {  // Si han pasado 15 segundos seguidos
        digitalWrite(RELAY_PIN, LOW); // Cerrar válvula
        isValveClosed = true;
        Serial.println("⚠️ ¡ALERTA! Posible fuga detectada. CERRANDO VÁLVULA ⚠️");
        
        // Enviar actualización al servidor
        HTTPClient http;
        http.begin(serverName);
        http.addHeader("Content-Type", "application/json");
        String jsonData = "{\"timestamp\":\"" + getFormattedTime() + 
                         "\",\"leak_status\":true,\"high_consumption\":false," +
                         "\"valve_status\":false,\"location\":\"baño\",\"user_id\":1," +
                         "\"flowRate\":" + String(flowRate) + 
                         ",\"totalLiters\":" + String(totalLiters) + "}";
        http.POST(jsonData);
        http.end();
    }
} else {
    if (lowFlowStart != 0) {
        Serial.println("Flujo bajo normalizado, reiniciando conteo");
    }
    lowFlowStart = 0; // Reiniciar contador de flujo bajo
}

// Solo mantener la válvula abierta si no hay condiciones de alarma
if (flowRate == 0 || (flowRate >= 83.33 && flowRate < 100)) {  // Flujo normal
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

            String jsonData = "{\"timestamp\":\"" + timestamp + "\",\"leak_status\":" + (leak_status ? "true" : "false") + ",\"high_consumption\":" + (high_consumption ? "true" : "false") + ",\"valve_status\":" + (!isValveClosed ? "true" : "false") + ",\"location\":\"Baño Principal\",\"user_id\":1,\"flowRate\":" + String(flowRate) + ",\"totalLiters\":" + String(totalLiters) + "}";


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

    // Verificar el estado de la válvula cada 2 segundos
    static unsigned long lastValveCheck = 0;
    if (millis() - lastValveCheck >= 2000) {
        lastValveCheck = millis();
        checkValveStatus();

        // Imprimir estado actual para debug
        Serial.print("Estado actual de la válvula: ");
        Serial.println(isValveClosed ? "CERRADA" : "ABIERTA");
    }
}
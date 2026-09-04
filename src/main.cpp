#include <Arduino.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
// #include "BluetoothSerial.h"
// Bluetooth desativado por limitações de espaço

// Verifica se o Bluetooth está habilitado nas configurações do chip
// #if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
// #error O Bluetooth não está habilitado! Habilite-o no menuconfig.
// #endif

// BluetoothSerial SerialBT;

// Macro inteligente para imprimir em ambas as seriais (USB e Bluetooth)
#define DEBUG_PRINT(x)    { Serial.print(x); /*SerialBT.print(x);*/ }
#define DEBUG_PRINTLN(x)  { Serial.println(x); /*SerialBT.println(x);*/ }
#define DEBUG_PRINTF(format, ...) { Serial.printf(format, ##__VA_ARGS__); /*SerialBT.printf(format, ##__VA_ARGS__);*/ }

#define TRIGGER_PIN 0 // Usa o próprio botão BOOT do ESP32 para reset
#define MOTOR_PIN_CLOCKWISE 23
#define MOTOR_PIN_COUNTER_CLOCKWISE 34

Preferences preferences;
char torno_id[32] = "TORNO_01"; // Valor padrão caso nunca tenha sido configurado

// Configurações do Servidor NTP e Fuso Horário
const char* ntpServer = "a.st1.ntp.br";  // Servidor NTP (pode usar "pool.ntp.org")
const long gmtOffset_sec = -3 * 3600;    // Fuso Horário (UTC-3 / Brasília: -3 * 3600 segundos)
const int daylightOffset_sec = 0;        // Horário de verão (0 se não houver)

// URL Base da planilha Google
String urlBase = "https://script.google.com/macros/s/AKfycbxvy1evArJ8iMJhsP3SbZJcrJohTcLogSUlTB3r18W2Avmwf9mfFNmOhiLKdTUrxkBORw/exec";

bool lastPinState = LOW;

// --- Estrutura do Buffer ---
struct MotorEvent {
    String state;
    time_t timestamp;
};
std::vector<MotorEvent> eventBuffer;

// --- Variáveis da Máquina de Estados ---
enum SystemState { IDLE, RUNNING, PAUSED };
SystemState currentState = IDLE;

unsigned long lastStateChangeMillis = 0;
const unsigned long TIMEOUT_OFF_MS = 60000; // 1 minuto em milissegundos

unsigned long totalTimeOn = 0;
unsigned long totalTimeOff = 0;
time_t operationStartTime = 0;
time_t operationEndTime = 0;

WiFiClientSecure client;
HTTPClient http;

// URLs do seu repositório no GitHub (Links "Raw")
const char* versionURL = "https://raw.githubusercontent.com/ramonmoraesrr/horimetro_senai390/master/version.txt";
const char* firmwareURL = "https://raw.githubusercontent.com/ramonmoraesrr/horimetro_senai390/master/firmware.bin";

// --- Funções Auxiliares ---
void checkForUpdates() {
//   Serial.println("Buscando atualizações no GitHub...");
  DEBUG_PRINTLN("Buscando atualizações no GitHub...");
    
  WiFiClientSecure client;
  client.setInsecure(); // Necessário para acessar HTTPS ignorando validação de certificado
  
  HTTPClient http;
  http.begin(client, versionURL);
  
  int httpCode = http.GET();
  
  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    
    // FIRMWARE_VERSION é injetado automaticamente pelo script Python!
    long currentVersion = FIRMWARE_VERSION; 
    long serverVersion = payload.toInt();
    
    // Serial.printf("Versão atual (ESP32): %ld\n", currentVersion);
    // Serial.printf("Versão online (GitHub): %ld\n", serverVersion);
    DEBUG_PRINTF("Versão atual (ESP32): %ld\n", currentVersion);
    DEBUG_PRINTF("Versão online (GitHub): %ld\n", serverVersion);
    
    // Verifica se a versão da nuvem é mais recente (maior timestamp)
    if (serverVersion > currentVersion) {
    //   Serial.println("Nova versão encontrada! Baixando firmware.bin...");
      DEBUG_PRINTLN("Nova versão encontrada! Baixando firmware.bin...");
      
      t_httpUpdate_return ret = httpUpdate.update(client, firmwareURL);
      
      switch (ret) {
        case HTTP_UPDATE_FAILED:
        //   Serial.printf("Falha no OTA (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          DEBUG_PRINTF("Falha no OTA (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_NO_UPDATES:
        //   Serial.println("Nenhuma atualização encontrada na URL do binário.");
          DEBUG_PRINTLN("Nenhuma atualização encontrada na URL do binário.");
          break;
        case HTTP_UPDATE_OK:
        //   Serial.println("Atualização concluída com sucesso! Reiniciando...");
          DEBUG_PRINTLN("Atualização concluída com sucesso! Reiniciando...");
          break;
      }
    } else {
    //   Serial.println("O firmware já está na versão mais recente.");
      DEBUG_PRINTLN("O firmware já está na versão mais recente.");
    }
  } else {
    // Serial.printf("Falha ao acessar version.txt. Erro HTTP: %d\n", httpCode);
    DEBUG_PRINTF("Falha ao acessar version.txt. Erro HTTP: %d\n", httpCode);
  }
  
  http.end();
}

String formatTime(time_t epochTime) {
    struct tm *timeinfo = localtime(&epochTime);
    char buffer[30];
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %H:%M:%S", timeinfo);
    return String(buffer);
}

String urlEncode(String str) {
    String encodedString = "";
    char c;
    char code0;
    char code1;
    for (int i = 0; i < str.length(); i++) {
        c = str.charAt(i);
        if (isalnum(c)) {
            encodedString += c;
        } else if (c == ' ') {
            encodedString += "%20";
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
            encodedString += '%';
            encodedString += code0;
            encodedString += code1;
        }
    }
    return encodedString;
}

void printFinalReport() {
    // Serial.println("\n--- RELATÓRIO FINAL DA OPERAÇÃO ---");
    // Serial.printf("Início da Operação: %s\n", formatTime(operationStartTime).c_str());
    // Serial.printf("Fim da Operação: %s\n", formatTime(operationEndTime).c_str());
    DEBUG_PRINTLN("\n--- RELATÓRIO FINAL DA OPERAÇÃO ---");
    DEBUG_PRINTF("Início da Operação: %s\n", formatTime(operationStartTime).c_str());
    DEBUG_PRINTF("Fim da Operação: %s\n", formatTime(operationEndTime).c_str());
    
    // Converte milissegundos para segundos para exibição
    // Serial.printf("Tempo Total Ligado: %lu segundos\n", totalTimeOn / 1000);
    // Serial.printf("Tempo Total Desligado: %lu segundos\n", totalTimeOff / 1000);
    DEBUG_PRINTF("Tempo Total Ligado: %lu segundos\n", totalTimeOn / 1000);
    DEBUG_PRINTF("Tempo Total Desligado: %lu segundos\n", totalTimeOff / 1000);
    
    // Serial.println("\n--- Histórico de Eventos (Buffer) ---");
    DEBUG_PRINTLN("\n--- Histórico de Eventos (Buffer) ---");
    for (size_t i = 0; i < eventBuffer.size(); i++) {
        // Serial.printf("[%s] Motor %s\n", formatTime(eventBuffer[i].timestamp).c_str(), eventBuffer[i].state.c_str());
        DEBUG_PRINTF("[%s] Motor %s\n", formatTime(eventBuffer[i].timestamp).c_str(), eventBuffer[i].state.c_str());
    }
    // Serial.println("-----------------------------------\n");
    DEBUG_PRINTLN("-----------------------------------\n");

    // Monta a URL final com os parâmetros do GET
    // O resultado será: https://script.google.../exec?id_torno=4&start_datetime=1234&end_datetime=1234&uptime=1234
    String urlFinal = urlBase + "?id_torno=" + urlEncode(torno_id) + "&start_datetime=" + urlEncode(formatTime(operationStartTime)) + "&end_datetime=" + urlEncode(formatTime(operationEndTime)) + "&uptime=" + String(totalTimeOn / 1000);

    // Serial.println("Enviando requisição para: " + urlFinal);
    DEBUG_PRINTLN("Enviando requisição para: " + urlFinal);

    // Inicia a conexão
    http.begin(client, urlFinal);
    
    // O redirecionamento no GET funciona perfeitamente no ESP32
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    // Faz a requisição GET
    int codigoResposta = http.GET();

    if (codigoResposta > 0) {
    //   Serial.print("Código HTTP: ");
    //   Serial.println(codigoResposta);
      DEBUG_PRINT("Código HTTP: ");
      DEBUG_PRINTLN(codigoResposta);
      String respostaServidor = http.getString();
    //   Serial.println("Resposta: " + respostaServidor);
      DEBUG_PRINTLN("Resposta: " + respostaServidor);
    } else {
    //   Serial.print("Erro no envio: ");
    //   Serial.println(http.errorToString(codigoResposta).c_str());
      DEBUG_PRINT("Erro no envio: ");
      DEBUG_PRINTLN(http.errorToString(codigoResposta).c_str());
    }

    http.end();
}

void setup() {
    Serial.begin(115200);

    // Inicia o Bluetooth com o nome que vai aparecer no seu PC/Celular
    // SerialBT.begin("ESP32_Debug_BT"); 
    // Serial.println("O dispositivo Bluetooth iniciou. Pode parear agora!");
    // Bluetooth desativado por questões de espaço. O monitoramente só será possível via cabo USB

    // 1. Configuração dos Pinos de Hardware
    pinMode(MOTOR_PIN_CLOCKWISE, INPUT_PULLDOWN);
    pinMode(MOTOR_PIN_COUNTER_CLOCKWISE, INPUT_PULLDOWN);
    pinMode(TRIGGER_PIN, INPUT_PULLUP);

    // 2. Carrega o ID salvo anteriormente na memória Flash (NVS)
    preferences.begin("config", false);
    String saved_id = preferences.getString("torno_id", "TORNO_01");
    saved_id.toCharArray(torno_id, sizeof(torno_id));
    preferences.end();

    // 3. Janela para Reset das Credenciais Wi-Fi (opcional)
    Serial.println("\nIniciando... Segure o botão BOOT agora se quiser redefinir o Wi-Fi.");
    delay(3000);

    WiFiManager wm;

    // 4. Cria o campo customizado no portal HTML do WiFiManager
    // Argumentos: ID HTML, Rótulo, Valor Padrão, Tamanho Máximo
    WiFiManagerParameter custom_torno_id("torno_id_key", "ID do Torno / Máquina", torno_id, 32);
    wm.addParameter(&custom_torno_id);

    // Se o botão BOOT estiver pressionado ao ligar, apaga o Wi-Fi salvo
    if (digitalRead(TRIGGER_PIN) == LOW) {
        Serial.println("Botão BOOT pressionado! Apagando Wi-Fi salvo...");
        wm.resetSettings();
        delay(1000);
    }

    // 5. Conexão Wi-Fi via WiFiManager
    // Substitui o WiFi.begin() e o loop while(WiFi.status() != WL_CONNECTED)
    // Define timeout para o portal não ficar aberto para sempre (ex: 3 minutos)
    wm.setConfigPortalTimeout(180);

    // Tenta conectar. Se falhar, abre o AP chamado "ESP32-Setup"
    if (!wm.autoConnect("ESP32-Setup")) {
        Serial.println("Falha ao conectar ou tempo esgotado. Reiniciando...");
        ESP.restart();
    }

    // 6. Se o usuário preencheu/alterou o campo no portal, salva na memória NVS
    if (strlen(custom_torno_id.getValue()) > 0) {
        strcpy(torno_id, custom_torno_id.getValue());
        
        preferences.begin("config", false);
        preferences.putString("torno_id", torno_id);
        preferences.end();
    }

    Serial.println("\nWi-Fi conectado com sucesso!");
    Serial.printf("\nID do Torno registrado: %s\n", torno_id);

    // 7. Sincronização de Tempo NTP (Executada apenas após o Wi-Fi estar ativo)
    Serial.println("Iniciando sincronização NTP...");
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Falha ao obter a hora via NTP");
    } else {
        Serial.println("Hora sincronizada com sucesso!");
        Serial.printf("Data/Hora atual: %02d/%02d/%04d %02d:%02d:%02d\n",
                      timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    checkForUpdates();
}

void loop() {
    // Configuração do cliente web
    client.setInsecure();

    // Leitura do pino com um Debounce simples
    bool currentPinState = (digitalRead(MOTOR_PIN_CLOCKWISE) || digitalRead(MOTOR_PIN_COUNTER_CLOCKWISE));
    if (currentPinState != lastPinState) {
        delay(50); // Filtro de ruído mecânico/elétrico
        currentPinState = (digitalRead(MOTOR_PIN_CLOCKWISE) || digitalRead(MOTOR_PIN_COUNTER_CLOCKWISE));
    }

    // --- MÁQUINA DE ESTADOS ---
    if (currentState == IDLE) {
        // Aguardando motor ligar pela primeira vez
        if (currentPinState == HIGH) {
            currentState = RUNNING;
            operationStartTime = time(NULL);
            lastStateChangeMillis = millis();
            
            // Prepara o buffer e zera contadores
            eventBuffer.clear();
            eventBuffer.push_back({"LIGADO", operationStartTime});
            totalTimeOn = 0;
            totalTimeOff = 0;
            
            Serial.println(">> Operação Iniciada.");
        }
    } 
    else if (currentState == RUNNING) {
        // Motor está rodando
        if (currentPinState == LOW) {
            currentState = PAUSED;
            time_t now = time(NULL);
            
            eventBuffer.push_back({"DESLIGADO", now});
            operationEndTime = now; // Salva como provável fim da operação
            
            // Acumula o tempo que ficou ligado
            totalTimeOn += (millis() - lastStateChangeMillis);
            lastStateChangeMillis = millis();

            Serial.println(">> Operação Interrompida.");
        }
    } 
    else if (currentState == PAUSED) {
        // Motor desligou. Avaliando se volta a ligar ou se dá timeout
        if (currentPinState == HIGH) {
            currentState = RUNNING;
            eventBuffer.push_back({"LIGADO", time(NULL)});
            
            // Acumula o tempo que ficou desligado neste intervalo
            totalTimeOff += (millis() - lastStateChangeMillis);
            lastStateChangeMillis = millis();

            Serial.println(">> Operação Retomada.");
        } 
        else {
            // Se continua desligado, verifica se passou 1 minuto
            if (millis() - lastStateChangeMillis >= TIMEOUT_OFF_MS) {
                currentState = IDLE; // Reseta para aguardar nova operação

                Serial.println(">> Operação Finalizada.");

                printFinalReport();  // Imprime, salva ou envia os dados
            }
        }
    }

    lastPinState = currentPinState;
    delay(100);
}
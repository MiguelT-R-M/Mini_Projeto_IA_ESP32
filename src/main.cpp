#define CAMERA_MODEL_ESP32S3_EYE

#include "camera_pins.h"
#include <Arduino.h>
#include <img_converters.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <WiFi.h>
#include <SPI.h>
#include <TFT_eSPI.h>

// lib de detecção facial
#include <human_face_detect_msr01.hpp>
#include "face_recognition_112_v1_s8.hpp"

FaceRecognition112V1S8 recognizer;

// ===========================
// SSID e senha do WIFI abaixo(Apenas 2.4G na ESP32-S3-N16R8)
// ===========================
const char *ssid = "";
const char *password = "";

TaskHandle_t aiTaskHandle = NULL;
httpd_handle_t server = NULL;

uint8_t *global_jpg_buf = nullptr;
size_t global_jpg_len = 0;
// "Semáforos" em hardware FreeRTOS
// O semáforo JPG previnem que o servidor http não utilize um buffer enquanto a AItask está usando ele, e vice versa
SemaphoreHandle_t jpgSemaphore = NULL;
// O semáforo REG previne que vários registros de nome/face acontecam ao mesmo tempo
SemaphoreHandle_t regSemaphore = NULL; 

// Inicia as classes de TFT_eSPI que serão usadas para desenhar os retângulos/nomes ascii no servidor http /stream
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite spr = TFT_eSprite(&tft);

char log_buffer[2048] = "=== ESP32 Log Buffer Iniciou! ===\n";
SemaphoreHandle_t logSemaphore = NULL;

// Função para logging
void webLog(const char* format, ...) {
    char temp[128];
    va_list args;
    va_start(args, format);
    vsnprintf(temp, sizeof(temp), format, args);
    va_end(args);

    // Print pro serial
    Serial.println(temp);

    // Save no logger web
    if (logSemaphore && (xSemaphoreTake(logSemaphore, pdMS_TO_TICKS(50)) == pdTRUE)) {
        // Concatenação/reset do buffer
        if (strlen(log_buffer) + strlen(temp) < sizeof(log_buffer) - 1) {
            strcat(log_buffer, "\n");
            strcat(log_buffer, temp);
        } else {
            // Reset para prevenir oveflow
            strcpy(log_buffer, "=== Buffer Reset ===\n");
            strcat(log_buffer, temp);
        }
        xSemaphoreGive(logSemaphore);
    }
}

// Handler de logs
static esp_err_t logs_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/plain");
    
    if (logSemaphore && (xSemaphoreTake(logSemaphore, portMAX_DELAY) == pdTRUE)) {
        httpd_resp_send(req, log_buffer, HTTPD_RESP_USE_STRLEN);
        xSemaphoreGive(logSemaphore);
    } else {
        httpd_resp_send(req, "Log buffer travado", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// Buffer de nome à ser registrado
char pending_registration_name[32] = "";

// Registrador de faces usando HTTP
static esp_err_t register_handler(httpd_req_t *req) {
    char query[128];
    char name[32] = "User";

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "name", name, sizeof(name)) == ESP_OK) {
            if (regSemaphore && (xSemaphoreTake(regSemaphore, portMAX_DELAY) == pdTRUE)) {
                strlcpy(pending_registration_name, name, sizeof(pending_registration_name));
                xSemaphoreGive(regSemaphore);
            }
        }
    }

    httpd_resp_send(req, "Nome Enviado! Diga Xis!", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler da stream HTTP
static esp_err_t stream_handler(httpd_req_t *req) {
    char part_buf[64];
    esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=123456789000000000000987654321");
    if (res != ESP_OK) return res;

    while (true) {
        uint8_t *local_jpg = nullptr;
        size_t local_len = 0;

        if (xSemaphoreTake(jpgSemaphore, portMAX_DELAY) == pdTRUE) {
            if (global_jpg_buf && global_jpg_len > 0) {
                local_len = global_jpg_len;
                local_jpg = (uint8_t *)malloc(local_len);
                if (local_jpg) {
                    memcpy(local_jpg, global_jpg_buf, local_len);
                }
            }
            xSemaphoreGive(jpgSemaphore);
        }

        if (!local_jpg) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int hlen = snprintf(part_buf, sizeof(part_buf), "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", local_len);
        if (httpd_resp_send_chunk(req, part_buf, hlen) != ESP_OK) {
            free(local_jpg);
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)local_jpg, local_len) != ESP_OK) {
            free(local_jpg);
            break;
        }
        if (httpd_resp_send_chunk(req, "\r\n--123456789000000000000987654321\r\n", strlen("\r\n--123456789000000000000987654321\r\n")) != ESP_OK) {
            free(local_jpg);
            break;
        }
        free(local_jpg);

        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return ESP_OK;
}

void startMyServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    //Struct que define o Http local de stream
    httpd_uri_t stream_uri = {
        .uri      = "/stream",
        .method   = HTTP_GET,
        .handler  = stream_handler,
        .user_ctx = NULL
    };

    //Struct que define o Http local de registro
    httpd_uri_t register_uri = {
        .uri      = "/register",
        .method   = HTTP_GET,
        .handler  = register_handler,
        .user_ctx = NULL
    };

    //struct que define o http local de logging
    httpd_uri_t logger = {
        .uri      = "/logs",
        .method   = HTTP_GET,
        .handler  = logs_handler,
        .user_ctx = NULL
    };
    
    // Começa o server e manda o IP pelo Serial
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &stream_uri);
        httpd_register_uri_handler(server, &register_uri);
        httpd_register_uri_handler(server, &logger);
        Serial.print("Server iniciado no port 80 ip: http://");
        Serial.print(WiFi.localIP());
        Serial.println(" (/stream e /register e /logs)");

    }
}

// Função aitask, usando o Human_face_detect_msr01.hpp
void aiTask(void *pvParameters) {
    vTaskDelay(pdMS_TO_TICKS(3000));

    HumanFaceDetectMSR01 detector(0.12F, 0.2F, 4, 0.2F); // O mais balanceado que encontrei
    recognizer.set_thresh(0.65f);

    webLog("AI Task funcionando e usando TFT_eSPI para renderizar sprites...");

    int frame_counter = 0;
    String cached_displayName = "Desconhecido";

    while (1) {
        camera_fb_t *fb = esp_camera_fb_get();
        
        if (fb != nullptr) {
            if (fb->format == PIXFORMAT_RGB565) {
                
                memcpy(spr.frameBuffer(0), fb->buf, fb->width * fb->height * 2);

                std::vector<int> shape = {(int)fb->height, (int)fb->width, 3};
                std::list<dl::detect::result_t> results = detector.infer((uint16_t *)fb->buf, shape);

                if (!results.empty()) {

                    char current_target[32] = "";
                    if (xSemaphoreTake(regSemaphore, portMAX_DELAY) == pdTRUE) {
                        if (strlen(pending_registration_name) > 0) {
                            strlcpy(current_target, pending_registration_name, sizeof(current_target));
                        }
                        xSemaphoreGive(regSemaphore);
                    }

                    frame_counter++;
                    bool should_recognize = (frame_counter % 3 == 0) || (strlen(current_target) > 0);

                    for (const auto &res : results) {
                        int x1 = res.box[0];
                        int y1 = res.box[1];
                        int x2 = res.box[2];
                        int y2 = res.box[3];

                        // Keypoints sintéticos para prevenir crashes
                        int w = x2 - x1;
                        int h = y2 - y1;
                        std::vector<int> mutable_keypoints = {
                            x1 + (int)(w * 0.35f), y1 + (int)(h * 0.35f),
                            x1 + (int)(w * 0.65f), y1 + (int)(h * 0.35f),
                            x1 + (int)(w * 0.50f), y1 + (int)(h * 0.55f),
                            x1 + (int)(w * 0.38f), y1 + (int)(h * 0.75f),
                            x1 + (int)(w * 0.62f), y1 + (int)(h * 0.75f)
                        };

                        // Registro via Web
                        if (strlen(current_target) > 0) {
                            int id = recognizer.enroll_id((uint16_t *)fb->buf, shape, mutable_keypoints, current_target, false);
                            if (id >= 0) {
                                webLog("Successfully enrolled %s with ID %d\n", current_target, id);
                                if (xSemaphoreTake(regSemaphore, portMAX_DELAY) == pdTRUE) {
                                    memset(pending_registration_name, 0, sizeof(pending_registration_name));
                                    xSemaphoreGive(regSemaphore);
                                }
                                cached_displayName = String(current_target);
                                current_target[0] = '\0';
                            } else {
                                webLog("Enrollment failed!\n");
                            }
                        }
                        
                        // Reconhecimento facial (Executado apenas no intervalo definido)
                        if (should_recognize && strlen(current_target) == 0) {
                            face_info_t match = recognizer.recognize((uint16_t *)fb->buf, shape, mutable_keypoints);
                            if (match.similarity >= recognizer.get_thresh()) {
                                cached_displayName = String(match.name.c_str());
                            } else {
                                cached_displayName = "Desconhecido";
                            }
                        }

                        // Renderização gráfica usa o cached_displayName para evitar flickering
                        spr.drawRect(x1, y1, x2 - x1, y2 - y1, 0x07E0);
                        spr.drawRect(x1 + 1, y1 + 1, (x2 - x1) - 2, (y2 - y1) - 2, 0x07E0);

                        spr.setTextColor(0x07E0, 0x0000);
                        spr.setTextSize(1);
                        
                        char labelBuffer[32];
                        char nomeBuffer[32];
                        snprintf(labelBuffer, sizeof(labelBuffer), "Score: %.2f", res.score);
                        snprintf(nomeBuffer, sizeof(nomeBuffer), "Nome: %s", cached_displayName.c_str());

                        spr.drawString(labelBuffer, x1, max(0, y1 - 16), 2);
                        spr.drawString(nomeBuffer, x1, y2, 2);
                    }
                }

                memcpy(fb->buf, spr.frameBuffer(0), fb->width * fb->height * 2);

                uint8_t *jpg_buf = nullptr;
                size_t jpg_len = 0;
                if (frame2jpg(fb, 80, &jpg_buf, &jpg_len)) {
                    if (xSemaphoreTake(jpgSemaphore, portMAX_DELAY) == pdTRUE) {
                        if (global_jpg_buf) free(global_jpg_buf);
                        global_jpg_buf = jpg_buf;
                        global_jpg_len = jpg_len;
                        xSemaphoreGive(jpgSemaphore);
                    } else {
                        free(jpg_buf);
                    }
                }
            }
            esp_camera_fb_return(fb);
        }

        vTaskDelay(pdMS_TO_TICKS(60));
    }
}

void setup() {
    delay(2000);
    Serial.begin(115200);
    Serial.setDebugOutput(true);
    webLog("");

    jpgSemaphore = xSemaphoreCreateMutex();
    regSemaphore = xSemaphoreCreateMutex();
    logSemaphore = xSemaphoreCreateMutex();

    spr.setColorDepth(16);
    spr.createSprite(320, 240);

    // Setup da camera OV5640 no ESP32-S3-N16R8
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.frame_size = FRAMESIZE_QVGA;     
    config.pixel_format = PIXFORMAT_RGB565; 
    config.grab_mode = CAMERA_GRAB_LATEST;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 3;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
    webLog("Inicializacao de camera falhou com erro: 0x%x", err);
    return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, -2);
    }

    #if defined(CAMERA_MODEL_ESP32S3_EYE)
    s->set_vflip(s, 1);
    #endif

    WiFi.begin(ssid, password);
    WiFi.setSleep(false);

    webLog("WiFi connectando");
    while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    webLog(".");
    }
    webLog("\nWiFi connectado!");

    startMyServer();

    xTaskCreatePinnedToCore(
        aiTask,         
        "AI_Task",      
        8192,           
        NULL,           
        2,              
        &aiTaskHandle,  
        1               
    );
}

void loop() {
    delay(10000);   
}
#include <DHT.h>
#include <MsTimer2.h>
#include <MQ135.h>
#include <SoftwareSerial.h>
#include <WiFiEsp.h>

// 매크로 정의 //

#define DEBUG 1

#define SERIAL_BAUDRATE 38400
#define AP_SSID "iot0"
#define AP_PASSWORD "iot00000"
#define SERVER_IP "10.10.141.61"
#define SERVER_PORT 5000
#define CLIENT_ID "MP_SENSOR"
#define CLIENT_PASSWORD "PASSWD"

#define WATER_LEVEL_PIN A0
#define MQ135_PIN A1
#define DHT_PIN 4
#define RX_PIN 6
#define TX_PIN 7

#define DHT_TYPE DHT11
#define MAX_ARRAY_SIZE 8
#define MAX_COMMAND_LENGTH 60


// 전역 변수 //

char client_id[10] = "MP_SENSOR";
char master_id[10] = "MP_MASTER";
char tx_buffer[MAX_COMMAND_LENGTH] = {0};   // 전송 버퍼
bool is_wifi_init = false;                  // WiFi 초기화 여부

SoftwareSerial sw_serial(RX_PIN, TX_PIN);   // 아두이노 소프트웨어 RX, TX
WiFiEspClient client;                       // WiFi 클라이언트
DHT dht(DHT_PIN, DHT_TYPE);                 // DHT11 센서
MQ135 mq135(MQ135_PIN);                     // MQ135 센서

float humidity = 0;                         // 습도
float temperature = 0;                      // 온도
int water_level = 0;                        // 수위
float corrected_ppm = 0;                    // 대기질

unsigned long time_passed = 0;              // 경과 시간 (초)
int sensor_read_interval = 5;               // 센서 읽기 간격 (초)
bool timer_flag = false;                    // 타이머 플래그


// 함수 선언 //

void setup();
void loop();
void setup_wifi();
void init_wifi_module();
void connect_to_server();
void socket_event();
void read_sensor();
void on_timer_interrupt();


// 함수 정의 //

/**
 * 설정 함수
 */
void setup()
{
#ifdef DEBUG
    Serial.begin(115200);
    Serial.println("(setup) Program started");
#endif
    setup_wifi();
    MsTimer2::set(1000, on_timer_interrupt);
    MsTimer2::start();
    dht.begin();
}

/**
 * 루프 함수
 */
void loop()
{
    // Bluetooth 동작 수행
    if (client.available()) {
        socket_event();
    }

    // 타이머 동작 수행
    if (timer_flag) {
        timer_flag = false;

        // 5초마다 서버 연결 시도
        if (time_passed % 5 == 0) {
            if (!client.connected()) {
                connect_to_server();
            }
        }
        // 센서 읽기
        if (client.connected() && (time_passed % sensor_read_interval == 0)) {
            read_sensor();
            sprintf(tx_buffer, "[%s]SENSOR@%d@%d@%d@%d\n", master_id, (int)humidity, (int)temperature, water_level, (int)corrected_ppm);
            client.write(tx_buffer, strlen(tx_buffer));
            client.flush();
#ifdef DEBUG
            Serial.print("(loop) Transmit: ");
            Serial.print(tx_buffer);
#endif    
        }
    }

//     // 타이머 동작 수행
// 	if (timer_flag) {
//         timer_flag = false;

//         // 센서 읽기
//         if (time_passed % sensor_read_interval == 0) {
//             read_sensor();
//             sprintf(tx_buffer, "[%s]SENSOR@%d@%d@%d@%d\n", master_id, (int)humidity, (int)temperature, water_level, (int)corrected_ppm);
//             client.write(tx_buffer, strlen(tx_buffer));
//             client.flush();
// #ifdef DEBUG
//             Serial.print("(loop) Transmit: ");
//             Serial.print(tx_buffer);
// #endif    
//         }
//    }
}

/**
 * WiFi 설정 함수
 */
void setup_wifi()
{
    sw_serial.begin(SERIAL_BAUDRATE);
    init_wifi_module();
    // connect_to_server();
}

/**
 * WiFi 모듈 초기화 함수
 */
void init_wifi_module()
{
    // WiFi 모듈과 시리얼 연결
    do {
        WiFi.init(&sw_serial);
        // 연결된 모듈이 없을 경우
        if (WiFi.status() == WL_NO_SHIELD) {
#ifdef DEBUG
            Serial.println("(init_wifi) WiFi shield not present");
#endif
            delay(1000);
        }
        else {
            break;
        }
    } while (1);

    // 무선 AP에 연결
    while (WiFi.begin(AP_SSID, AP_PASSWORD) != WL_CONNECTED) {
#ifdef DEBUG
        Serial.print("(init_wifi) Attempting to connect to WPA SSID: ");
        Serial.println(AP_SSID);
#endif
    }
    is_wifi_init = true;

    // 연결 성공
#ifdef DEBUG
    Serial.println("(init_wifi) You're connected to the network");
    Serial.print("(init_wifi) IP Address: ");
    Serial.print(WiFi.localIP());
    Serial.print(", Signal strength (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
#endif
}

/**
 * 서버 연결 함수
 */
void connect_to_server()
{
    if (!is_wifi_init) {
        return;
    }

    if (client.connect(SERVER_IP, SERVER_PORT)) {
#ifdef DEBUG
        Serial.println("(connect_to_server) Connected to server");
#endif
        client.print("[" CLIENT_ID ":" CLIENT_PASSWORD "]");
    }
    else {
#ifdef DEBUG
        Serial.println("(connect_to_server) Connection failed");
#endif
    }
}

/**
 * 소켓 이벤트 함수
 */
void socket_event()
{
    int i = 0;
    char *pToken;
    char *pArray[MAX_ARRAY_SIZE] = {0};
    char rx_buffer[MAX_COMMAND_LENGTH] = {0};
    int rx_length;

    tx_buffer[0] = '\0';
    rx_length = client.readBytesUntil('\n', rx_buffer, MAX_COMMAND_LENGTH);
    client.flush();
#ifdef DEBUG
    Serial.print("(socket_event) Received: ");
    Serial.println(rx_buffer);
#endif

    // 명령어 파싱
    pToken = strtok(rx_buffer, "[@]");
    while (pToken != NULL) {
        pArray[i] = pToken;
        i += 1;
        if (i >= MAX_ARRAY_SIZE) {
            break;
        }
        pToken = strtok(NULL, "[@]");
    }

    // 명령어 처리
    // [0]: ID, [1]: 명령어, [2~]: 값
    if (!strcmp(pArray[1], "SET")) {
        // 센서 읽기 간격 설정
        if (!strcmp(pArray[2], "INTERVAL")) {
            int value = atoi(pArray[3]);
            if (value < 1) {
                sprintf(tx_buffer, "[%s]interval must be greater than 0\n", pArray[0]);
            }
            else {
                sensor_read_interval = value;
                sprintf(tx_buffer, "[%s]interval changed to %d\n", pArray[0], sensor_read_interval);
            }
        }
    }
    else if (!strcmp(pArray[1], "GET")) {
        if (!strcmp(pArray[2], "INTERVAL")) {
            sprintf(tx_buffer, "[%s]interval is %d\n", pArray[0], sensor_read_interval);
        }
    }
    else if (!strcmp(pArray[1], "New")) {
        // do nothing
    }
    else {
        // do nothing
    }
    
    client.write(tx_buffer, strlen(tx_buffer));
    client.flush();
}

void send_event()
{

}

/**
 * 센서 값 읽기
 */
void read_sensor()
{
    humidity = dht.readHumidity();
    temperature = dht.readTemperature();
    water_level = analogRead(WATER_LEVEL_PIN);
    corrected_ppm = mq135.getCorrectedPPM(temperature, humidity);
}

/**
 * 타이머 인터럽트 함수
 */
void on_timer_interrupt()
{
    time_passed += 1;
    timer_flag = true;
}
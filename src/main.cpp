#include <SPI.h>

#include <RF24.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <BMI088.h>
#include <BMP388_DEV.h>
#include <LoRa.h>
#include <TinyGPS++.h>

#include "IOSdcard.h"

// PINS
#define SDCARDCS 33

SPIClass spi = SPIClass(VSPI);
// Handle tasks
TaskHandle_t AccelGyroTask;
TaskHandle_t PressureTask;
TaskHandle_t FlusherTask;
TaskHandle_t GPSTask;

// Semaphores
SemaphoreHandle_t SDWriteSemaphore = xSemaphoreCreateMutex();
SemaphoreHandle_t I2CSemaphore = xSemaphoreCreateMutex();

// Files
File pressureFile;
File accelGyroFile;
File gpsFile;

// Buffer size for data
#define BUFFERSIZE 5

// Hold Pressure data
struct PressureStuct
{
    float pressure;
    float temperature;
    float altitude;
    unsigned long time;
};

struct GPSStruct
{
    double lat, lng;
    uint32_t date;
    uint32_t time;
    double speed;
    double course;
    double altitude;
    int satellites;
    int hdop;
    unsigned long systemTime;
};

struct AccelGyroStruct
{
    float ax;
    float ay;
    float az;
    float gx;
    float gy;
    float gz;
    unsigned long time;
};

struct Packet
{
    uint32_t seq_no;
    uint32_t time_ms;
    double gps_lat, gps_lng, gps_alt;
    float bmp_alt, bmp_temp;
    float acc_x, acc_y, acc_z;
    float gyr_x, gyr_y, gyr_z;
};

QueueHandle_t AccelQueue = xQueueCreate(BUFFERSIZE, sizeof(AccelGyroStruct));
QueueHandle_t PressureQueue = xQueueCreate(BUFFERSIZE, sizeof(PressureStuct));
QueueHandle_t GPSQueue = xQueueCreate(BUFFERSIZE, sizeof(GPSStruct));

// Devices
BMP388_DEV bmp388;
SPIClass *hspi;
RF24 radio(26, 15);

void Pressure(void *pvParameters)
{
    if(bmp388.begin())
        Serial.println("BMP388 is online");
    else
        Serial.println("BMP388 is not offline");

    // Default initialisation, place the BMP388 into SLEEP_MODE
    bmp388.setTimeStandby(TIME_STANDBY_160MS); // Set the standby time to 1280ms
    bmp388.startNormalConversion();            // Start NORMAL conversion mode
    float temperature, pressure, altitude;
    uint8_t initial = bmp388.getMeasurements(temperature, pressure, altitude);

    while(!initial)
        initial = bmp388.getMeasurements(temperature, pressure, altitude);

    bmp388.setSeaLevelPressure(pressure);

    PressureStuct pressureStruct;
    while(1)
    {
        if(bmp388.getMeasurements(temperature, pressure, altitude))  // Check if the measurement is complete
        {
            // PressureStuct pressureStruct;
            pressureStruct.pressure = pressure;
            pressureStruct.temperature = temperature;
            pressureStruct.altitude = altitude;
            pressureStruct.time = millis();
            xQueueSend(PressureQueue, &pressureStruct, 0);
            Serial.println();
        }

        delay(100);
    }
}

void AccelGyro(void *pvParameters)
{
    //   delay(1000);
    if(bmi088.isConnection())
    {
        bmi088.initialize();
        Serial.println("BMI088 connected");
    }
    else
    {
        Serial.println("BMI088 not connected");
        while (1)
        {
        }
    }
    AccelGyroStruct accelGyro;
    while(1)
    {
        float x, y, z = 0;
        // Serial.println("Getting accel data");
        bmi088.getAcceleration(&x, &y, &z);
        // printf("Acceleration: %f, %f, %f\n", x, y, z);
        float xg, yg, zg = 0;
        bmi088.getGyroscope(&xg, &yg, &zg);
        // printf("Gyroscope: %f, %f, %f\n", xg, yg, zg);
        // AccelGyroStruct accelGyro;
        accelGyro.ax = x;
        accelGyro.ay = y;
        accelGyro.az = z;
        accelGyro.gx = xg;
        accelGyro.gy = yg;
        accelGyro.gz = zg;
        accelGyro.time = millis();
        xQueueSend(AccelQueue, &accelGyro, 0);

        delay(100);
    }
}

void GPS(void *pvParameters)
{
    TinyGPSPlus gps;

    while(1)
    {
        GPSStruct gpsStruct = {};

        while(Serial.available() > 0)
        {
            char byte = Serial.read();
            Serial.print(byte);
            gps.encode(byte);
        }

        if(gps.location.isUpdated())
        {
            if(gps.location.isValid())
            {
                gpsStruct.lat = gps.location.lat();
                gpsStruct.lng = gps.location.lng();
            }

            if(gps.date.isValid())
                gpsStruct.date = gps.date.value();

            if(gps.time.isValid())
                gpsStruct.time = gps.time.value();

            if(gps.altitude.isValid())
                gpsStruct.altitude = gps.altitude.meters();

            if(gps.speed.isValid())
                gpsStruct.speed = gps.speed.mps();

            if(gps.course.isValid())
                gpsStruct.course = gps.course.deg();

            gpsStruct.systemTime = millis();

            xQueueSend(GPSQueue, &gpsStruct, 0);
        }

        delay(100);
    }
}

void setup()
{

    Serial.begin(9600);

    Wire.begin(21, 22);

    pinMode(19, INPUT_PULLUP);
    spi.begin(18, 19, 23, SDCARDCS);

    if(!SD.begin(SDCARDCS, spi))
    {
        Serial.println("initialization failed!\n");
        return;
    }
    // TODO:
    // Do threads need to be added if the components don't initialise?

    // Open files
    xSemaphoreTake(SDWriteSemaphore, portMAX_DELAY);
    pressureFile = SD.open("/pressure.txt", FILE_WRITE);
    accelGyroFile = SD.open("/accelGyro.txt", FILE_WRITE);
    gpsFile = SD.open("/gps.txt", FILE_WRITE);
    xSemaphoreGive(SDWriteSemaphore);

    xTaskCreatePinnedToCore(AccelGyro, "Accel", 40000, NULL, 1, &AccelGyroTask, 0);
    xTaskCreatePinnedToCore(Pressure, "pressure", 20000, NULL, 1, &PressureTask, 0);
    xTaskCreatePinnedToCore(GPS, "GPS", 20000, NULL, 1, &GPSTask, 1);
}

int counter = 0;

void loop()
{

    if(uxQueueMessagesWaiting(AccelQueue) > 0)
    {
        AccelGyroStruct accelGyro;

        xQueueReceive(AccelQueue, &accelGyro, portMAX_DELAY);

        int returnSize = snprintf(NULL, 0, "%f,%f,%f,%f,%f,%f,%lu\n", accelGyro.ax, accelGyro.ay, accelGyro.az, accelGyro.gx, accelGyro.gy, accelGyro.gz, accelGyro.time);

        char *buffer = (char *)malloc(returnSize + 1);

        snprintf(buffer, returnSize + 1, "%f,%f,%f,%f,%f,%f,%lu\n", accelGyro.ax, accelGyro.ay, accelGyro.az, accelGyro.gx, accelGyro.gy, accelGyro.gz, accelGyro.time);

        accelGyroFile.print(buffer);
        counter++;
        free(buffer);
    }

    if(uxQueueMessagesWaiting(PressureQueue) > 0)
    {
        PressureStuct pressure;

        xQueueReceive(PressureQueue, &pressure, portMAX_DELAY);

        int returnSize = snprintf(NULL, 0, "%f,%f,%f,%lu\n", pressure.pressure, pressure.temperature, pressure.altitude, pressure.time);

        char *buffer = (char *)malloc(returnSize + 1);

        snprintf(buffer, returnSize + 1, "%f,%f,%f,%lu\n", pressure.pressure, pressure.temperature, pressure.altitude, pressure.time);

        pressureFile.print(buffer);
        counter++;
        free(buffer);
    }

    if(uxQueueMessagesWaiting(GPSQueue) > 0)
    {
        GPSStruct gps;

        xQueueReceive(GPSQueue, &gps, portMAX_DELAY);

        int returnSize = snprintf(NULL, 0, "%f,%f,%f,%f,%f,%u,%u,%lu\n", gps.lat, gps.lng, gps.altitude, gps.speed, gps.course, gps.date, gps.time, gps.systemTime);

        char *buffer = (char *)malloc(returnSize + 1);

        snprintf(buffer, returnSize + 1, "%f,%f,%f,%f,%f,%u,%u,%lu\n", gps.lat, gps.lng, gps.altitude, gps.speed, gps.course, gps.date, gps.time, gps.systemTime);

        gpsFile.print(buffer);
        counter++;
        free(buffer);
    }

    if(counter > 10)
    {
        counter = 0;
        accelGyroFile.flush();
        pressureFile.flush();
        gpsFile.flush();
    }

    delay(1);
}

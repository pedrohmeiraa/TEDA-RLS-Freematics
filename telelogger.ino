/******************************************************************************
* Arduino sketch of a vehicle data data logger and telemeter for Freematics Hub
* Works with Freematics ONE+ Model A and Model B
* Developed by Stanley Huang <stanley@freematics.com.au>
* Distributed under BSD license
* Visit https://freematics.com/products for hardware information
* Visit https://hub.freematics.com to view live and history telemetry data
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
******************************************************************************/

#include <FreematicsPlus.h>
#include <RLSFilter.h>
#include <httpd.h>
#include "config.h"
// #include <stdio.h>
#include <sys/time.h>
#include "telestore.h"
#include "teleclient.h"
#include "telemesh.h"
#if BOARD_HAS_PSRAM
#include "esp32/himem.h"
#endif
#include "driver/adc.h"
#if ENABLE_OLED
#include "FreematicsOLED.h"
#endif

// TEDARLS
const int N = 2;
const float lambda = 0.99;
const float delta = 0.1;

byte byte_y_pred = 0x1B0; // Defina um novo PID para o valor predito (por exemplo, 10E)
byte byte_x_ant = 0x1B1;
byte byte_x_atual = 0x1B2;
byte byte_flag = 0x1B3;
byte byte_x_antp = 0x1B4;
byte byte_x_atualp = 0x1B5;
byte time_flag = 0x1B6;
byte byte_y_predp = 0x1B7;
byte time_teda_flag = 0x1B8;
byte time_rls_filter_1_flag = 0x1B9;
byte time_rls_filter_2_flag = 0x1BA;
byte time_rls_update_flag = 0x1BB;


float Y[20] = {};
float vetor_de_entrada[N] = {0.0, 0.0};
float x_ant[N] = {0.0, 0.0};
float y_pred = 0;
int flag;
// int valor_atual = 0;

RLSFilter filter(lambda, delta);
TEDA teda(2.0);

int count = 0;

// states
#define STATE_STORAGE_READY 0x1
#define STATE_OBD_READY 0x2
#define STATE_GPS_READY 0x4
#define STATE_MEMS_READY 0x8
#define STATE_NET_READY 0x10
#define STATE_CELL_CONNECTED 0x20
#define STATE_WIFI_CONNECTED 0x40
#define STATE_WORKING 0x80
#define STATE_STANDBY 0x100

typedef struct {
  byte pid;
  byte tier;
  int value;
  uint32_t ts;
} PID_POLLING_INFO;

PID_POLLING_INFO obdData[]= {
  {PID_FUEL_LEVEL, 1},
  {PID_SPEED, 1},
  {PID_RPM, 1},
  {PID_THROTTLE, 1},
  {PID_ENGINE_LOAD, 1},
  {PID_FUEL_PRESSURE, 1},
  {PID_TIMING_ADVANCE, 1},
  {PID_COOLANT_TEMP, 1},
  {PID_INTAKE_TEMP, 1},
};

CBufferManager bufman;
Task subtask;

#if ENABLE_MEMS
float accBias[3] = {0}; // calibrated reference accelerometer data
float accSum[3] = {0};
float acc[3] = {0};
float gyr[3] = {0};
float mag[3] = {0};
uint8_t accCount = 0;
#endif
int deviceTemp = 0;

// live data
int16_t rssi = 0;
int16_t rssiLast = 0;
char vin[18] = {0};
uint16_t dtc[6] = {0};
float batteryVoltage = 0;
GPS_DATA* gd = 0;

char devid[12] = {0};
char isoTime[32] = {0};

// stats data
uint32_t lastMotionTime = 0;
uint32_t timeoutsOBD = 0;
uint32_t timeoutsNet = 0;
uint32_t lastStatsTime = 0;

int32_t syncInterval = SERVER_SYNC_INTERVAL * 1000;
int32_t dataInterval = 1000;

#if STORAGE != STORAGE_NONE
int fileid = 0;
uint16_t lastSizeKB = 0;
#endif

uint32_t lastCmdToken = 0;
String serialCommand;

byte ledMode = 0;
int outlier_count = 0;
int N_outlier_max = 2;
bool correction = true;

bool serverSetup(IPAddress& ip);
void serverProcess(int timeout);
void processMEMS(CBuffer* buffer);
bool processGPS(CBuffer* buffer);
void processBLE(int timeout);

class State {
public:
  bool check(uint16_t flags) { return (m_state & flags) == flags; }
  void set(uint16_t flags) { m_state |= flags; }
  void clear(uint16_t flags) { m_state &= ~flags; }
  uint16_t m_state = 0;
};

FreematicsESP32 sys;

class OBD : public COBD
{
protected:
  void idleTasks()
  {
    // do some quick tasks while waiting for OBD response
#if ENABLE_MEMS
    processMEMS(0);
#endif
    delay(5);
  }
};

OBD obd;

MEMS_I2C* mems = 0;

#if STORAGE == STORAGE_SPIFFS
SPIFFSLogger logger;
#elif STORAGE == STORAGE_SD
SDLogger logger;
#endif

#if SERVER_PROTOCOL == PROTOCOL_UDP
TeleClientUDP teleClient;
#else
TeleClientHTTP teleClient;
#endif

#if ENABLE_OLED
OLED_SH1106 oled;
#endif

State state;

void printTimeoutStats()
{
  Serial.print("Timeouts: OBD:");
  Serial.print(timeoutsOBD);
  Serial.print(" Network:");
  Serial.println(timeoutsNet);
}

#if LOG_EXT_SENSORS
void processExtInputs(CBuffer* buffer)
{
#if LOG_EXT_SENSORS == 1
  uint8_t levels[2] = {(uint8_t)digitalRead(PIN_SENSOR1), (uint8_t)digitalRead(PIN_SENSOR2)};
  buffer->add(PID_EXT_SENSORS, ELEMENT_UINT8, levels, sizeof(levels), 2);
#elif LOG_EXT_SENSORS == 2
  uint16_t reading[] = {adc1_get_raw(ADC1_CHANNEL_0), adc1_get_raw(ADC1_CHANNEL_1)};
  Serial.print("GPIO0:");
  Serial.print((float)reading[0] * 3.15 / 4095 - 0.01);
  Serial.print(" GPIO1:");
  Serial.println((float)reading[1] * 3.15 / 4095 - 0.01);
  buffer->add(PID_EXT_SENSORS, ELEMENT_UINT16, reading, sizeof(reading), 2);
#endif
}
#endif

/*******************************************************************************
  HTTP API
*******************************************************************************/
#if ENABLE_HTTPD
int handlerLiveData(UrlHandlerParam* param)
{
    char *buf = param->pucBuffer;
    int bufsize = param->bufSize;
    int n = snprintf(buf, bufsize, "{\"obd\":{\"vin\":\"%s\",\"battery\":%.1f,\"pid\":[", vin, batteryVoltage);
    uint32_t t = millis();
    for (int i = 0; i < sizeof(obdData) / sizeof(obdData[0]); i++) {
        n += snprintf(buf + n, bufsize - n, "{\"pid\":%u,\"value\":%d,\"age\":%u},",
            0x100 | obdData[i].pid, obdData[i].value, (unsigned int)(t - obdData[i].ts));
    }
    n--;
    n += snprintf(buf + n, bufsize - n, "]}");
#if ENABLE_MEMS
    if (accCount) {
      n += snprintf(buf + n, bufsize - n, ",\"mems\":{\"acc\":[%d,%d,%d],\"stationary\":%u}",
          (int)((accSum[0] / accCount - accBias[0]) * 100), (int)((accSum[1] / accCount - accBias[1]) * 100), (int)((accSum[2] / accCount - accBias[2]) * 100),
          (unsigned int)(millis() - lastMotionTime));
    }
#endif
    if (gd && gd->ts) {
      n += snprintf(buf + n, bufsize - n, ",\"gps\":{\"utc\":\"%s\",\"lat\":%f,\"lng\":%f,\"alt\":%f,\"speed\":%f,\"sat\":%d,\"age\":%u}",
          isoTime, gd->lat, gd->lng, gd->alt, gd->speed, (int)gd->sat, (unsigned int)(millis() - gd->ts));
    }
    buf[n++] = '}';
    param->contentLength = n;
    param->contentType=HTTPFILETYPE_JSON;
    return FLAG_DATA_RAW;
}
#endif

/*******************************************************************************
  Reading and processing OBD data
*******************************************************************************/
#if ENABLE_OBD
void processOBD(CBuffer* buffer)
{
  static int idx[2] = {0, 0};
  int tier = 1;
  for (byte i = 0; i < sizeof(obdData) / sizeof(obdData[0]); i++) {
    if (obdData[i].tier > tier) {
        // reset previous tier index
        idx[tier - 2] = 0;
        // keep new tier number
        tier = obdData[i].tier;
        // move up current tier index
        i += idx[tier - 2]++;
        // check if into next tier
        if (obdData[i].tier != tier) {
            idx[tier - 2]= 0;
            i--;
            continue;
        }
    }
    byte pid = obdData[i].pid;
    if (!obd.isValidPID(pid)) continue;
    int value;
    float valuef;
    
    if (obd.readPID(pid, value)) {

        obdData[i].ts = millis();
        obdData[i].value = value;

        // if pid is speed
        if (pid == PID_SPEED) {
          struct timeval start_time, end_time;
          struct timeval start_time_teda, end_time_teda;
          struct timeval start_time_rls_filter_1, end_time_rls_filter_1;
          struct timeval start_time_rls_filter_2, end_time_rls_filter_2;
          struct timeval start_time_rls_update, end_time_rls_update;

          gettimeofday(&start_time, NULL);
          valuef = (float)value;
        
          // if (count < 2){
          //     count++;

          //     flag = teda.run(valuef);
              
          //     if (count == 1){
          //         x_ant[0] = valuef;
          //         x_ant[1] = valuef;
          //     }

          //     buffer->add((uint16_t)byte_x_ant | 0x100, ELEMENT_FLOAT, &x_ant[1], sizeof(x_ant[1]));

          //     if (count == 2){
          //         vetor_de_entrada[0] = x_ant[1];
          //         vetor_de_entrada[1] = valuef;
    
          //         y_pred = filter.filter(vetor_de_entrada);

          //         // Atualiza os pesos do filtro
          //         filter.update(valuef, x_ant);

          //         x_ant[0] = x_ant[1];
          //         x_ant[1] = valuef;
          //         y_pred = valuef;
          //     }

          //     buffer->add((uint16_t)pid | 0x100, ELEMENT_INT32, &value, sizeof(value));
          //     buffer->add((uint16_t)byte_flag | 0x100, ELEMENT_INT32, &flag, sizeof(flag));
          //     buffer->add((uint16_t)byte_x_antp | 0x100, ELEMENT_FLOAT, &x_ant[1], sizeof(x_ant[1]));
          //     buffer->add((uint16_t)byte_x_atualp | 0x100, ELEMENT_FLOAT, &valuef, sizeof(valuef));
          //     buffer->add((uint16_t)byte_y_pred | 0x100, ELEMENT_FLOAT, &y_pred, sizeof(y_pred));
          //     buffer->add((uint16_t)byte_y_predp | 0x100, ELEMENT_INT32, &value, sizeof(value));

          // } else {

              gettimeofday(&start_time_teda, NULL);
              flag = teda.run(valuef);
              gettimeofday(&end_time_teda, NULL);

              buffer->add((uint16_t)byte_x_ant | 0x100, ELEMENT_FLOAT, &x_ant[1], sizeof(x_ant[1]));
              buffer->add((uint16_t)byte_x_atual | 0x100, ELEMENT_FLOAT, &valuef, sizeof(valuef));
              // buffer->add((uint16_t)pid | 0x100, ELEMENT_INT32, &value, sizeof(value));

              if (flag == 1){
                  outlier_count++;
                  valuef = y_pred;
              } else {
                outlier_count = 0;
              }

              if (correction == true && outlier_count == N_outlier_max + 1)
              {
                  valuef = value;
              }

              vetor_de_entrada[0] = x_ant[1];
              vetor_de_entrada[1] = valuef;     

              Serial.print("x_ant: ");
              Serial.print(x_ant[0]);
              Serial.print(" ");
              Serial.println(x_ant[1]);

              Serial.print("vetor_de_entrada: ");
              Serial.print(vetor_de_entrada[0]);
              Serial.print(" ");
              Serial.println(vetor_de_entrada[1]);

              gettimeofday(&start_time_rls_filter_1, NULL);
              y_pred = filter.filter(vetor_de_entrada);
              gettimeofday(&end_time_rls_filter_1, NULL);

              Serial.print("y_pred: ");
              Serial.println(y_pred);

              gettimeofday(&start_time_rls_update, NULL);
              // Atualiza os pesos do filtro
              filter.update(valuef, x_ant);
              gettimeofday(&end_time_rls_update, NULL);

              vetor_de_entrada[0] = x_ant[1];
              vetor_de_entrada[1] = valuef;

              gettimeofday(&start_time_rls_filter_2, NULL);
              // predicting
              y_pred = filter.filter(vetor_de_entrada);
              gettimeofday(&end_time_rls_filter_2, NULL);

              // Second stage
              if (flag == 1){
                  valuef = y_pred;
              }

              // Consecutive outliers treating
              if(correction == true && outlier_count == N_outlier_max + 1){
                  outlier_count = 0;
                  valuef = value;
              }

              // calculate the times of each stage
              double elapsed_time_teda = (end_time_teda.tv_sec - start_time_teda.tv_sec) +
                         (end_time_teda.tv_usec - start_time_teda.tv_usec) * 1e-6;

              double elapsed_time_rls_filter_1 = (end_time_rls_filter_1.tv_sec - start_time_rls_filter_1.tv_sec) +
                          (end_time_rls_filter_1.tv_usec - start_time_rls_filter_1.tv_usec) * 1e-6;

              double elapsed_time_rls_filter_2 = (end_time_rls_filter_2.tv_sec - start_time_rls_filter_2.tv_sec) +
                          (end_time_rls_filter_2.tv_usec - start_time_rls_filter_2.tv_usec) * 1e-6;

              double elapsed_time_rls_update = (end_time_rls_update.tv_sec - start_time_rls_update.tv_sec) +
                          (end_time_rls_update.tv_usec - start_time_rls_update.tv_usec) * 1e-6;

              // cast to microseconds
              int microseconds_teda = elapsed_time_teda * 1000000;
              int microseconds_rls_filter_1 = elapsed_time_rls_filter_1 * 1000000;
              int microseconds_rls_filter_2 = elapsed_time_rls_filter_2 * 1000000;
              int microseconds_rls_update = elapsed_time_rls_update * 1000000;

              buffer->add(time_teda_flag, ELEMENT_INT32, &microseconds_teda, sizeof(microseconds_teda));
              buffer->add(time_rls_filter_1_flag, ELEMENT_INT32, &microseconds_rls_filter_1, sizeof(microseconds_rls_filter_1));
              buffer->add(time_rls_filter_2_flag, ELEMENT_INT32, &microseconds_rls_filter_2, sizeof(microseconds_rls_filter_2));
              buffer->add(time_rls_update_flag, ELEMENT_INT32, &microseconds_rls_update, sizeof(microseconds_rls_update));

              // buffer->add((uint16_t)byte_x_atual | 0x100, ELEMENT_FLOAT, &valuef, sizeof(valuef));
              buffer->add((uint16_t)byte_flag | 0x100, ELEMENT_INT32, &flag, sizeof(flag));
              buffer->add((uint16_t)byte_x_antp | 0x100, ELEMENT_FLOAT, &x_ant[1], sizeof(x_ant[1]));
              buffer->add((uint16_t)byte_x_atualp | 0x100, ELEMENT_FLOAT, &valuef, sizeof(valuef));
              buffer->add((uint16_t)byte_y_pred | 0x100, ELEMENT_FLOAT, &y_pred, sizeof(y_pred));

              x_ant[0] = vetor_de_entrada[0];
              x_ant[1] = vetor_de_entrada[1];

              // buffer->add((uint16_t)pid | 0x100, ELEMENT_FLOAT, &valuef, sizeof(valuef));
              // buffer->add((uint16_t)byte_x_atual | 0x100, ELEMENT_FLOAT, &valuef, sizeof(valuef));
              // buffer->add((uint16_t)byte_flag | 0x100, ELEMENT_INT32, &flag, sizeof(flag));
              // buffer->add((uint16_t)byte_x_antp | 0x100, ELEMENT_FLOAT, &x_ant[1], sizeof(x_ant[1]));
              // buffer->add((uint16_t)byte_x_atualp | 0x100, ELEMENT_FLOAT, &valuef, sizeof(valuef));
              // buffer->add((uint16_t)byte_y_pred | 0x100, ELEMENT_FLOAT, &y_pred, sizeof(y_pred));
              // buffer->add((uint16_t)byte_y_predp | 0x100, ELEMENT_INT32, &value, sizeof(value));
          // }
          gettimeofday(&end_time, NULL);
          double elapsed_time = (end_time.tv_sec - start_time.tv_sec) +
                         (end_time.tv_usec - start_time.tv_usec) * 1e-6;
          Serial.print("Tempo de execucao: ");
          // print with 5 decimals
          Serial.println(elapsed_time, 6);
          
          // cast to microseconds
          int microseconds = elapsed_time * 1000000;

          buffer->add(time_flag, ELEMENT_INT32, &microseconds, sizeof(microseconds));
          
          
        } else {
          buffer->add((uint16_t)pid | 0x100, ELEMENT_INT32, &value, sizeof(value));
        }
        
      
    } else {
        timeoutsOBD++;
        printTimeoutStats();
        break;
    }
    processBLE(0);
    if (tier > 1) break;
  }
  int kph = obdData[0].value;
  if (kph >= 2) lastMotionTime = millis();
}
#endif

bool processGPS(CBuffer* buffer)
{
  static uint32_t lastGPStime = 0;
  static float lastGPSLat = 0;
  static float lastGPSLng = 0;

  if (!gd) {
    lastGPStime = 0;
    lastGPSLat = 0;
    lastGPSLng = 0;
  }
#if GNSS == GNSS_INTERNAL || GNSS == GNSS_EXTERNAL
  if (state.check(STATE_GPS_READY)) {
    // read parsed GPS data
    if (!sys.gpsGetData(&gd)) {
      return false;
    }
  }
#else
    if (!teleClient.cell.getLocation(&gd)) {
      return false;
    }
#endif

  if (!gd || lastGPStime == gd->time || gd->date == 0 || (gd->lng == 0 && gd->lat == 0)) return false;

  if ((lastGPSLat || lastGPSLng) && (abs(gd->lat - lastGPSLat) > 0.001 || abs(gd->lng - lastGPSLng > 0.001))) {
    // invalid coordinates data
    lastGPSLat = 0;
    lastGPSLng = 0;
    return false;
  }
  lastGPSLat = gd->lat;
  lastGPSLng = gd->lng;
  
  float kph = gd->speed * 1.852f;
  if (kph >= 2) lastMotionTime = millis();

  if (buffer) {
    buffer->add(PID_GPS_TIME, ELEMENT_UINT32, &gd->time, sizeof(uint32_t));
    if (gd->lat && gd->lng && gd->sat > 3) {
      buffer->add(PID_GPS_LATITUDE, ELEMENT_FLOAT, &gd->lat, sizeof(float));
      buffer->add(PID_GPS_LONGITUDE, ELEMENT_FLOAT, &gd->lng, sizeof(float));
      buffer->add(PID_GPS_ALTITUDE, ELEMENT_FLOAT_D1, &gd->alt, sizeof(float)); /* m */
      buffer->add(PID_GPS_SPEED, ELEMENT_FLOAT_D1, &kph, sizeof(kph));
      buffer->add(PID_GPS_HEADING, ELEMENT_UINT16, &gd->heading, sizeof(uint16_t));
      buffer->add(PID_GPS_SAT_COUNT, ELEMENT_UINT8, &gd->sat, sizeof(uint8_t));
      buffer->add(PID_GPS_HDOP, ELEMENT_UINT8, &gd->hdop, sizeof(uint8_t));
    }
  }
  
  // generate ISO time string
  char *p = isoTime + sprintf(isoTime, "%04u-%02u-%02uT%02u:%02u:%02u",
      (unsigned int)(gd->date % 100) + 2000, (unsigned int)(gd->date / 100) % 100, (unsigned int)(gd->date / 10000),
      (unsigned int)(gd->time / 1000000), (unsigned int)(gd->time % 1000000) / 10000, (unsigned int)(gd->time % 10000) / 100);
  unsigned char tenth = (gd->time % 100) / 10;
  if (tenth) p += sprintf(p, ".%c00", '0' + tenth);
  *p = 'Z';
  *(p + 1) = 0;

  Serial.print("[GPS] ");
  Serial.print(gd->lat, 6);
  Serial.print(' ');
  Serial.print(gd->lng, 6);
  Serial.print(' ');
  Serial.print((int)kph);
  Serial.print("km/h");
  if (gd->sat) {
    Serial.print(" SATS:");
    Serial.print(gd->sat);
  }
  Serial.print(" Course:");
  Serial.print(gd->heading);

  Serial.print(' ');
  Serial.println(isoTime);
  //Serial.println(gd->errors);
  lastGPStime = gd->time;
  return true;
}

bool waitMotionGPS(int timeout)
{
  unsigned long t = millis();
  lastMotionTime = 0;
  do {
      serverProcess(100);
    if (!processGPS(0)) continue;
    if (lastMotionTime) return true;
  } while (millis() - t < timeout);
  return false;
}

#if ENABLE_MEMS
void processMEMS(CBuffer* buffer)
{
  if (!state.check(STATE_MEMS_READY)) return;

  // load and store accelerometer data
  float temp;
#if ENABLE_ORIENTATION
  ORIENTATION ori;
  if (!mems->read(acc, gyr, mag, &temp, &ori)) return;
#else
  if (!mems->read(acc, gyr, mag, &temp)) return;
#endif
  deviceTemp = (int)temp;

  accSum[0] += acc[0];
  accSum[1] += acc[1];
  accSum[2] += acc[2];
  accCount++;

  if (buffer) {
    if (accCount) {
      float value[3];
      value[0] = accSum[0] / accCount - accBias[0];
      value[1] = accSum[1] / accCount - accBias[1];
      value[2] = accSum[2] / accCount - accBias[2];
      buffer->add(PID_ACC, ELEMENT_FLOAT_D2, value, sizeof(value), 3);
/*
      Serial.print("[ACC] ");
      Serial.print(value[0]);
      Serial.print('/');
      Serial.print(value[1]);
      Serial.print('/');
      Serial.println(value[2]);
*/
#if ENABLE_ORIENTATION
      value[0] = ori.yaw;
      value[1] = ori.pitch;
      value[2] = ori.roll;
      buffer->add(PID_ORIENTATION, ELEMENT_FLOAT_D2, value, sizeof(value), 3);
#endif
#if 0
      // calculate motion
      float motion = 0;
      for (byte i = 0; i < 3; i++) {
        motion += value[i] * value[i];
      }
      if (motion >= MOTION_THRESHOLD * MOTION_THRESHOLD) {
        lastMotionTime = millis();
        Serial.print("Motion:");
        Serial.println(motion);
      }
#endif
    }
    accSum[0] = 0;
    accSum[1] = 0;
    accSum[2] = 0;
    accCount = 0;
  }
}

void calibrateMEMS()
{
  if (state.check(STATE_MEMS_READY)) {
    accBias[0] = 0;
    accBias[1] = 0;
    accBias[2] = 0;
    int n;
    unsigned long t = millis();
    for (n = 0; millis() - t < 1000; n++) {
      float acc[3];
      if (!mems->read(acc)) continue;
      accBias[0] += acc[0];
      accBias[1] += acc[1];
      accBias[2] += acc[2];
      delay(10);
    }
    accBias[0] /= n;
    accBias[1] /= n;
    accBias[2] /= n;
    Serial.print("ACC BIAS:");
    Serial.print(accBias[0]);
    Serial.print('/');
    Serial.print(accBias[1]);
    Serial.print('/');
    Serial.println(accBias[2]);
  }
}
#endif

void printTime()
{
  time_t utc;
  time(&utc);
  struct tm *btm = gmtime(&utc);
  if (btm->tm_year > 100) {
    // valid system time available
    char buf[64];
    sprintf(buf, "%04u-%02u-%02u %02u:%02u:%02u",
      1900 + btm->tm_year, btm->tm_mon + 1, btm->tm_mday, btm->tm_hour, btm->tm_min, btm->tm_sec);
    Serial.print("UTC:");
    Serial.println(buf);
  }
}

/*******************************************************************************
  Initializing all data logging components
*******************************************************************************/
void initialize()
{
    // turn on buzzer at 2000Hz frequency 
  sys.buzzer(2000);
  delay(100);
  // turn off buzzer
  sys.buzzer(0);

  // dump buffer data
  bufman.purge();

#if ENABLE_MEMS
  if (state.check(STATE_MEMS_READY)) {
    calibrateMEMS();
  }
#endif

#if GNSS == GNSS_INTERNAL || GNSS == GNSS_EXTERNAL
  // start GPS receiver
  if (!state.check(STATE_GPS_READY)) {
#if GNSS == GNSS_EXTERNAL
    if (sys.gpsBeginExt())
#else
    if (sys.gpsBegin())
#endif
    {
      state.set(STATE_GPS_READY);
      Serial.println("GNSS:OK");
#if ENABLE_OLED
      oled.println("GNSS OK");
#endif
    } else {
      Serial.println("GNSS:NO");
    }
  }
#endif

#if ENABLE_OBD
  // initialize OBD communication
  if (!state.check(STATE_OBD_READY)) {
    timeoutsOBD = 0;
    if (obd.init()) {
      Serial.println("OBD:OK");
      state.set(STATE_OBD_READY);
#if ENABLE_OLED
      oled.println("OBD OK");
#endif
    } else {
      Serial.println("OBD:NO");
      //state.clear(STATE_WORKING);
      //return;
    }
  }
#endif

#if STORAGE != STORAGE_NONE
  if (!state.check(STATE_STORAGE_READY)) {
    // init storage
    if (logger.init()) {
      state.set(STATE_STORAGE_READY);
    }
  }
  if (state.check(STATE_STORAGE_READY)) {
    fileid = logger.begin();
  }
#endif

  // re-try OBD if connection not established
#if ENABLE_OBD
  if (state.check(STATE_OBD_READY)) {
    char buf[128];
    if (obd.getVIN(buf, sizeof(buf))) {
      memcpy(vin, buf, sizeof(vin) - 1);
      Serial.print("VIN:");
      Serial.println(vin);
    }
    int dtcCount = obd.readDTC(dtc, sizeof(dtc) / sizeof(dtc[0]));
    if (dtcCount > 0) {
      Serial.print("DTC:");
      Serial.println(dtcCount);
    }
#if ENABLE_OLED
    oled.print("VIN:");
    oled.println(vin);
#endif
  }
#endif

  // check system time
  printTime();

  lastMotionTime = millis();
  state.set(STATE_WORKING);

#if ENABLE_OLED
  delay(1000);
  oled.clear();
  oled.print("DEVICE ID: ");
  oled.println(devid);
  oled.setCursor(0, 7);
  oled.print("Packets");
  oled.setCursor(80, 7);
  oled.print("KB Sent");
  oled.setFontSize(FONT_SIZE_MEDIUM);
#endif
}

void showStats()
{
  uint32_t t = millis() - teleClient.startTime;
  char buf[32];
  sprintf(buf, "%02u:%02u.%c ", t / 60000, (t % 60000) / 1000, (t % 1000) / 100 + '0');
  Serial.print("[NET] ");
  Serial.print(buf);
  Serial.print("| Packet #");
  Serial.print(teleClient.txCount);
  Serial.print(" | Out: ");
  Serial.print(teleClient.txBytes >> 10);
  Serial.print(" KB | In: ");
  Serial.print(teleClient.rxBytes);
  Serial.print(" bytes");
  Serial.println();
#if ENABLE_OLED
  oled.setCursor(0, 2);
  oled.println(timestr);
  oled.setCursor(0, 5);
  oled.printInt(teleClient.txCount, 2);
  oled.setCursor(80, 5);
  oled.printInt(teleClient.txBytes >> 10, 3);
#endif
}

bool waitMotion(long timeout)
{
#if ENABLE_MEMS
  unsigned long t = millis();
  if (state.check(STATE_MEMS_READY)) {
    do {
      // calculate relative movement
      float motion = 0;
      float acc[3];
      if (!mems->read(acc)) continue;
      if (accCount == 10) {
        accCount = 0;
        accSum[0] = 0;
        accSum[1] = 0;
        accSum[2] = 0;
      }
      accSum[0] += acc[0];
      accSum[1] += acc[1];
      accSum[2] += acc[2];
      accCount++;
      for (byte i = 0; i < 3; i++) {
        float m = (acc[i] - accBias[i]);
        motion += m * m;
      }
#if ENABLE_HTTTPD
      serverProcess(100);
#endif
      processBLE(100);
      // check movement
      if (motion >= MOTION_THRESHOLD * MOTION_THRESHOLD) {
        //lastMotionTime = millis();
        Serial.println(motion);
        return true;
      }
    } while ((long)(millis() - t) < timeout || timeout == -1);
    return false;
  }
#endif
  serverProcess(timeout);
  return false;
}

/*******************************************************************************
  Collecting and processing data
*******************************************************************************/
void process()
{
  uint32_t startTime = millis();

  CBuffer* buffer = bufman.getFree();
  buffer->state = BUFFER_STATE_FILLING;

#if ENABLE_OBD
  // process OBD data if connected
  if (state.check(STATE_OBD_READY)) {
    processOBD(buffer);
    if (obd.errors >= MAX_OBD_ERRORS) {
      if (!obd.init()) {
        Serial.println("[OBD] ECU OFF");
        state.clear(STATE_OBD_READY | STATE_WORKING);
        return;
      }
    }
  } else if (obd.init(PROTO_AUTO, true)) {
    state.set(STATE_OBD_READY);
    Serial.println("[OBD] ECU ON");
  }
#endif

  if (rssi != rssiLast) {
    int val = (rssiLast = rssi);
    buffer->add(PID_CSQ, ELEMENT_INT32, &val, sizeof(val));
  }
#if ENABLE_OBD
  if (sys.devType > 12) {
    batteryVoltage = (float)(analogRead(A0) * 45) / 4095;
  } else {
    batteryVoltage = obd.getVoltage();
  }
  if (batteryVoltage) {
    uint16_t v = batteryVoltage * 100;
    buffer->add(PID_BATTERY_VOLTAGE, ELEMENT_UINT16, &v, sizeof(v));
  }
#endif

#if LOG_EXT_SENSORS
  processExtInputs(buffer);
#endif

#if ENABLE_MEMS
  processMEMS(buffer);
#endif

  processGPS(buffer);

  if (!state.check(STATE_MEMS_READY)) {
    deviceTemp = readChipTemperature();
  }
  buffer->add(PID_DEVICE_TEMP, ELEMENT_INT32, &deviceTemp, sizeof(deviceTemp));

  buffer->timestamp = millis();
  buffer->state = BUFFER_STATE_FILLED;

  // display file buffer stats
  if (startTime - lastStatsTime >= 3000) {
    bufman.printStats();
    lastStatsTime = startTime;
  }

#if STORAGE != STORAGE_NONE
  if (state.check(STATE_STORAGE_READY)) {
    buffer->serialize(logger);
    uint16_t sizeKB = (uint16_t)(logger.size() >> 10);
    if (sizeKB != lastSizeKB) {
      logger.flush();
      lastSizeKB = sizeKB;
      Serial.print("[FILE] ");
      Serial.print(sizeKB);
      Serial.println("KB");
    }
  }
#endif

  const int dataIntervals[] = DATA_INTERVAL_TABLE;
#if ENABLE_OBD || ENABLE_MEMS
  // motion adaptive data interval control
  const uint16_t stationaryTime[] = STATIONARY_TIME_TABLE;
  unsigned int motionless = (millis() - lastMotionTime) / 1000;
  bool stationary = true;
  for (byte i = 0; i < sizeof(stationaryTime) / sizeof(stationaryTime[0]); i++) {
    dataInterval = dataIntervals[i];
    if (motionless < stationaryTime[i] || stationaryTime[i] == 0) {
      stationary = false;
      break;
    }
  }
  if (stationary) {
    // stationery timeout
    Serial.print("Stationary for ");
    Serial.print(motionless);
    Serial.println(" secs");
    // trip ended, go into standby
    state.clear(STATE_WORKING);
    return;
  }
#else
  dataInterval = dataIntervals[0];
#endif
  do {
    long t = dataInterval - (millis() - startTime);
    processBLE(t > 0 ? t : 0);
  } while (millis() - startTime < dataInterval);
}

bool initCell(bool quick = false)
{
  Serial.println("[CELL] Activating...");
  // power on network module
  if (!teleClient.cell.begin(&sys)) {
    Serial.println("[CELL] No supported module");
#if ENABLE_OLED
    oled.println("No Cell Module");
#endif
    return false;
  }
  if (quick) return true;
#if ENABLE_OLED
    oled.print(teleClient.cell.deviceName());
    oled.println(" OK\r");
    oled.print("IMEI:");
    oled.println(teleClient.cell.IMEI);
#endif
  Serial.print("CELL:");
  Serial.println(teleClient.cell.deviceName());
  if (!teleClient.cell.checkSIM(SIM_CARD_PIN)) {
    Serial.println("NO SIM CARD");
    //return false;
  }
  Serial.print("IMEI:");
  Serial.println(teleClient.cell.IMEI);
  Serial.println("[CELL] Searching...");
  if (teleClient.cell.setup(CELL_APN)) {
    String op = teleClient.cell.getOperatorName();
    if (op.length()) {
      Serial.print("Operator:");
      Serial.println(op);
#if ENABLE_OLED
      oled.println(op);
#endif
    }

#if GNSS == GNSS_CELLULAR
    if (teleClient.cell.setGPS(true)) {
      Serial.println("CELL GNSS:OK");
    }
#endif

    String ip = teleClient.cell.getIP();
    if (ip.length()) {
      Serial.print("[CELL] IP:");
      Serial.println(ip);
#if ENABLE_OLED
      oled.print("IP:");
      oled.println(ip);
#endif
    }
    state.set(STATE_CELL_CONNECTED);
  } else {
    char *p = strstr(teleClient.cell.getBuffer(), "+CPSI:");
    if (p) {
      char *q = strchr(p, '\r');
      if (q) *q = 0;
      Serial.print("[CELL] ");
      Serial.println(p + 7);
#if ENABLE_OLED
      oled.println(p + 7);
#endif
    } else {
      Serial.print(teleClient.cell.getBuffer());
    }

    for (int n = 0; n < 3; n++) {
      rssi = teleClient.cell.RSSI();
      if (rssi) {
        Serial.print("RSSI:");
        Serial.print(rssi);
        Serial.println("dBm");
      }
      delay(1000);
    }
  }
  timeoutsNet = 0;
  return state.check(STATE_CELL_CONNECTED);
}

/*******************************************************************************
  Initializing network, maintaining connection and doing transmissions
*******************************************************************************/
void telemetry(void* inst)
{
  uint32_t lastRssiTime = 0;
  uint8_t connErrors = 0;
  CStorageRAM store;
  store.init(
#if HAS_LARGE_RAM
    (char*)heap_caps_malloc(SERIALIZE_BUFFER_SIZE, MALLOC_CAP_SPIRAM),
#else
    (char*)malloc(SERIALIZE_BUFFER_SIZE),
#endif
    SERIALIZE_BUFFER_SIZE
  );
  teleClient.reset();

  for (;;) {
    if (state.check(STATE_STANDBY)) {
      if (state.check(STATE_CELL_CONNECTED) || state.check(STATE_WIFI_CONNECTED)) {
        teleClient.shutdown();
      }
      state.clear(STATE_NET_READY | STATE_CELL_CONNECTED | STATE_WIFI_CONNECTED);
      teleClient.reset();
      bufman.purge();

#if GNSS == GNSS_INTERNAL || GNSS == GNSS_EXTERNAL
      if (state.check(STATE_GPS_READY)) {
        Serial.println("[GPS] OFF");
#if GNSS_ALWAYS_ON
        sys.gpsEnd(false);
#else
        sys.gpsEnd(true);
#endif
        state.clear(STATE_GPS_READY);
      }
      gd = 0;
#endif

      uint32_t t = millis();
      do {
        delay(1000);
      } while (state.check(STATE_STANDBY) && millis() - t < 1000L * PING_BACK_INTERVAL);
      if (state.check(STATE_STANDBY)) {
        // start ping
#if ENABLE_WIFI
        Serial.print("[WIFI] Joining SSID:");
        Serial.println(WIFI_SSID);
        teleClient.wifi.begin(WIFI_SSID, WIFI_PASSWORD);
        if (teleClient.wifi.setup()) {
          Serial.println("[WIFI] Ping...");
          teleClient.ping();
        }
        else
#endif
        {
          if (initCell()) {
            Serial.println("[CELL] Ping...");
            teleClient.ping();
          }
        }
        teleClient.shutdown();
        state.clear(STATE_CELL_CONNECTED | STATE_WIFI_CONNECTED);
      }
      continue;
    }
    
#if ENABLE_WIFI
    if (!state.check(STATE_WIFI_CONNECTED)) {
      Serial.print("[WIFI] Joining SSID:");
      Serial.println(WIFI_SSID);
      teleClient.wifi.begin(WIFI_SSID, WIFI_PASSWORD);
      teleClient.wifi.setup();
      initCell(true);
    }
#endif

    while (state.check(STATE_WORKING)) {
#if ENABLE_WIFI
      if (!state.check(STATE_WIFI_CONNECTED) && teleClient.wifi.connected()) {
        String ip = teleClient.wifi.getIP();
        if (ip.length()) {
          Serial.print("[WIFI] IP:");
          Serial.println(ip);
        }
        connErrors = 0;
        if (teleClient.connect()) {
          state.set(STATE_WIFI_CONNECTED | STATE_NET_READY);
          // switch off cellular module when wifi connected
          teleClient.cell.end();
          state.clear(STATE_CELL_CONNECTED);
          Serial.println("[CELL] Deactivated");
        }
      } else if (state.check(STATE_WIFI_CONNECTED) && !teleClient.wifi.connected()) {
        Serial.println("[WIFI] Disconnected");
        state.clear(STATE_WIFI_CONNECTED);
      }
#endif
      if (!state.check(STATE_WIFI_CONNECTED) && !state.check(STATE_CELL_CONNECTED)) {
        connErrors = 0;
        if (!initCell() || !teleClient.connect()) {
          teleClient.cell.end();
          state.clear(STATE_NET_READY | STATE_CELL_CONNECTED);
          delay(15000);
          break;
        }
        Serial.println("[CELL] In service");
      }

      if (millis() - lastRssiTime > SIGNAL_CHECK_INTERVAL * 1000) {
#if ENABLE_WIFI
        if (state.check(STATE_WIFI_CONNECTED))
        {
          rssi = teleClient.wifi.RSSI();
        }
        else
#endif
        {
          rssi = teleClient.cell.RSSI();
        }
        if (rssi) {
          Serial.print("RSSI:");
          Serial.print(rssi);
          Serial.println("dBm");
        }
        lastRssiTime = millis();

#if ENABLE_WIFI
        if (!state.check(STATE_WIFI_CONNECTED)) {
          teleClient.wifi.begin(WIFI_SSID, WIFI_PASSWORD);
        }
#endif
      }

      // get data from buffer
      CBuffer* buffer = bufman.getNewest();
      if (!buffer) {
        delay(50);
        continue;
      }
#if SERVER_PROTOCOL == PROTOCOL_UDP
      store.header(devid);
#endif
      store.timestamp(buffer->timestamp);
      buffer->serialize(store);
      bufman.free(buffer);
      store.tailer();
      Serial.println(store.buffer());

      // start transmission
#ifdef PIN_LED
      if (ledMode == 0) digitalWrite(PIN_LED, HIGH);
#endif

      if (teleClient.transmit(store.buffer(), store.length())) {
        // successfully sent
        connErrors = 0;
        showStats();
      } else {
        timeoutsNet++;
        connErrors++;
        printTimeoutStats();
        if (connErrors < MAX_CONN_ERRORS_RECONNECT) {
          // quick reconnect
          teleClient.connect(true);
        }
      }
#ifdef PIN_LED
      if (ledMode == 0) digitalWrite(PIN_LED, LOW);
#endif
      store.purge();

      teleClient.inbound();

      if (state.check(STATE_CELL_CONNECTED) && !teleClient.cell.check(1000)) {
        Serial.println("[CELL] Not in service");
        state.clear(STATE_NET_READY | STATE_CELL_CONNECTED);
        break;
      }

      if (syncInterval > 10000 && millis() - teleClient.lastSyncTime > syncInterval) {
        Serial.println("[NET] Poor connection");
        timeoutsNet++;
        if (!teleClient.connect()) {
          connErrors++;
        }
      }

      if (connErrors >= MAX_CONN_ERRORS_RECONNECT) {
#if ENABLE_WIFI
        if (state.check(STATE_WIFI_CONNECTED)) {
          teleClient.wifi.end();
          state.clear(STATE_NET_READY | STATE_WIFI_CONNECTED);
          break;
        }
#endif
        if (state.check(STATE_CELL_CONNECTED)) {
          teleClient.cell.end();
          state.clear(STATE_NET_READY | STATE_CELL_CONNECTED);
          break;
        }
      }

      if (deviceTemp >= COOLING_DOWN_TEMP) {
        // device too hot, cool down by pause transmission
        Serial.print("HIGH DEVICE TEMP: ");
        Serial.println(deviceTemp);
        bufman.purge();
      }

    }
  }
}

/*******************************************************************************
  Implementing stand-by mode
*******************************************************************************/
void standby()
{
  state.set(STATE_STANDBY);
#if STORAGE != STORAGE_NONE
  if (state.check(STATE_STORAGE_READY)) {
    logger.end();
  }
#endif
  state.clear(STATE_WORKING | STATE_OBD_READY | STATE_STORAGE_READY);
  // this will put co-processor into sleep mode
#if ENABLE_OLED
  oled.print("STANDBY");
  delay(1000);
  oled.clear();
#endif
  Serial.println("STANDBY");
  obd.enterLowPowerMode();
#if ENABLE_MEMS
  calibrateMEMS();
  waitMotion(-1);
#elif ENABLE_OBD
  do {
    delay(5000);
  } while (obd.getVoltage() < JUMPSTART_VOLTAGE);
#else
  delay(5000);
#endif
  Serial.println("Wakeup");

  sys.resetLink();
#if RESET_AFTER_WAKEUP
#if ENABLE_MEMS
  mems->end();  
#endif
  ESP.restart();
#endif  
  state.clear(STATE_STANDBY);
}

/*******************************************************************************
  Tasks to perform in idle/waiting time
*******************************************************************************/
void genDeviceID(char* buf)
{
    uint64_t seed = ESP.getEfuseMac() >> 8;
    for (int i = 0; i < 8; i++, seed >>= 5) {
      byte x = (byte)seed & 0x1f;
      if (x >= 10) {
        x = x - 10 + 'A';
        switch (x) {
          case 'B': x = 'W'; break;
          case 'D': x = 'X'; break;
          case 'I': x = 'Y'; break;
          case 'O': x = 'Z'; break;
        }
      } else {
        x += '0';
      }
      buf[i] = x;
    }
    buf[8] = 0;
}

void showSysInfo()
{
  Serial.print("CPU:");
  Serial.print(ESP.getCpuFreqMHz());
  Serial.print("MHz FLASH:");
  Serial.print(ESP.getFlashChipSize() >> 20);
  Serial.println("MB");
  Serial.print("IRAM:");
  Serial.print(ESP.getHeapSize() >> 10);
  Serial.print("KB");
#if BOARD_HAS_PSRAM
  Serial.print(" PSRAM:");
  Serial.print(esp_spiram_get_size() >> 20);
  Serial.print("MB");
#endif
  Serial.println();

  int rtc = rtc_clk_slow_freq_get();
  if (rtc) {
    Serial.print("RTC:");
    Serial.println(rtc);
  }

#if ENABLE_OLED
  oled.clear();
  oled.print("CPU:");
  oled.print(ESP.getCpuFreqMHz());
  oled.print("Mhz ");
  oled.print(getFlashSize() >> 10);
  oled.println("MB Flash");
#endif

  Serial.print("DEVICE ID:");
  Serial.println(devid);
#if ENABLE_OLED
  oled.print("DEVICE ID:");
  oled.println(devid);
#endif
}

#if CONFIG_MODE_TIMEOUT
void configMode()
{
  uint32_t t = millis();

  do {
    if (Serial.available()) {
      // enter config mode
      Serial.println("#CONFIG MODE#");
      Serial1.begin(LINK_UART_BAUDRATE, SERIAL_8N1, PIN_LINK_UART_RX, PIN_LINK_UART_TX);
      do {
        if (Serial.available()) {
          Serial1.write(Serial.read());
          t = millis();
        }
        if (Serial1.available()) {
          Serial.write(Serial1.read());
          t = millis();
        }
      } while (millis() - t < CONFIG_MODE_TIMEOUT);
      Serial.println("#RESET#");
      delay(100);
      ESP.restart();
    }
  } while (millis() - t < CONFIG_MODE_TIMEOUT);
}
#endif

void processBLE(int timeout)
{
#if ENABLE_BLE
    static byte echo = 0;
    char* cmd;
    if (!(cmd = ble_recv_command(timeout))) {
        return;
    }

    char *p = strchr(cmd, '\r');
    if (p) *p = 0;
    char buf[48];
    int bufsize = sizeof(buf);
    int n = 0;
    if (echo) n += snprintf(buf + n, bufsize - n, "%s\r", cmd);
    Serial.print("[BLE] ");
    Serial.print(cmd);
    if (!strcmp(cmd, "UPTIME") || !strcmp(cmd, "TICK")) {
        n += snprintf(buf + n, bufsize - n, "%lu", millis());
    } else if (!strcmp(cmd, "BATT")) {
        n += snprintf(buf + n, bufsize - n, "%.2f", (float)(analogRead(A0) * 42) / 4095);
    } else if (!strcmp(cmd, "RESET")) {
#if STORAGE
        logger.end();
#endif
        ESP.restart();
        // never reach here
    } else if (!strcmp(cmd, "OFF")) {
        state.set(STATE_STANDBY);
        n += snprintf(buf + n, bufsize - n, "OK");
    } else if (!strcmp(cmd, "ON")) {
        state.clear(STATE_STANDBY);
        n += snprintf(buf + n, bufsize - n, "OK");
    } else if (!strcmp(cmd, "ON?")) {
        n += snprintf(buf + n, bufsize - n, "%u", state.check(STATE_STANDBY) ? 0 : 1);
#if ENABLE_MEMS
    } else if (!strcmp(cmd, "TEMP")) {
        n += snprintf(buf + n, bufsize - n, "%d", (int)deviceTemp);
    } else if (!strcmp(cmd, "ACC")) {
        n += snprintf(buf + n, bufsize - n, "%.1f/%.1f/%.1f", acc[0], acc[1], acc[2]);
    } else if (!strcmp(cmd, "GYRO")) {
        n += snprintf(buf + n, bufsize - n, "%.1f/%.1f/%.1f", gyr[0], gyr[1], gyr[2]);
    } else if (!strcmp(cmd, "GF")) {
        n += snprintf(buf + n, bufsize - n, "%f", (float)sqrt(acc[0]*acc[0] + acc[1]*acc[1] + acc[2]*acc[2]));
#endif
    } else if (!strcmp(cmd, "RSSI")) {
        n += snprintf(buf + n, bufsize - n, "%d", rssi);
    } else if (!strcmp(cmd, "ATE0")) {
        echo = 0;
        n += snprintf(buf + n, bufsize - n, "OK");
    } else if (!strcmp(cmd, "ATE1")) {
        echo = 1;
        n += snprintf(buf + n, bufsize - n, "OK");
    } else if (!strcmp(cmd, "FS")) {
        n += snprintf(buf + n, bufsize - n, "%u",
#if STORAGE == STORAGE_NONE
          0
#else
          logger.size()
#endif
        );
    } else if (!memcmp(cmd, "01", 2)) {
        byte pid = hex2uint8(cmd + 2);
        for (byte i = 0; i < sizeof(obdData) / sizeof(obdData[0]); i++) {
            if (obdData[i].pid == pid) {
                n += snprintf(buf + n, bufsize - n, "%d", obdData[i].value);
                pid = 0;
                break;
            }
        }
        if (pid) {
            int value;
            if (obd.readPID(pid, value)) {
                n += snprintf(buf + n, bufsize - n, "%d", value);
            } else {
                n += snprintf(buf + n, bufsize - n, "N/A");
            }
        }
    } else if (!strcmp(cmd, "VIN")) {
        n += snprintf(buf + n, bufsize - n, "%s", vin[0] ? vin : "N/A");
    } else if (!strcmp(cmd, "LAT") && gd) {
        n += snprintf(buf + n, bufsize - n, "%f", gd->lat);
    } else if (!strcmp(cmd, "LNG") && gd) {
        n += snprintf(buf + n, bufsize - n, "%f", gd->lng);
    } else if (!strcmp(cmd, "ALT") && gd) {
        n += snprintf(buf + n, bufsize - n, "%d", (int)gd->alt);
    } else if (!strcmp(cmd, "SAT") && gd) {
        n += snprintf(buf + n, bufsize - n, "%u", (unsigned int)gd->sat);
    } else if (!strcmp(cmd, "SPD") && gd) {
        n += snprintf(buf + n, bufsize - n, "%d", (int)(gd->speed * 1852 / 1000));
    } else if (!strcmp(cmd, "CRS") && gd) {
        n += snprintf(buf + n, bufsize - n, "%u", (unsigned int)gd->heading);
    } else {
        n += snprintf(buf + n, bufsize - n, "ERROR");
    }
    Serial.print(" -> ");
    Serial.println((p = strchr(buf, '\r')) ? p + 1 : buf);
    if (n < bufsize - 1) {
        buf[n++] = '\r';
    } else {
        n = bufsize - 1;
    }
    buf[n] = 0;
    ble_send_response(buf, n, cmd);
#else
    if (timeout) delay(timeout);
#endif
}

void setup()
{
  delay(500);
  
#if ENABLE_OLED
  oled.begin();
  oled.setFontSize(FONT_SIZE_SMALL);
#endif
  // initialize USB serial
  Serial.begin(115200);

  // init LED pin
#ifdef PIN_LED
  pinMode(PIN_LED, OUTPUT);
  if (ledMode == 0) digitalWrite(PIN_LED, HIGH);
#endif

  // generate unique device ID
  genDeviceID(devid);

#if CONFIG_MODE_TIMEOUT
  configMode();
#endif

#if LOG_EXT_SENSORS == 1
  pinMode(PIN_SENSOR1, INPUT);
  pinMode(PIN_SENSOR2, INPUT);
#elif LOG_EXT_SENSORS == 2
  adc1_config_width(ADC_WIDTH_BIT_12);
  adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_11);
  adc1_config_channel_atten(ADC1_CHANNEL_1, ADC_ATTEN_DB_11);
#endif

  // show system information
  showSysInfo();

  bufman.init();
  
  //Serial.print(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10);
  //Serial.println("KB");

#if ENABLE_OBD
  if (sys.begin()) {
    Serial.print("TYPE:");
    Serial.println(sys.devType);
    obd.begin(sys.link);
  }
#else
  sys.begin(false, true);
#endif

#if ENABLE_MEMS
if (!state.check(STATE_MEMS_READY)) do {
  Serial.print("MEMS:");
  mems = new ICM_42627;
  byte ret = mems->begin();
  if (ret) {
    state.set(STATE_MEMS_READY);
    Serial.println("ICM-42627");
    break;
  }
  delete mems;
  mems = new ICM_20948_I2C;
  ret = mems->begin();
  if (ret) {
    state.set(STATE_MEMS_READY);
    Serial.println("ICM-20948");
    break;
  } 
  delete mems;
  mems = new MPU9250;
  ret = mems->begin();
  if (ret) {
    state.set(STATE_MEMS_READY);
    Serial.println("MPU-9250");
    break;
  } 
  Serial.println("NO");
} while (0);
#endif

#if ENABLE_HTTPD
  IPAddress ip;
  if (serverSetup(ip)) {
    Serial.println("HTTPD:");
    Serial.println(ip);
#if ENABLE_OLED
    oled.println(ip);
#endif
  } else {
    Serial.println("HTTPD:NO");
  }
#endif

  state.set(STATE_WORKING);

#if ENABLE_BLE
  // init BLE
  ble_init();
#endif

  // initialize components
  initialize();

  // initialize network and maintain connection
  subtask.create(telemetry, "telemetry", 2, 8192);

#ifdef PIN_LED
  digitalWrite(PIN_LED, LOW);
#endif
}

void loop()
{
  // error handling
  if (!state.check(STATE_WORKING)) {
    standby();
#ifdef PIN_LED
    if (ledMode == 0) digitalWrite(PIN_LED, HIGH);
#endif
    initialize();
#ifdef PIN_LED
    digitalWrite(PIN_LED, LOW);
#endif
    return;
  }

  // collect and log data
  process();
}
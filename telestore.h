#include <SPI.h>
#include <FS.h>
#include <SD.h>
#include <SPIFFS.h>

#include <math.h>

#define WINDOW_THRESHOLD 3
#define M_PI  3.14159265358979323846  /* pi */

class TEDA
{
private:
    int window_threshold = WINDOW_THRESHOLD;
    float k = 1;
    float m;
    float variance = 0;
    float eccentricity = 0;
    float mean = 0;
    int window_count = 0;
    int tempo = 0;
    float norm_eccentricity, outlier_threshold, last_value;

public:
    TEDA(float threshold){
      m = threshold;
    }

    virtual void resetWindow(float x)
    {
        k = 1;
        variance = 0;
        mean = 0;
        window_count = 0;
        last_value = x;
    }
    virtual float calcMean(float x)
    {
        float new_mean = (((k - 1) / k) * mean) + ((1 / k) * x);
        //Serial.print("new_mean = ");
        //Serial.println(new_mean);
        return new_mean;
    }
    virtual float calcVariance(float x)
    {
        float distance_squared = ((x - mean) * (x - mean));
        float new_var = (((k - 1) / k) * variance) + (distance_squared * (1 / (k - 1)));
        //Serial.print("new_var = ");
        //Serial.println(new_var);
        return new_var;
    }
    virtual float calcEccentricity(float x)
    {
        float new_ecc;
        float mean2 = (mean - x) * (mean - x);
        if (mean2 == 0)
        {
            new_ecc = 0;
        }
        else
        {
            new_ecc = (1 / k) + ((mean2) / (k * variance));
        }
        //Serial.print("new_ecc = ");
        //Serial.println(new_ecc);
        return new_ecc;
    }
    virtual int run(float x)
    {

        //int n = 1.5;
        tempo = tempo + 1;

        if (k == 1)
        {
            // Serial.println(k);
            mean = x;
            variance = 0;
            k = k + 1;
            last_value = x;
            if (tempo == 1)
                return 1;
            return 0;
        }
        else if (x == last_value && variance == 0)
        {
            mean = calcMean(x);
            variance = calcVariance(x);

            k = k + 1;
            last_value = x;
            return 0;
        }
        else

        {
            mean = calcMean(x);
            variance = calcVariance(x);
            eccentricity = calcEccentricity(x);
            norm_eccentricity = eccentricity / 2;

            outlier_threshold = ((m * m) + 1) / (2 * k);

            if (norm_eccentricity > outlier_threshold)
            {
                k = k + 1;
                last_value = x;
                return 1;
            }
            else
            {
                k = k + 1;
                last_value = x;
                return 0;
            }
        }
    };
};

class CStorage;

class CStorage {
public:
    virtual bool init() { return true; }
    virtual void uninit() {}
    virtual void log(uint16_t pid, uint8_t values[], uint8_t count);
    virtual void log(uint16_t pid, uint16_t values[], uint8_t count);
    virtual void log(uint16_t pid, uint32_t values[], uint8_t count);
    virtual void log(uint16_t pid, int32_t values[], uint8_t count);
    virtual void log(uint16_t pid, float values[], uint8_t count, const char* fmt = "%f");
    virtual void timestamp(uint32_t ts);
    virtual void purge() { m_samples = 0; }
    virtual uint16_t samples() { return m_samples; }
    virtual void dispatch(const char* buf, byte len);
protected:
    byte checksum(const char* data, int len);
    virtual void header(const char* devid) {}
    virtual void tailer() {}
    int m_samples = 0;
    char m_delimiter = ':';
};

class CStorageRAM: public CStorage {
public:
    void init(char* cache, unsigned int cacheSize)
    {
        m_cacheSize = cacheSize;
        m_cache = cache;
    }
    void uninit()
    {
        if (m_cache) {
            delete m_cache;
            m_cache = 0;
            m_cacheSize = 0;
        }
    }
    void purge() { m_cacheBytes = 0; m_samples = 0; }
    unsigned int length() { return m_cacheBytes; }
    char* buffer() { return m_cache; }
    void dispatch(const char* buf, byte len);
    void header(const char* devid);
    void tailer();
    void untailer();
protected:
    unsigned int m_cacheSize = 0;
    unsigned int m_cacheBytes = 0;
    char* m_cache = 0;
};

class FileLogger : public CStorage {
public:
    FileLogger() { m_delimiter = ','; }
    virtual void dispatch(const char* buf, byte len);
    virtual uint32_t size() { return m_size; }
    virtual void end()
    {
        m_file.close();
        m_id = 0;
        m_size = 0;
    }
    virtual void flush()
    {
        m_file.flush();
    }
protected:
    int getFileID(File& root);
    uint32_t m_dataTime = 0;
    uint32_t m_dataCount = 0;
    uint32_t m_size = 0;
    uint32_t m_id = 0;
    File m_file;
};

class SDLogger : public FileLogger {
public:
    bool init();
    uint32_t begin();
    void flush();
};

class SPIFFSLogger : public FileLogger {
public:
    bool init();
    uint32_t begin();
private:
    void purge();
};

#ifndef INFLUXDB_CLIENT_H
#define INFLUXDB_CLIENT_H

#include <string>
#include <curl/curl.h>

class InfluxDBClient {
public:
    InfluxDBClient();
    InfluxDBClient(const std::string& url,
                  const std::string& org,
                  const std::string& bucket,
                  const std::string& token);
    ~InfluxDBClient();

    bool send(const std::string& measurement,
             const std::string& tags,
             const std::string& fields,
             int64_t timestamp = 0);

    bool isConnected() const { return connected_; }

private:
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp);
    bool initializeCurl();
    void cleanupCurl();

    std::string url_;
    std::string org_;
    std::string bucket_;
    std::string token_;
    bool connected_;
    CURL* curl_;
    struct curl_slist* headers_;
};

#endif // INFLUXDB_CLIENT_H

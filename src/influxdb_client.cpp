
#include "influxdb_client.h"
#include <iostream>

InfluxDBClient::InfluxDBClient() {
    connected_ = false;
}

InfluxDBClient::InfluxDBClient(const std::string& url,
                             const std::string& org,
                             const std::string& bucket,
                             const std::string& token)
    : url_(url), org_(org), bucket_(bucket), token_(token),
      connected_(false), curl_(nullptr), headers_(nullptr),
      not_connect_notify(false) {
    connected_ = initializeCurl();
}

InfluxDBClient::~InfluxDBClient() {
    if (connected_)
        cleanupCurl();
}

bool InfluxDBClient::initializeCurl() {
    curl_ = curl_easy_init();
    if (!curl_) {
        std::cerr << "Failed to initialize CURL" << std::endl;
        return false;
    }

    // Set common options that persist across requests
    curl_easy_setopt(curl_, CURLOPT_URL, (url_ + "/api/v2/write?org=" + org_ + "&bucket=" + bucket_).c_str());
     std::cout << url_ + "/api/v2/write?org=" + org_ + "&bucket=" + bucket_ << "\n";

    // Set up headers
    headers_ = curl_slist_append(headers_, ("Authorization: Token " + token_).c_str());
    headers_ = curl_slist_append(headers_, "Content-Type: text/plain; charset=utf-8");
    headers_ = curl_slist_append(headers_, "Accept: application/json");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);

    return true;
}

void InfluxDBClient::cleanupCurl() {
    if (headers_) {
        curl_slist_free_all(headers_);
        headers_ = nullptr;
    }
    if (curl_) {
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }
}

bool InfluxDBClient::send(const std::string& measurement,
                         const std::string& tags,
                         const std::string& fields,
                         int64_t timestamp) {
    if (!connected_) {
        if (!not_connect_notify) {
            std::cerr << "Not connected to InfluxDB" << std::endl;
            not_connect_notify = true;
        }
        return false;
    }

    // Construct the line protocol
    std::string data = measurement;
    if (!tags.empty()) {
        data += "," + tags;
    }
    data += " " + fields;
    if (timestamp > 0) {
        data += " " + std::to_string(timestamp);
    }

    // Set up the POST request
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, data.c_str());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, data.length());

    // Response handling
    std::string response;
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response);

    // Execute the request
    CURLcode res = curl_easy_perform(curl_);
    if (res != CURLE_OK) {
        std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        return false;
    }

    // Check HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 204) { // 204 is success for InfluxDB
        std::cerr << "HTTP error: " << http_code << " Response: " << response << std::endl;
        return false;
    }

    return true;
}

size_t InfluxDBClient::writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

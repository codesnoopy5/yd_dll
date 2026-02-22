#include "protobuf_http_client.hpp"
#include <curl/curl.h>
#include <sstream>
#include <iostream>
#include "little_goal.pb.h"

class ProtobufHttpClient::Impl {
public:
    Impl(const Config& config) : config_(config) {
        curl_ = curl_easy_init();
        if (!curl_) {
            throw Exception("CURL initialization failed");
        }
    }

    ~Impl() {
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    template<typename RequestType, typename ResponseType>
    bool performRequest(const std::string& method,
        const std::string& endpoint,
        const RequestType& request,
        ResponseType& response);

private:
    static size_t writeCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        auto* buffer = static_cast<std::string*>(userp);
        buffer->append(static_cast<char*>(contents), size * nmemb);
        return size * nmemb;
    }

    void setupCurlCommon() {
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, &Impl::writeCallback);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, config_.timeout_ms);
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 1L);

        if (!config_.ca_cert_path.empty()) {
            curl_easy_setopt(curl_, CURLOPT_CAINFO, config_.ca_cert_path.c_str());
        }
        // 其他CURL通用设置...
    }

    Config config_;
    CURL* curl_;
};

template<typename RequestType, typename ResponseType>
bool ProtobufHttpClient::Impl::performRequest(
    const std::string& method,
    const std::string& endpoint,
    const RequestType& request,
    ResponseType& response)
{
    curl_easy_reset(curl_);
    setupCurlCommon();

    std::string url = config_.base_url + endpoint;
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_CUSTOMREQUEST, method.c_str());

    std::string request_data;
    if (!request.SerializeToString(&request_data)) {
        throw Exception("Protobuf serialization failed");
    }

    if (method != "GET") {
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, request_data.data());
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, request_data.size());
    }

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/protobuf");
    headers = curl_slist_append(headers, "Accept: application/protobuf");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    std::string response_data;
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_data);

    CURLcode res = curl_easy_perform(curl_);
    curl_slist_free_all(headers);

    if (res != CURLE_OK) {
        throw Exception(curl_easy_strerror(res));
    }

    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code < 200 || http_code >= 300) {
        throw Exception("HTTP error: " + std::to_string(http_code));
    }

    if (!response.ParseFromString(response_data)) {
        throw Exception("Failed to parse response");
    }

    return true;
}

// ProtobufHttpClient成员函数实现
//ProtobufHttpClient::ProtobufHttpClient(const Config& config)
//    : impl_(std::make_unique<Impl>(config)) {
//}

ProtobufHttpClient::ProtobufHttpClient(const Config& config)
    : impl_(std::make_unique<Impl>(config)), config_(config) {
}


ProtobufHttpClient::~ProtobufHttpClient() = default;

template<typename ResponseType>
std::unique_ptr<ResponseType> ProtobufHttpClient::get(const std::string& endpoint) {
    auto response = std::make_unique<ResponseType>();
    google::protobuf::Empty empty;
    if (performRequest("GET", endpoint, empty, *response)) {
        return response;
    }
    return nullptr;
}

template<typename RequestType, typename ResponseType>
std::unique_ptr<ResponseType> ProtobufHttpClient::post(
    const std::string& endpoint, const RequestType& request) {
    auto response = std::make_unique<ResponseType>();
    if (performRequest("POST", endpoint, request, *response)) {
        return response;
    }
    return nullptr;
}

template<typename RequestType, typename ResponseType>
bool ProtobufHttpClient::performRequest(
    const std::string& method,
    const std::string& endpoint,
    const RequestType& request,
    ResponseType& response) {
    try {
        return impl_->performRequest(method, endpoint, request, response);
    }
    catch (const Exception& e) {
        std::cerr << "ProtobufHttpClient error: " << e.what() << std::endl;
        return false;
    }
}

// 显式实例化定义
template bool ProtobufHttpClient::performRequest<google::protobuf::Empty, AccountInfoResponse>(
    const std::string&, const std::string&, const google::protobuf::Empty&, AccountInfoResponse&);

template bool ProtobufHttpClient::performRequest<google::protobuf::Empty, PositionsResponse>(
    const std::string&, const std::string&, const google::protobuf::Empty&, PositionsResponse&);

template bool ProtobufHttpClient::performRequest<PlaceOrder, PlaceOrderResponse>(
    const std::string&, const std::string&, const PlaceOrder&, PlaceOrderResponse&);

template bool ProtobufHttpClient::performRequest<CancelOrderId, CancelOrderIdResponse>(
    const std::string&, const std::string&, const CancelOrderId&, CancelOrderIdResponse&);

template bool ProtobufHttpClient::performRequest<CancelStockScope, CancelStockScopeResponse>(
    const std::string&, const std::string&, const CancelStockScope&, CancelStockScopeResponse&);
template bool ProtobufHttpClient::performRequest<Entrusts, EntrustsResponse>(
    const std::string&, const std::string&, const Entrusts&, EntrustsResponse&);
template bool ProtobufHttpClient::performRequest<Entrusts, TodayEntrustsValueResponse>(
    const std::string&, const std::string&, const Entrusts&, TodayEntrustsValueResponse&);
template bool ProtobufHttpClient::performRequest<StockPositions, PositionsResponse>(
    const std::string&, const std::string&, const StockPositions&, PositionsResponse&);


template std::unique_ptr<AccountInfoResponse> ProtobufHttpClient::get<AccountInfoResponse>(
    const std::string&);

template std::unique_ptr<PositionsResponse> ProtobufHttpClient::get<PositionsResponse>(
    const std::string&);
template std::unique_ptr<OrderResponse> ProtobufHttpClient::get<OrderResponse>(
    const std::string&);
template std::unique_ptr<TradeResponse> ProtobufHttpClient::get<TradeResponse>(
    const std::string&);
template std::unique_ptr<PlaceOrderResponse> ProtobufHttpClient::post<PlaceOrder, PlaceOrderResponse>(
    const std::string&, const PlaceOrder&);
template std::unique_ptr<CancelOrderIdResponse> ProtobufHttpClient::post<CancelOrderId, CancelOrderIdResponse>(
    const std::string&, const CancelOrderId&);
template std::unique_ptr<CancelStockScopeResponse> ProtobufHttpClient::post<CancelStockScope, CancelStockScopeResponse>(
    const std::string&, const CancelStockScope&);
template std::unique_ptr<EntrustsResponse> ProtobufHttpClient::post<Entrusts, EntrustsResponse>(
    const std::string&, const Entrusts&);
template std::unique_ptr<TodayEntrustsValueResponse> ProtobufHttpClient::post<Entrusts, TodayEntrustsValueResponse>(
    const std::string&, const Entrusts&);
template std::unique_ptr<PositionsResponse> ProtobufHttpClient::post<StockPositions, PositionsResponse>(
    const std::string&, const StockPositions&);

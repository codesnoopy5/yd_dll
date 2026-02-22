#pragma once

#include <memory>
#include <string>
#include <functional>
#include <google/protobuf/message.h>
#include <google/protobuf/empty.pb.h>
#include "little_goal.pb.h"
#include <thread> 
#include <functional>

// 前置声明使用的消息类型
class AccountInfoResponse;
class PositionsResponse;
class PlaceOrder;
class PlaceOrderResponse;
// 添加其他需要的消息类型前置声明...

class ProtobufHttpClient {
public:
    class Exception : public std::runtime_error {
    public:
        using std::runtime_error::runtime_error;
    };

    struct Config {
        std::string base_url;
        std::string ca_cert_path;
        std::string client_cert_path;
        std::string client_key_path;
        long timeout_ms = 5000;
        bool verify_ssl = true;
    };

    template <typename ResponseType>
    using AsyncCallback = std::function<void(std::unique_ptr<ResponseType>, const std::string& /* error */)>;
    explicit ProtobufHttpClient(const Config& config);


    ~ProtobufHttpClient();

    ProtobufHttpClient(const ProtobufHttpClient&) = delete;
    ProtobufHttpClient& operator=(const ProtobufHttpClient&) = delete;

    // GET请求
    template<typename ResponseType>
    std::unique_ptr<ResponseType> get(const std::string& endpoint);

    // POST请求
    template<typename RequestType, typename ResponseType>
    std::unique_ptr<ResponseType> post(const std::string& endpoint,
        const RequestType& request);

    template <typename RequestType, typename ResponseType>
    void async_post(const std::string& endpoint,
        const RequestType& request,
        AsyncCallback<ResponseType> callback)
    {
        // 复制请求数据和配置（确保线程安全）
        RequestType request_copy = request;
        Config config_copy = config_;

        // 启动独立线程处理请求
        std::thread([endpoint,
            request = std::move(request_copy),
            callback = std::move(callback),
            config_copy]() mutable
            {
                try {
                    // 创建临时客户端（使用复制的配置，线程安全）
                    ProtobufHttpClient temp_client(config_copy);

                    // 调用同步POST方法
                    auto response = temp_client.post<RequestType, ResponseType>(endpoint, request);

                    if (response) {
                        callback(std::move(response), "");
                    }
                    else {
                        callback(nullptr, "POST request failed: server returned error");
                    }
                }
                catch (const Exception& e) {
                    callback(nullptr, std::string("Exception: ") + e.what());
                }
                catch (const std::exception& e) {
                    callback(nullptr, std::string("Std exception: ") + e.what());
                }
                catch (...) {
                    callback(nullptr, "Unknown error in POST request");
                }
            }).detach();  // 分离线程，生命周期独立
    }

    // 底层请求方法
    template<typename RequestType, typename ResponseType>
    bool performRequest(const std::string& method,
        const std::string& endpoint,
        const RequestType& request,
        ResponseType& response);

    template<typename T>
    using AsyncCallback = std::function<void(std::unique_ptr<T>, const std::string&)>;


private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    Config config_;
};

// 显式实例化声明
extern template bool ProtobufHttpClient::performRequest<google::protobuf::Empty, AccountInfoResponse>(
    const std::string&, const std::string&, const google::protobuf::Empty&, AccountInfoResponse&);

extern template bool ProtobufHttpClient::performRequest<google::protobuf::Empty, PositionsResponse>(
    const std::string&, const std::string&, const google::protobuf::Empty&, PositionsResponse&);

extern template bool ProtobufHttpClient::performRequest<PlaceOrder, PlaceOrderResponse>(
    const std::string&, const std::string&, const PlaceOrder&, PlaceOrderResponse&);

extern template bool ProtobufHttpClient::performRequest<CancelOrderId, CancelOrderIdResponse>(
    const std::string&, const std::string&, const CancelOrderId&, CancelOrderIdResponse&);

extern template bool ProtobufHttpClient::performRequest<CancelStockScope, CancelStockScopeResponse>(
    const std::string&, const std::string&, const CancelStockScope&, CancelStockScopeResponse&);
extern template bool ProtobufHttpClient::performRequest<Entrusts, EntrustsResponse>(
    const std::string&, const std::string&, const Entrusts&, EntrustsResponse&);
extern template bool ProtobufHttpClient::performRequest<Entrusts, TodayEntrustsValueResponse>(
    const std::string&, const std::string&, const Entrusts&, TodayEntrustsValueResponse&);
extern template bool ProtobufHttpClient::performRequest<StockPositions, PositionsResponse>(
    const std::string&, const std::string&, const StockPositions&, PositionsResponse&);

// GET模板实例化声明
extern template std::unique_ptr<AccountInfoResponse> ProtobufHttpClient::get<AccountInfoResponse>(
    const std::string&);

extern template std::unique_ptr<PositionsResponse> ProtobufHttpClient::get<PositionsResponse>(
    const std::string&);

extern template std::unique_ptr<OrderResponse> ProtobufHttpClient::get<OrderResponse>(
    const std::string&);

extern template std::unique_ptr<TradeResponse> ProtobufHttpClient::get<TradeResponse>(
    const std::string&);


// POST模板实例化声明
extern template std::unique_ptr<PlaceOrderResponse> ProtobufHttpClient::post<PlaceOrder, PlaceOrderResponse>(
    const std::string&, const PlaceOrder&);
extern template std::unique_ptr<CancelOrderIdResponse> ProtobufHttpClient::post<CancelOrderId, CancelOrderIdResponse>(
    const std::string&, const CancelOrderId&);
extern template std::unique_ptr<CancelStockScopeResponse> ProtobufHttpClient::post<CancelStockScope, CancelStockScopeResponse>(
    const std::string&, const CancelStockScope&);
extern template std::unique_ptr<EntrustsResponse> ProtobufHttpClient::post<Entrusts, EntrustsResponse>(
    const std::string&, const Entrusts&);
extern template std::unique_ptr<TodayEntrustsValueResponse> ProtobufHttpClient::post<Entrusts, TodayEntrustsValueResponse>(
    const std::string&, const Entrusts&);
extern template std::unique_ptr<PositionsResponse> ProtobufHttpClient::post<StockPositions, PositionsResponse>(
    const std::string&, const StockPositions&);

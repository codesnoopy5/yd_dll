// YdFunc.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include "YdFunc.h"
#include "little_goal.pb.h"
#include "protobuf_http_client.hpp"
#pragma comment(lib, "ws2_32.lib")
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <iostream>
#include <atomic>
#include <filesystem>
#include <fstream>  
#include <direct.h>
#include <format> // C++20
#include "RedisClient.h"
#include "LMDBClient.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"
#include <string_view>
#include "IniReader.h"
#include <optional>
#include <string>
#include <map>

// === 资源安全访问封装 ===

// 懒汉式单例获取 Logger，避免静态初始化顺序问题
std::shared_ptr<spdlog::logger> GetLogger() {
    static std::shared_ptr<spdlog::logger> logger;
    static std::once_flag log_init_flag;
    std::call_once(log_init_flag, []() {
        try {
            // 确保 logs 目录存在
            if (!std::filesystem::exists("logs")) {
                std::filesystem::create_directory("logs");
            }
            logger = spdlog::basic_logger_mt("file_logger", "logs/app.log");
            logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
            logger->flush_on(spdlog::level::info);
        }
        catch (const spdlog::spdlog_ex& ex) {
            std::cerr << "Log init failed: " << ex.what() << std::endl;
        }
        });
    return logger;
}

// 全局配置管理
class ConfigManager {
public:
    static IniReader& getInstance() {
        static IniReader reader;
        static bool initialized = false;
        static std::mutex mtx;

        std::lock_guard<std::mutex> lock(mtx);
        if (!initialized) {
            if (!reader.load("config.ini")) {
                if (auto log = GetLogger()) {
                    log->warn("[Config] Failed to load config.ini, using defaults.");
                }
            }
            initialized = true;
        }
        return reader;
    }

    static std::optional<std::string> getStr(const std::string& section, const std::string& key) {
        return getInstance().getString(section, key);
    }

    // 获取带默认值的配置
    static std::string getStr(const std::string& section, const std::string& key, const std::string& defaultVal) {
        auto val = getInstance().getString(section, key);
        return val.value_or(defaultVal);
    }

    static int getInt(const std::string& section, const std::string& key, int defaultVal) {
        auto val = getInstance().getString(section, key);
        if (val) {
            try { return std::stoi(*val); }
            catch (...) {}
        }
        return defaultVal;
    }
};

// 全局变量管理 (通过 ConfigManager 初始化)
const std::string& GetRedisUri() {
    static std::string uri = ConfigManager::getStr("redis", "uri", "127.0.0.1:6379");
    return uri;
}

const std::string& GetDbPath() {
    static std::string path = ConfigManager::getStr("lmdb", "path", "./litg_db");
    return path;
}

const std::string& GetHttpBaseUrl() {
    static std::string url = ConfigManager::getStr("http", "base_url", "http://localhost:8000");
    return url;
}

int GetTimeoutMs() {
    return ConfigManager::getInt("http", "timeout_ms", 10000);
}

// 缓存相关的常量
const std::string STRING_BIT_PREFIX = "BLK_";
const char* redis_channel = "stock_trade"; // 保持常量

// 辅助函数
std::string convertStockCodeMarketStartWithDot(std::string code) {
    if (code.empty()) return "";
    if (code.size() < 2) return code;
    return code.insert(2, 1, '.');
}

std::string convertStockCodeMarketEnd(const std::string& code) {
    if (code.size() != 8) return code;
    std::string exchange = code.substr(0, 2);
    std::string stockNum = code.substr(2);
    return stockNum + "." + exchange;
}

// === 导出函数实现 ===
// 注意：所有导出函数均增加了 try-catch 防御

__declspec(dllexport) int WINAPI AUTO_TRADE(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;

        std::string stock_code = pData->m_strStkLabel;
        if (stock_code.size() > 6) {
            stock_code = stock_code.substr(stock_code.size() - 6);
        }

        if (pData->m_nNumParam == 4 &&
            pData->m_pParam[0] != NULL &&
            pData->m_pParam[1] != NULL &&
            pData->m_pParam[2] != NULL &&
            pData->m_pParam[3] != NULL)
        {
            double Price = pData->m_pParam[0]->m_dSingleData;
            int HowMany = (int)pData->m_pParam[1]->m_dSingleData;
            int OrderType = (int)pData->m_pParam[2]->m_dSingleData;

            if (HowMany > 0)
            {
                ProtobufHttpClient::Config config{
                    .base_url = GetHttpBaseUrl(),
                    .timeout_ms = (long)GetTimeoutMs()
                };
                ProtobufHttpClient client(config);

                PlaceOrder place_order;
                place_order.set_stock_code(stock_code.c_str());
                place_order.set_how_many(HowMany);
                place_order.set_price(Price);

                std::string order_type;
                std::string endpoint;

                if (OrderType == 1) { order_type = "buy"; endpoint = "/place_order/amount"; }
                else if (OrderType == 2) { order_type = "sell"; endpoint = "/place_order/amount"; }
                else if (OrderType == 3) { order_type = "buy"; endpoint = "/place_order/vol"; }
                else if (OrderType == 4) { order_type = "sell"; endpoint = "/place_order/vol"; }
                else if (OrderType == 5) { order_type = "buy"; endpoint = "/place_order/percent"; }
                else if (OrderType == 6) { order_type = "sell"; endpoint = "/place_order/percent"; }
                else { return -1; }

                place_order.set_order_type(order_type);

                // 异步发送并记录回调日志
                client.async_post<PlaceOrder, PlaceOrderResponse>(
                    endpoint,
                    place_order,
                    [](auto response, auto error) {
                        if (!error.empty()) {
                            if (auto log = GetLogger()) log->error("AUTO_TRADE async error: {}", error);
                        }
                        else {
                            if (auto log = GetLogger()) log->info("AUTO_TRADE async success.");
                        }
                    }
                );
            }
        }
        return 1;
    }
    catch (const std::exception& e) {
        if (auto log = GetLogger()) log->error("Exception in AUTO_TRADE: {}", e.what());
        return -1;
    }
    catch (...) {
        if (auto log = GetLogger()) log->error("Unknown exception in AUTO_TRADE");
        return -1;
    }
}


__declspec(dllexport) int WINAPI AUTO_CANCEL(DLLCALCINFO* pData)
{
    try {
        if (!pData || pData->m_dwHeadTag != YDDLL_HEADTAG) return -1;

        std::string stock_code = pData->m_strStkLabel;
        if (stock_code.size() > 6) stock_code = stock_code.substr(stock_code.size() - 6);

        if (pData->m_nNumParam == 4 &&
            pData->m_pParam[0] != NULL &&
            pData->m_pParam[1] != NULL &&
            pData->m_pParam[2] != NULL &&
            pData->m_pParam[3] != NULL)
        {
            int CancelType = (int)pData->m_pParam[0]->m_dSingleData;
            int CancelScope = (int)pData->m_pParam[1]->m_dSingleData;

            ProtobufHttpClient::Config config{
             .base_url = GetHttpBaseUrl(),
             .timeout_ms = (long)GetTimeoutMs()
            };
            ProtobufHttpClient client(config);

            std::string endpoint = "/cancel/stock_scope";
            CancelStockScope cancel_stock_scope;
            std::string order_type;
            std::string target_code;

            if (CancelScope == 0) return -1;

            if (CancelType == 1) order_type = "buy";
            else if (CancelType == 2) order_type = "sell";
            else if (CancelType == 3) order_type = "all";
            else return 1;

            if (CancelScope == 1) target_code = stock_code;
            else if (CancelScope == 2) target_code = "all";
            else return 1;

            cancel_stock_scope.set_order_type(order_type);
            cancel_stock_scope.set_stock_code(target_code);

            client.async_post<CancelStockScope, CancelStockScopeResponse>(
                endpoint,
                cancel_stock_scope,
                [](auto response, auto error) {
                    if (!error.empty()) {
                        if (auto log = GetLogger()) log->error("AUTO_CANCEL async error: {}", error);
                    }
                }
            );
        }
        return 1;
    }
    catch (const std::exception& e) {
        if (auto log = GetLogger()) log->error("Exception in AUTO_CANCEL: {}", e.what());
        return -1;
    }
}


__declspec(dllexport) int WINAPI STOCK_POSITIONS(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;
        LMDBClient& db = LMDBClient::GetInstance();
        db.Initialize(GetDbPath(), 100, false);

        std::string stock_code = pData->m_strStkLabel;
        stock_code = convertStockCodeMarketStartWithDot(stock_code);
        std::string parent = "positions";

        double vol = 0;
        db.GetDouble(parent + ":" + stock_code + ":" + "vol", &vol);

        double available_vol = 0;
        db.GetDouble(parent + ":" + stock_code + ":" + "available_vol", &available_vol);

        double avg_cost = 0;
        db.GetDouble(parent + ":" + stock_code + ":" + "avg_cost", &avg_cost);

        pData->m_pResultBuf[pData->m_nNumData - 1] = available_vol;
        pData->m_pResultBuf[pData->m_nNumData - 2] = vol;
        pData->m_pResultBuf[pData->m_nNumData - 3] = avg_cost;

        return 1;
    }
    catch (...) { return -1; }
}


__declspec(dllexport) int WINAPI ACCOUNT_ALL(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;
        LMDBClient& db = LMDBClient::GetInstance();
        db.Initialize(GetDbPath(), 100, false);

        std::string parent = "account";

        double total_asset = 0, frozen_cash = 0, cash = 0, market_value = 0;
        db.GetDouble(parent + ":" + "total_asset", &total_asset);
        db.GetDouble(parent + ":" + "frozen_cash", &frozen_cash);
        db.GetDouble(parent + ":" + "cash", &cash);
        db.GetDouble(parent + ":" + "market_value", &market_value);

        pData->m_pResultBuf[pData->m_nNumData - 1] = cash;
        pData->m_pResultBuf[pData->m_nNumData - 2] = frozen_cash;
        pData->m_pResultBuf[pData->m_nNumData - 3] = market_value;
        pData->m_pResultBuf[pData->m_nNumData - 4] = total_asset;

        return 1;
    }
    catch (...) { return -1; }
}


__declspec(dllexport) int WINAPI ADD_TO_BLOCK(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;
        LMDBClient& db = LMDBClient::GetInstance();
        db.Initialize(GetDbPath(), 100, false);

        std::string stock_code = pData->m_strStkLabel;
        if (stock_code.size() > 6) stock_code = stock_code.substr(stock_code.size() - 6);

        if (pData->m_nNumParam >= 1 && pData->m_pParam[0] != NULL)
        {
            int index1 = (int)pData->m_pParam[0]->m_dSingleData;
            std::string parent = "blk_size:";
            if (index1 >= 0) {
                db.AtomicSetStringBit(STRING_BIT_PREFIX + stock_code, index1, '1');
                db.AtomicIncrement(parent + std::to_string(index1), 1);
            }
        }
        return 1;
    }
    catch (...) { return -1; }
}

__declspec(dllexport) int WINAPI IS_IN_BLOCK(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;
        LMDBClient& db = LMDBClient::GetInstance();
        db.Initialize(GetDbPath(), 100, false);

        std::string stock_code = pData->m_strStkLabel;
        if (stock_code.size() > 6) stock_code = stock_code.substr(stock_code.size() - 6);

        if (pData->m_nNumParam >= 1 && pData->m_pParam[0] != NULL)
        {
            int index1 = (int)pData->m_pParam[0]->m_dSingleData;
            if (index1 >= 0) {
                bool bit_val = false;
                db.GetStringBit(STRING_BIT_PREFIX + stock_code, index1, &bit_val);
                pData->m_pResultBuf[pData->m_nNumData - 1] = (bit_val ? 1 : 0);
            }
        }
        return 1;
    }
    catch (...) { return -1; }
}

__declspec(dllexport) int WINAPI RESET_STATUS(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;
        LMDBClient& db = LMDBClient::GetInstance();
        db.Initialize(GetDbPath(), 100, false);

        if (pData->m_nNumParam >= 3 && pData->m_pParam[0] && pData->m_pParam[1] && pData->m_pParam[2])
        {

            const char* p1_str = pData->m_pParam[0]->m_pszText;
            if (!p1_str) return -1;

            if (strcmp(p1_str, "block") == 0) {
                db.DeleteKeys(STRING_BIT_PREFIX);
                db.DeleteKeys("blk_size");
            }
            else {
                db.DeleteKeys(p1_str);
            }
        }
        return 1;
    }
    catch (...) { return -1; }
}

__declspec(dllexport) int WINAPI GET_KEY(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;

        if (pData->m_nNumParam == 4 &&
            pData->m_pParam[0] != NULL &&
            pData->m_pParam[1] != NULL &&
            pData->m_pParam[2] != NULL &&
            pData->m_pParam[3] != NULL)
        {
            const char* acc_str = pData->m_pParam[3]->m_pszText;
            if (!acc_str) return -1;

            auto account_id_opt = ConfigManager::getStr("ths_account", acc_str);
            if (!account_id_opt) {
                if (auto log = GetLogger()) log->error("读取同花顺资金账号失败: {}", acc_str);
                return 0;
            }

            std::string ths_acount_id = account_id_opt.value();
            LMDBClient& db = LMDBClient::GetInstance();
            db.Initialize(GetDbPath(), 100, false);

      
            int key_int = (int)pData->m_pParam[0]->m_dSingleData;

            std::string key = "";
            const char* key_str = pData->m_pParam[0]->m_pszText;

            if (!key_str) {
                key = std::to_string(key_int);
            }
            else {
                key = key_str;
            }

            double current_val = 0;
            std::string parent = std::format("key:{}:", ths_acount_id);
            db.GetDouble(parent + key, &current_val);
            pData->m_pResultBuf[pData->m_nNumData - 1] = current_val;
            
            return 1;
        }
    }
    catch (...) { return -1; }
}

__declspec(dllexport) int WINAPI ADD_KEY(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;

        if (pData->m_nNumParam == 4 &&
            pData->m_pParam[0] != NULL &&
            pData->m_pParam[1] != NULL &&
            pData->m_pParam[2] != NULL &&
            pData->m_pParam[3] != NULL)
        {
            const char* acc_str = pData->m_pParam[3]->m_pszText;
            if (!acc_str) return -1;

            auto account_id_opt = ConfigManager::getStr("ths_account", acc_str);
            if (!account_id_opt) {
                if (auto log = GetLogger()) log->error("读取同花顺资金账号失败: {}", acc_str);
                return 0;
            }

            std::string ths_acount_id = account_id_opt.value();


            LMDBClient& db = LMDBClient::GetInstance();
            db.Initialize(GetDbPath(), 100, false);

            int key_int = (int)pData->m_pParam[0]->m_dSingleData;

            std::string key = "";
            const char* key_str = pData->m_pParam[0]->m_pszText;

            if (!key_str) {
                key = std::to_string(key_int);
            }
            else {
                key = key_str;
            }

            double value = (double)pData->m_pParam[1]->m_dSingleData;

            std::string parent = std::format("key:{}:", ths_acount_id);
            db.AtomicIncrementDouble(parent + key, value);
        }
        return 1;
    }
    catch (...) { return -1; }
}

__declspec(dllexport) int WINAPI SET_KEY(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;

        if (pData->m_nNumParam == 4 &&
            pData->m_pParam[0] != NULL &&
            pData->m_pParam[1] != NULL &&
            pData->m_pParam[2] != NULL &&
            pData->m_pParam[3] != NULL)
        {
            const char* acc_str = pData->m_pParam[3]->m_pszText;
            if (!acc_str) return -1;



            auto account_id_opt = ConfigManager::getStr("ths_account", acc_str);
            if (!account_id_opt) {
                if (auto log = GetLogger()) log->error("读取同花顺资金账号失败: {}", acc_str);
                return 0;
            }

            std::string ths_acount_id = account_id_opt.value();


            LMDBClient& db = LMDBClient::GetInstance();
            db.Initialize(GetDbPath(), 100, false);


            int key_int = (int)pData->m_pParam[0]->m_dSingleData;

            std::string key = "";
            const char* key_str = pData->m_pParam[0]->m_pszText;

            if (!key_str) {
                key = std::to_string(key_int);
            }
            else {
                key = key_str;
            }

            double value = pData->m_pParam[1]->m_dSingleData;
            std::string parent = std::format("key:{}:", ths_acount_id);
            db.PutDouble(parent + key, value);

            return 1;
        }
    }
    catch (...) { return -1; }
    
}

__declspec(dllexport) int WINAPI DEL_KEY(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;

        if (pData->m_nNumParam == 4 &&
            pData->m_pParam[0] != NULL &&
            pData->m_pParam[1] != NULL &&
            pData->m_pParam[2] != NULL &&
            pData->m_pParam[3] != NULL)
        {
            const char* acc_str = pData->m_pParam[3]->m_pszText;
            if (!acc_str) return -1;



            auto account_id_opt = ConfigManager::getStr("ths_account", acc_str);
            if (!account_id_opt) {
                if (auto log = GetLogger()) log->error("读取同花顺资金账号失败: {}", acc_str);
                return 0;
            }

            std::string ths_acount_id = account_id_opt.value();


            LMDBClient& db = LMDBClient::GetInstance();
            db.Initialize(GetDbPath(), 100, false);


            int key_int = (int)pData->m_pParam[0]->m_dSingleData;

            std::string key = "";
            const char* key_str = pData->m_pParam[0]->m_pszText;

            if (!key_str) {
                key = std::to_string(key_int);
            }
            else {
                key = key_str;
            }


            std::string parent = std::format("key:{}:", ths_acount_id);
            db.Delete(parent + key);

            return 1;
        }
    }
    catch (...) { return -1; }

}

__declspec(dllexport) int WINAPI GET_BLOCK_SIZE(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;
        LMDBClient& db = LMDBClient::GetInstance();
        db.Initialize(GetDbPath(), 100, false);

        if (pData->m_nNumParam >= 1 && pData->m_pParam[0])
        {
            int key1 = (int)pData->m_pParam[0]->m_dSingleData;
            int current_val = 0;
            std::string parent = "blk_size:";
            db.GetInt(parent + std::to_string(key1), &current_val);
            pData->m_pResultBuf[pData->m_nNumData - 1] = current_val;
        }
        return 1;
    }
    catch (...) { return -1; }
}

__declspec(dllexport) int WINAPI ASK_BID(DLLCALCINFO* pData)
{
    try {
        if (!pData || pData->m_dwHeadTag != YDDLL_HEADTAG) return -1;

        std::string stock_code = pData->m_strStkLabel;
        if (stock_code.size() > 6) stock_code = stock_code.substr(stock_code.size() - 6);

        if (pData->m_nNumParam >= 3 && pData->m_pParam[0] && pData->m_pParam[1] && pData->m_pParam[2])
        {
            int TradeType = (int)pData->m_pParam[0]->m_dSingleData;
            int isEnable = (int)pData->m_pParam[1]->m_dSingleData;
            int DataType = (int)pData->m_pParam[2]->m_dSingleData;

            if (isEnable == 0) return -1;

            ProtobufHttpClient::Config config{
               .base_url = GetHttpBaseUrl(),
               .timeout_ms = (long)GetTimeoutMs()
            };
            ProtobufHttpClient client(config);

            Entrusts entrusts;
            entrusts.set_stock_code(stock_code);
            if (DataType == 1) entrusts.set_data_type("vol");
            else if (DataType == 2) entrusts.set_data_type("amount");
            else return 1;

            if (TradeType == 1) entrusts.set_trade_type("buy");
            else if (TradeType == 2) entrusts.set_trade_type("sell");
            else return 1;

            auto entrustsResponse = client.post<Entrusts, EntrustsResponse>("/entrusts", entrusts);

            if (entrustsResponse) {
                if (entrustsResponse->status() == "success") {
                    pData->m_pResultBuf[pData->m_nNumData - 1] = entrustsResponse->result();
                }
            }
        }
        return 1;
    }
    catch (...) { return -1; }
}

__declspec(dllexport) int WINAPI TODAY_ENTRUSTS(DLLCALCINFO* pData)
{
    try {
        if (!pData) return -1;

        if (pData->m_nNumParam >= 2 && pData->m_pParam[0] && pData->m_pParam[1])
        {
            int TradeType = (int)pData->m_pParam[0]->m_dSingleData;
            int EntrustStatus = (int)pData->m_pParam[1]->m_dSingleData;

            ProtobufHttpClient::Config config{
               .base_url = GetHttpBaseUrl(),
               .timeout_ms = (long)GetTimeoutMs()
            };
            ProtobufHttpClient client(config);

            Entrusts entrusts;
            if (TradeType == 1) entrusts.set_trade_type("buy");
            else if (TradeType == 2) entrusts.set_trade_type("sell");
            else {
                pData->m_pResultBuf[pData->m_nNumData - 1] = 0;
                return 1;
            }

            auto todayEntrustsValueResponse = client.post<Entrusts, TodayEntrustsValueResponse>("/today_entrusts_value", entrusts);

            if (todayEntrustsValueResponse && todayEntrustsValueResponse->status() == "success") {
                if (EntrustStatus == 1) pData->m_pResultBuf[pData->m_nNumData - 1] = todayEntrustsValueResponse->envalue();
                else if (EntrustStatus == 2) pData->m_pResultBuf[pData->m_nNumData - 1] = todayEntrustsValueResponse->unvalue();
                else pData->m_pResultBuf[pData->m_nNumData - 1] = 0;
            }
            else {
                pData->m_pResultBuf[pData->m_nNumData - 1] = 0;
            }
        }
        else {
            pData->m_pResultBuf[pData->m_nNumData - 1] = 0;
        }
        return 1;
    }
    catch (...) { return -1; }
}
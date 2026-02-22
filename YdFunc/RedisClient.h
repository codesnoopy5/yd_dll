#ifndef REDIS_CLIENT_H
#define REDIS_CLIENT_H

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libevent.h>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <functional>
#include <thread>
#include <unordered_map>
#include <condition_variable>
#include <queue>
#include <stdexcept>
#include <cstddef>
#include <unordered_set>

// 订阅回调函数类型
using SubscribeCallback = std::function<void(const std::string& channel, const std::string& message)>;

class RedisClient {
public:
    RedisClient(const RedisClient&) = delete;
    RedisClient& operator=(const RedisClient&) = delete;

    static RedisClient& GetInstance() {
        static RedisClient instance;
        return instance;
    }

    bool Initialize(const std::string& connection_string, bool read_only = true);
    static RedisClient& GetInstanceAndInitialize(const std::string& connection_string, bool read_only = true);

    bool Get(const std::string& key, std::string* value);
    bool GetDouble(const std::string& key, double* value);
    bool Exists(const std::string& key);
    std::vector<std::string> GetKeys(const std::string& prefix = "", size_t max_keys = 10000);

    bool Put(const std::string& key, const std::string& value);
    bool PutDouble(const std::string& key, double value);
    bool PutSlice(const std::string& key, const std::string& value);
    bool Delete(const std::string& key);

    bool WriteBatch(const std::vector<std::pair<std::string, std::string>>& puts, const std::vector<std::string>& deletes);
    bool WriteBatchDouble(const std::vector<std::pair<std::string, double>>& puts, const std::vector<std::string>& deletes);

    bool AtomicIncrement(const std::string& key, int64_t delta);
    bool AtomicGetInt64(const std::string& key, int64_t* value);
    bool AtomicGetDouble(const std::string& key, double* value);
    bool AtomicSetStringBit(const std::string& key, size_t index, char value);

    bool WriteBatchIncrement(const std::vector<std::pair<std::string, int64_t>>& increments,
        const std::vector<std::pair<std::string, std::string>>& puts = {},
        const std::vector<std::string>& deletes = {});

    bool Publish(const std::string& channel, const std::string& message);
    bool Subscribe(const std::string& channel, SubscribeCallback callback);
    bool Unsubscribe(const std::string& channel);
    bool UnsubscribeAll();
    bool IsSubscribed(const std::string& channel) const;
    std::vector<std::string> GetSubscribedChannels() const;

    void SetMergeOperatorForPrefix(const std::string& prefix, const std::string& type, size_t param = 0);
    void Close();

    bool IsReadOnly() const { return read_only_; }
    bool IsInitialized() const { return initialized_; }

private:
    RedisClient();
    ~RedisClient();

    static redisReply* ExecuteCommand(redisContext* context, const char* format, ...);
    static bool CheckReply(redisReply* reply);

    void StartSubscriberThread();
    void StopSubscriberThread();
    void SubscriberLoop();
    void ProcessSubscriptionMessage(redisReply* reply);

    std::unique_ptr<redisContext, decltype(&redisFree)> context_;
    std::unique_ptr<redisAsyncContext, decltype(&redisAsyncFree)> async_context_;
    std::string connection_string_; // Store for subscriber thread

    bool initialized_ = false;
    bool read_only_ = true;
    mutable std::mutex mutex_;

    bool subscriber_thread_running_ = false;
    std::unique_ptr<std::thread> subscriber_thread_;
    mutable std::mutex subscription_mutex_;
    std::unordered_map<std::string, SubscribeCallback> subscriptions_;
    std::condition_variable subscription_cv_;
    bool subscription_changed_ = false;
};

#endif // REDIS_CLIENT_H
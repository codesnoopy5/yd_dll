#include "RedisClient.h"
#include <cstring>
#include <cstdio>
#include <cctype>
#include <stdexcept>
#include <iostream>
#include <algorithm>
#include <memory>
#include <chrono>
#include <thread>

redisReply* RedisClient::ExecuteCommand(redisContext* context, const char* format, ...) {
    if (!context) return nullptr;
    va_list ap;
    va_start(ap, format);
    redisReply* reply = static_cast<redisReply*>(redisvCommand(context, format, ap));
    va_end(ap);
    return reply;
}

bool RedisClient::CheckReply(redisReply* reply) {
    if (reply == nullptr) return false;
    if (reply->type == REDIS_REPLY_ERROR) {
        freeReplyObject(reply);
        return false;
    }
    return true;
}

RedisClient::RedisClient()
    : context_(nullptr, redisFree)
    , async_context_(nullptr, redisAsyncFree) {
}

RedisClient::~RedisClient() {
    Close();
}
bool RedisClient::Initialize(const std::string& connection_string, bool read_only) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return true;

    read_only_ = read_only;
    connection_string_ = connection_string;

    size_t colon_pos = connection_string.find(':');
    if (colon_pos == std::string::npos) return false;

    std::string host = connection_string.substr(0, colon_pos);
    int port = 0;
    try {
        port = std::stoi(connection_string.substr(colon_pos + 1));
    }
    catch (...) { return false; }

    context_ = std::unique_ptr<redisContext, decltype(&redisFree)>(
        redisConnect(host.c_str(), port),
        redisFree
    );

    if (context_ == nullptr || context_->err) {
        return false;
    }

    redisReply* ping_reply = ExecuteCommand(context_.get(), "PING");
    if (!CheckReply(ping_reply)) {
        context_.reset();
        return false;
    }
    freeReplyObject(ping_reply);

    initialized_ = true;
    return true;
}

RedisClient& RedisClient::GetInstanceAndInitialize(const std::string& connection_string, bool read_only) {
    RedisClient& client = GetInstance();
    client.Initialize(connection_string, read_only);
    return client;
}

bool RedisClient::Get(const std::string& key, std::string* value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return false;

    redisReply* reply = ExecuteCommand(context_.get(), "GET %b", key.data(), key.size());
    if (!CheckReply(reply)) return false;

    if (reply->type == REDIS_REPLY_NIL) {
        freeReplyObject(reply);
        return false;
    }

    if (value) *value = std::string(reply->str, reply->len);
    freeReplyObject(reply);
    return true;
}
bool RedisClient::Exists(const std::string& key) {
    std::string value;
    return Get(key, &value);
}

std::vector<std::string> RedisClient::GetKeys(const std::string& prefix, size_t max_keys) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return {};
    }

    std::vector<std::string> keys;
    std::string pattern = prefix.empty() ? "*" : prefix + "*";

    // Limit pattern length to prevent stack overflow
    if (pattern.size() > 1000) {
        throw std::invalid_argument("Prefix too long (max 1000 chars)");
    }

    // Use SCAN to get all keys matching the pattern
    long long cursor = 0;
    size_t total_keys = 0;

    do {
        redisReply* reply = ExecuteCommand(context_.get(), "SCAN %lld MATCH %s COUNT 100", cursor, pattern.c_str());
        if (!CheckReply(reply)) {
            freeReplyObject(reply);
            return {};
        }

        // Process the reply
        if (reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2) {
            redisReply* cursor_reply = reply->element[0];
            redisReply* keys_reply = reply->element[1];

            if (cursor_reply->type == REDIS_REPLY_STRING &&
                keys_reply->type == REDIS_REPLY_ARRAY) {

                cursor = std::stoll(std::string(cursor_reply->str, cursor_reply->len));

                // Limit total number of keys
                size_t keys_to_add = std::min<size_t>(keys_reply->elements, max_keys - total_keys);
                for (size_t i = 0; i < keys_to_add; i++) {
                    if (keys_reply->element[i]->type == REDIS_REPLY_STRING) {
                        keys.push_back(std::string(keys_reply->element[i]->str, keys_reply->element[i]->len));
                    }
                }
                total_keys += keys_to_add;

                // Stop if we've reached the maximum
                if (total_keys >= max_keys) {
                    break;
                }
            }
        }

        freeReplyObject(reply);
    } while (cursor != 0 && total_keys < max_keys);

    return keys;
}

bool RedisClient::Put(const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || read_only_) return false;
    redisReply* reply = ExecuteCommand(context_.get(), "SET %b %b", key.data(), key.size(), value.data(), value.size());
    bool success = CheckReply(reply);
    freeReplyObject(reply);
    return success;
}

bool RedisClient::PutSlice(const std::string& key, const std::string& value) {
    return Put(key, value);
}

bool RedisClient::Delete(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return false;
    }

    if (read_only_) {
        return false;
    }

    redisReply* reply = ExecuteCommand(context_.get(), "DEL %b", key.data(), key.size());
    bool success = CheckReply(reply);
    freeReplyObject(reply);
    return success;
}

bool RedisClient::GetDouble(const std::string& key, double* value) {
    std::string data;
    if (!Get(key, &data)) {
        if (value) *value = 0;
        return false;
    }
    try {
        if (value) *value = std::stod(data);
        return true;
    }
    catch (...) {
        if (value) *value = 0;
        return false;
    }
}

bool RedisClient::PutDouble(const std::string& key, double value) {
    // 将 double 转换为字符串存储（保留小数点后6位）
    std::string data = std::to_string(value);

    // 以字符串方式存储到 Redis
    return Put(key, data);
}

bool RedisClient::WriteBatch(const std::vector<std::pair<std::string, std::string>>& puts,
    const std::vector<std::string>& deletes) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return false;
    }

    if (read_only_) {
        return false;
    }

    // Use MULTI/EXEC for atomic batch operations
    redisReply* reply = ExecuteCommand(context_.get(), "MULTI");
    if (!CheckReply(reply)) {
        return false;
    }
    freeReplyObject(reply);

    // Execute PUT operations
    for (const auto& kv : puts) {
        reply = ExecuteCommand(context_.get(), "SET %b %b",
            kv.first.data(), kv.first.size(),
            kv.second.data(), kv.second.size());
        if (!CheckReply(reply)) {
            freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // Execute DELETE operations
    for (const auto& key : deletes) {
        reply = ExecuteCommand(context_.get(), "DEL %b", key.data(), key.size());
        if (!CheckReply(reply)) {
            freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // Execute the batch
    reply = ExecuteCommand(context_.get(), "EXEC");
    if (!CheckReply(reply)) {
        freeReplyObject(reply);
        return false;
    }

    // Check if all operations succeeded
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements > 0) {
        // If any command failed, reply will be an error
        for (size_t i = 0; i < reply->elements; i++) {
            if (reply->element[i]->type == REDIS_REPLY_ERROR) {
                freeReplyObject(reply);
                return false;
            }
        }
    }

    freeReplyObject(reply);
    return true;
}

bool RedisClient::WriteBatchDouble(const std::vector<std::pair<std::string, double>>& puts,
    const std::vector<std::string>& deletes) {
    std::vector<std::pair<std::string, std::string>> string_puts;
    for (const auto& kv : puts) {
        std::string data(reinterpret_cast<const char*>(&kv.second), sizeof(double));
        string_puts.emplace_back(kv.first, data);
    }
    return WriteBatch(string_puts, deletes);
}

bool RedisClient::AtomicIncrement(const std::string& key, int64_t delta) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return false;
    }

    if (read_only_) {
        return false;
    }

    redisReply* reply = ExecuteCommand(context_.get(), "INCRBY %b %lld",
        key.data(), key.size(), delta);
    bool success = CheckReply(reply);
    freeReplyObject(reply);
    return success;
}

bool RedisClient::AtomicGetInt64(const std::string& key, int64_t* value) {
    std::string str_value;
    if (!Get(key, &str_value)) {
        return false;
    }

    if (str_value.empty()) {
        return false;
    }

    try {
        *value = std::stoll(str_value);
    }
    catch (...) {
        return false;
    }

    return true;
}

bool RedisClient::AtomicGetDouble(const std::string& key, double* value) {
    return GetDouble(key, value);
}

bool RedisClient::AtomicSetStringBit(const std::string& key, size_t index, char value) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return false;
    }

    if (read_only_) {
        return false;
    }

    if (value != '0' && value != '1') {
        return false;
    }

    int bit_value = (value == '1') ? 1 : 0;
    redisReply* reply = ExecuteCommand(context_.get(), "SETBIT %b %lld %d",
        key.data(), key.size(), index, bit_value);
    bool success = CheckReply(reply);
    freeReplyObject(reply);
    return success;
}

bool RedisClient::WriteBatchIncrement(const std::vector<std::pair<std::string, int64_t>>& increments,
    const std::vector<std::pair<std::string, std::string>>& puts,
    const std::vector<std::string>& deletes) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return false;
    }

    if (read_only_) {
        return false;
    }

    // Use MULTI/EXEC for atomic batch operations
    redisReply* reply = ExecuteCommand(context_.get(), "MULTI");
    if (!CheckReply(reply)) {
        return false;
    }
    freeReplyObject(reply);

    // Execute INCR operations
    for (const auto& inc : increments) {
        reply = ExecuteCommand(context_.get(), "INCRBY %b %lld",
            inc.first.data(), inc.first.size(), inc.second);
        if (!CheckReply(reply)) {
            freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // Execute PUT operations
    for (const auto& kv : puts) {
        reply = ExecuteCommand(context_.get(), "SET %b %b",
            kv.first.data(), kv.first.size(),
            kv.second.data(), kv.second.size());
        if (!CheckReply(reply)) {
            freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // Execute DELETE operations
    for (const auto& key : deletes) {
        reply = ExecuteCommand(context_.get(), "DEL %b", key.data(), key.size());
        if (!CheckReply(reply)) {
            freeReplyObject(reply);
            return false;
        }
        freeReplyObject(reply);
    }

    // Execute the batch
    reply = ExecuteCommand(context_.get(), "EXEC");
    if (!CheckReply(reply)) {
        freeReplyObject(reply);
        return false;
    }

    // Check if all operations succeeded
    if (reply->type == REDIS_REPLY_ARRAY && reply->elements > 0) {
        for (size_t i = 0; i < reply->elements; i++) {
            if (reply->element[i]->type == REDIS_REPLY_ERROR) {
                freeReplyObject(reply);
                return false;
            }
        }
    }

    freeReplyObject(reply);
    return true;
}

void RedisClient::SetMergeOperatorForPrefix(const std::string& prefix, const std::string& type, size_t param) {
    // Redis doesn't require prefix-based merge operators
    // This is a no-op for Redis
}

// Pub/Sub Implementation

bool RedisClient::Publish(const std::string& channel, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        return false;
    }

    if (read_only_) {
        return false;
    }

    redisReply* reply = ExecuteCommand(context_.get(), "PUBLISH %b %b",
        channel.data(), channel.size(),
        message.data(), message.size());
    bool success = CheckReply(reply);
    freeReplyObject(reply);
    return success;
}

bool RedisClient::Subscribe(const std::string& channel, SubscribeCallback callback) {
    if (!initialized_) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        subscriptions_[channel] = callback;
        subscription_changed_ = true;
    }

    subscription_cv_.notify_all();

    // Start subscriber thread if not running
    if (!subscriber_thread_running_) {
        StartSubscriberThread();
    }

    return true;
}

bool RedisClient::Unsubscribe(const std::string& channel) {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    auto it = subscriptions_.find(channel);
    if (it != subscriptions_.end()) {
        subscriptions_.erase(it);
        subscription_changed_ = true;
        subscription_cv_.notify_all();
        return true;
    }
    return false;
}

bool RedisClient::UnsubscribeAll() {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    subscriptions_.clear();
    subscription_changed_ = true;
    subscription_cv_.notify_all();
    return true;
}

bool RedisClient::IsSubscribed(const std::string& channel) const {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    return subscriptions_.find(channel) != subscriptions_.end();
}

std::vector<std::string> RedisClient::GetSubscribedChannels() const {
    std::lock_guard<std::mutex> lock(subscription_mutex_);
    std::vector<std::string> channels;
    channels.reserve(subscriptions_.size());
    for (const auto& pair : subscriptions_) {
        channels.push_back(pair.first);
    }
    return channels;
}

void RedisClient::StartSubscriberThread() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!subscriber_thread_running_) {
        subscriber_thread_running_ = true;
        subscriber_thread_ = std::make_unique<std::thread>(&RedisClient::SubscriberLoop, this);
    }
}

void RedisClient::StopSubscriberThread() {
    {
        std::lock_guard<std::mutex> lock(subscription_mutex_);
        subscriber_thread_running_ = false;
        subscription_changed_ = true;
    }
    subscription_cv_.notify_all();

    if (subscriber_thread_ && subscriber_thread_->joinable()) {
        subscriber_thread_->join();
        subscriber_thread_.reset();
    }
}

void RedisClient::SubscriberLoop() {
    // 获取主连接的地址信息（这里简化为硬编码，实际可从配置获取）
    redisContext* sub_context = redisConnect("127.0.0.1", 6379);
    if (!sub_context || sub_context->err) {
        if (sub_context) {
            std::cerr << "Subscriber connection error: " << sub_context->errstr << std::endl;
            redisFree(sub_context);
        }
        return;
    }

    std::unordered_set<std::string> current_channels;

    while (subscriber_thread_running_) {
        // 同步当前订阅列表
        std::unordered_set<std::string> target_channels;
        {
            std::lock_guard<std::mutex> lock(subscription_mutex_);
            for (const auto& pair : subscriptions_) {
                target_channels.insert(pair.first);
            }
        }

        // 订阅新增频道
        for (const auto& ch : target_channels) {
            if (current_channels.find(ch) == current_channels.end()) {
                redisReply* r = static_cast<redisReply*>(redisCommand(sub_context, "SUBSCRIBE %s", ch.c_str()));
                if (r) {
                    std::cout << "[Subscriber] Subscribed to: " << ch << std::endl;
                    freeReplyObject(r);
                }
                current_channels.insert(ch);
            }
        }

        // 退订已删除频道
        std::vector<std::string> to_unsub;
        for (const auto& ch : current_channels) {
            if (target_channels.find(ch) == target_channels.end()) {
                to_unsub.push_back(ch);
            }
        }
        for (const auto& ch : to_unsub) {
            redisReply* r = static_cast<redisReply*>(redisCommand(sub_context, "UNSUBSCRIBE %s", ch.c_str()));
            if (r) {
                std::cout << "[Subscriber] Unsubscribed from: " << ch << std::endl;
                freeReplyObject(r);
            }
            current_channels.erase(ch);
        }

        // 如果没有任何订阅，短暂休眠后重试
        if (current_channels.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // 👇 关键：阻塞读取下一条消息（不设超时！）
        redisReply* reply = nullptr;
        int result = redisGetReply(sub_context, (void**)&reply);

        if (result != REDIS_OK || !reply) {
            // 连接出错，退出
            if (reply) freeReplyObject(reply);
            break;
        }

        // 处理消息
        ProcessSubscriptionMessage(reply);
        freeReplyObject(reply);
    }

    // 清理：退订所有频道
    for (const auto& ch : current_channels) {
        redisCommand(sub_context, "UNSUBSCRIBE %s", ch.c_str());
    }

    redisFree(sub_context);
}
void RedisClient::ProcessSubscriptionMessage(redisReply* reply) {
    if (!reply || reply->type != REDIS_REPLY_ARRAY || reply->elements < 3) {
        return;
    }

    if (reply->element[0]->type == REDIS_REPLY_STRING) {
        std::string type(reply->element[0]->str, reply->element[0]->len);
        if (type == "message" && reply->elements >= 3) {
            std::string channel(reply->element[1]->str, reply->element[1]->len);
            std::string message(reply->element[2]->str, reply->element[2]->len);

            SubscribeCallback callback;
            {
                std::lock_guard<std::mutex> lock(subscription_mutex_);
                auto it = subscriptions_.find(channel);
                if (it != subscriptions_.end()) {
                    callback = it->second;
                }
            }

            if (callback) {
                try {
                    callback(channel, message);
                }
                catch (...) {
                    // 忽略回调异常
                }
            }
        }
    }
}
void RedisClient::Close() {
    // Stop subscriber thread first
    StopSubscriberThread();

    std::lock_guard<std::mutex> lock(mutex_);
    if (context_) {
        context_.reset();
        initialized_ = false;
    }
}
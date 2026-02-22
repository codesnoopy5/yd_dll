#ifndef LMDB_CLIENT_H
#define LMDB_CLIENT_H

#include <string>
#include <vector>
#include <shared_mutex> // 替换 mutex 为 shared_mutex
#include <cstdint>
#include <lmdb.h>
#include <stdexcept>

class LMDBClient {
public:
    static constexpr int MAX_BIT_INDEX = 255;

    LMDBClient(const LMDBClient&) = delete;
    LMDBClient& operator=(const LMDBClient&) = delete;

    static LMDBClient& GetInstance() {
        static LMDBClient instance;
        return instance;
    }

    LMDBClient() = default;

    // === 转换函数 ===
    // 优化：内联简单转换，减少调用开销
    static std::string DoubleToBytes(double value);
    static double BytesToDouble(const std::string& bytes);
    static std::string IntToBytes(int value);
    static int BytesToInt(const std::string& bytes);

    bool Initialize(const std::string& db_path, size_t map_size_mb = 100, bool read_only = false);
    void Close();
    ~LMDBClient();

    // === 核心操作 ===
    // 使用 std::string_view 避免部分拷贝 (C++17)，为了兼容性这里仍用 string& 但内部优化
    bool Put(const std::string& key, const std::string& value);
    bool PutDouble(const std::string& key, double value);

    bool Get(const std::string& key, std::string* value);
    bool GetDouble(const std::string& key, double* value);
    bool GetInt(const std::string& key, int* value);

    bool Exists(const std::string& key);
    bool Delete(const std::string& key);

    bool AtomicSetStringBit(const std::string& key, int bit_index, bool set);
    bool GetStringBit(const std::string& key, int bit_index, bool* bit_value);

    bool AtomicIncrement(const std::string& key, int increment);
    bool AtomicIncrementDouble(const std::string& key, double increment);

    bool WriteBatch(const std::vector<std::pair<std::string, std::string>>& puts, const std::vector<std::string>& deletes = {});
    bool WriteBatchDouble(const std::vector<std::pair<std::string, double>>& puts, const std::vector<std::string>& deletes = {});

    // 清空整个数据库
    bool DeleteDatabase();
    // 删除所有以 prefix 开头的 key
    bool DeleteKeys(const std::string& prefix);

    std::vector<std::string> GetKeys(const std::string& prefix = "");

private:
    // 拆分读写事务执行器以利用不同的锁
    template<typename Func>
    bool ExecuteReadTransaction(Func&& func);

    template<typename Func>
    bool ExecuteWriteTransaction(Func&& func);

    mutable std::shared_mutex mutex_; // 核心优化：读写锁
    MDB_env* env_ = nullptr;
    MDB_dbi dbi_ = 0;
    bool initialized_ = false;
    bool read_only_ = false;
};

#endif // LMDB_CLIENT_H
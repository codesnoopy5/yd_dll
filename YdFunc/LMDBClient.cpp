#include "LMDBClient.h"
#include <filesystem>
#include <cstring>
#include <stdexcept>
#include <iostream>
#include <algorithm>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/stat.h>
#endif

void CreateDirIfNotExists(const std::string& path) {
    std::filesystem::create_directories(path);
}

// === 辅助函数 (保持 SSO 优化，小字符串无需堆分配) ===
std::string LMDBClient::DoubleToBytes(double value) {
    std::string buf(sizeof(double), '\0');
    std::memcpy(buf.data(), &value, sizeof(double));
    return buf;
}

double LMDBClient::BytesToDouble(const std::string& bytes) {
    if (bytes.size() != sizeof(double)) return 0.0; // 简化错误处理以提高速度，或保留 throw
    double val;
    std::memcpy(&val, bytes.data(), sizeof(double));
    return val;
}

std::string LMDBClient::IntToBytes(int value) {
    std::string buf(sizeof(int), '\0');
    std::memcpy(buf.data(), &value, sizeof(int));
    return buf;
}

int LMDBClient::BytesToInt(const std::string& bytes) {
    if (bytes.size() != sizeof(int)) return 0;
    int val;
    std::memcpy(&val, bytes.data(), sizeof(int));
    return val;
}

// === 初始化 ===
bool LMDBClient::Initialize(const std::string& db_path, size_t map_size_mb, bool read_only) {
    std::unique_lock<std::shared_mutex> lock(mutex_); // 写锁初始化
    if (initialized_) return true;

    read_only_ = read_only;
    CreateDirIfNotExists(db_path);

    int rc = mdb_env_create(&env_);
    if (rc != MDB_SUCCESS) return false;

    mdb_env_set_maxreaders(env_, 126);
    mdb_env_set_mapsize(env_, map_size_mb * 1024 * 1024);

    // 核心优化：MDB_NOSYNC | MDB_NOMETASYNC
    // 极大提高写入性能，依赖操作系统缓存保证一致性，不强制刷盘
    unsigned int flags = MDB_NOTLS | MDB_NOSYNC | MDB_NOMETASYNC;
    if (read_only) {
        flags |= MDB_RDONLY;
    }

    // mode 0664
    rc = mdb_env_open(env_, db_path.c_str(), flags, 0664);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    // 打开 DBI
    MDB_txn* txn = nullptr;
    rc = mdb_txn_begin(env_, nullptr, read_only ? MDB_RDONLY : 0, &txn);
    if (rc != MDB_SUCCESS) {
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    rc = mdb_dbi_open(txn, nullptr, 0, &dbi_);
    if (rc != MDB_SUCCESS) {
        mdb_txn_abort(txn);
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    mdb_txn_commit(txn);
    initialized_ = true;
    return true;
}

void LMDBClient::Close() {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (env_) {
        mdb_env_close(env_); // env_close 会自动关闭 dbi
        env_ = nullptr;
        dbi_ = 0;
        initialized_ = false;
    }
}

LMDBClient::~LMDBClient() {
    Close();
}

// === 事务模板 (拆分读写) ===

// 读事务：使用 shared_lock，允许并发读
template<typename Func>
bool LMDBClient::ExecuteReadTransaction(Func&& func) {
    std::shared_lock<std::shared_mutex> lock(this->mutex_);
    if (!this->initialized_) return false;

    MDB_txn* txn = nullptr;
    // MDB_RDONLY 对性能至关重要，不阻塞写锁等待
    int rc = mdb_txn_begin(this->env_, nullptr, MDB_RDONLY, &txn);
    if (rc != MDB_SUCCESS) return false;

    bool success = false;
    try {
        success = func(txn);
        // 只读事务通常使用 abort 释放资源比 commit 更快，
        // 但 MDB_RDONLY 下 commit/abort 开销极小
        mdb_txn_abort(txn);
        return success;
    }
    catch (...) {
        mdb_txn_abort(txn);
        return false;
    }
}

// 写事务：使用 unique_lock，严格序列化写操作
template<typename Func>
bool LMDBClient::ExecuteWriteTransaction(Func&& func) {
    if (read_only_) return false;
    std::unique_lock<std::shared_mutex> lock(this->mutex_);
    if (!this->initialized_) return false;

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(this->env_, nullptr, 0, &txn);
    if (rc != MDB_SUCCESS) return false;

    bool success = false;
    try {
        success = func(txn);
        if (success) {
            rc = mdb_txn_commit(txn);
            return (rc == MDB_SUCCESS);
        }
        else {
            mdb_txn_abort(txn);
            return false;
        }
    }
    catch (...) {
        mdb_txn_abort(txn);
        return false;
    }
}

// === 基础操作实现 ===

bool LMDBClient::Put(const std::string& key, const std::string& value) {
    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval{ value.size(), const_cast<char*>(value.data()) };
        return mdb_put(txn, dbi_, &mkey, &mval, 0) == MDB_SUCCESS;
        });
}

bool LMDBClient::PutDouble(const std::string& key, double value) {
    // 构造临时 string 在栈上，开销极小
    return Put(key, DoubleToBytes(value));
}

bool LMDBClient::Get(const std::string& key, std::string* value) {
    return ExecuteReadTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        int rc = mdb_get(txn, dbi_, &mkey, &mval);
        if (rc == MDB_SUCCESS) {
            if (value) {
                // 直接 assign，避免多次构造
                value->assign(static_cast<char*>(mval.mv_data), mval.mv_size);
            }
            return true;
        }
        return false;
        });
}

bool LMDBClient::GetDouble(const std::string& key, double* value) {
    return ExecuteReadTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        int rc = mdb_get(txn, dbi_, &mkey, &mval);
        if (rc == MDB_SUCCESS && mval.mv_size == sizeof(double)) {
            if (value) std::memcpy(value, mval.mv_data, sizeof(double));
            return true;
        }
        return false;
        });
}

bool LMDBClient::GetInt(const std::string& key, int* value) {
    return ExecuteReadTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        int rc = mdb_get(txn, dbi_, &mkey, &mval);

        // 校验：必须读取成功，且数据长度必须严格等于 int 的长度 (4字节)
        if (rc == MDB_SUCCESS && mval.mv_size == sizeof(int)) {
            if (value) std::memcpy(value, mval.mv_data, sizeof(int));
            return true;
        }
        return false;
        });
}

bool LMDBClient::Exists(const std::string& key) {
    return ExecuteReadTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        return mdb_get(txn, dbi_, &mkey, &mval) == MDB_SUCCESS;
        });
}

bool LMDBClient::Delete(const std::string& key) {
    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        return mdb_del(txn, dbi_, &mkey, nullptr) == MDB_SUCCESS;
        });
}

bool LMDBClient::GetStringBit(const std::string& key, int bit_index, bool* bit_value) {
    if (bit_index < 0 || bit_index > MAX_BIT_INDEX) return false;

    return ExecuteReadTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        int rc = mdb_get(txn, dbi_, &mkey, &mval);

        if (rc == MDB_NOTFOUND) return false;
        if (rc != MDB_SUCCESS) return false;

        size_t byte_offset = bit_index / 8;
        if (mval.mv_size <= byte_offset) {
            if (bit_value) *bit_value = false;
        }
        else {
            unsigned char* data = static_cast<unsigned char*>(mval.mv_data);
            if (bit_value) *bit_value = (data[byte_offset] >> (bit_index % 8)) & 1;
        }
        return true;
        });
}

// === 原子操作 (必须是写事务) ===

bool LMDBClient::AtomicSetStringBit(const std::string& key, int bit_index, bool set) {
    if (bit_index < 0 || bit_index > MAX_BIT_INDEX) return false;

    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        int rc = mdb_get(txn, dbi_, &mkey, &mval);

        std::string current_data; // 栈上分配，利用 SSO
        if (rc == MDB_SUCCESS) {
            current_data.assign(static_cast<char*>(mval.mv_data), mval.mv_size);
        }
        else if (rc != MDB_NOTFOUND) {
            return false;
        }

        size_t byte_offset = bit_index / 8;
        if (current_data.size() <= byte_offset) {
            current_data.resize(byte_offset + 1, 0);
        }

        unsigned char& target = reinterpret_cast<unsigned char&>(current_data[byte_offset]);
        if (set) target |= (1 << (bit_index % 8));
        else target &= ~(1 << (bit_index % 8));

        MDB_val mnew{ current_data.size(), current_data.data() };
        return mdb_put(txn, dbi_, &mkey, &mnew, 0) == MDB_SUCCESS;
        });
}

bool LMDBClient::AtomicIncrement(const std::string& key, int increment) {
    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        int current = 0;
        int rc = mdb_get(txn, dbi_, &mkey, &mval);

        if (rc == MDB_SUCCESS && mval.mv_size == sizeof(int)) {
            std::memcpy(&current, mval.mv_data, sizeof(int));
        }

        current += increment;
        // 直接构造 buffer 避免 IntToBytes 的拷贝
        char buf[sizeof(int)];
        std::memcpy(buf, &current, sizeof(int));

        MDB_val mnew{ sizeof(int), buf };
        return mdb_put(txn, dbi_, &mkey, &mnew, 0) == MDB_SUCCESS;
        });
}

bool LMDBClient::AtomicIncrementDouble(const std::string& key, double increment) {
    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        MDB_val mkey{ key.size(), const_cast<char*>(key.data()) };
        MDB_val mval;
        double current = 0.0;
        int rc = mdb_get(txn, dbi_, &mkey, &mval);

        if (rc == MDB_SUCCESS && mval.mv_size == sizeof(double)) {
            std::memcpy(&current, mval.mv_data, sizeof(double));
        }

        current += increment;

        char buf[sizeof(double)];
        std::memcpy(buf, &current, sizeof(double));

        MDB_val mnew{ sizeof(double), buf };
        return mdb_put(txn, dbi_, &mkey, &mnew, 0) == MDB_SUCCESS;
        });
}

bool LMDBClient::WriteBatch(const std::vector<std::pair<std::string, std::string>>& puts,
    const std::vector<std::string>& deletes) {
    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        for (const auto& [k, v] : puts) {
            MDB_val mkey{ k.size(), const_cast<char*>(k.data()) };
            MDB_val mval{ v.size(), const_cast<char*>(v.data()) };
            if (mdb_put(txn, dbi_, &mkey, &mval, 0) != MDB_SUCCESS) return false;
        }
        for (const auto& k : deletes) {
            MDB_val mkey{ k.size(), const_cast<char*>(k.data()) };
            if (mdb_del(txn, dbi_, &mkey, nullptr) != MDB_SUCCESS) return false;
        }
        return true;
        });
}

bool LMDBClient::WriteBatchDouble(const std::vector<std::pair<std::string, double>>& puts,
    const std::vector<std::string>& deletes) {
    // 对于批量操作，少量的 string 构造不是瓶颈，保持原样即可
    std::vector<std::pair<std::string, std::string>> str_puts;
    str_puts.reserve(puts.size());
    for (const auto& [k, v] : puts) {
        str_puts.emplace_back(k, DoubleToBytes(v));
    }
    return WriteBatch(str_puts, deletes);
}

bool LMDBClient::DeleteDatabase() {
    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        // mdb_drop: 0 表示清空数据但不关闭 DBI 句柄
        // 这是一个极快的 O(1) 操作
        return mdb_drop(txn, dbi_, 0) == MDB_SUCCESS;
        });
}

bool LMDBClient::DeleteKeys(const std::string& prefix) {
    // 如果前缀为空，逻辑上等同于清空数据库，但用 DeleteDatabase 更快
    if (prefix.empty()) {
        return DeleteDatabase();
    }

    return ExecuteWriteTransaction([&](MDB_txn* txn) {
        MDB_cursor* cursor;
        if (mdb_cursor_open(txn, dbi_, &cursor) != MDB_SUCCESS) return false;

        MDB_val key, data;
        MDB_val pkey{ prefix.size(), const_cast<char*>(prefix.data()) };

        // 1. 定位到第一个 >= prefix 的位置
        int rc = mdb_cursor_get(cursor, &pkey, &data, MDB_SET_RANGE);

        while (rc == MDB_SUCCESS) {
            std::string current_key(static_cast<char*>(pkey.mv_data), pkey.mv_size);

            // 2. 检查前缀匹配
            if (current_key.substr(0, prefix.size()) != prefix) {
                // 如果当前 key 不再以前缀开头，说明后面的都不匹配了（因为是排序的）
                break;
            }

            // 3. 删除当前游标指向的项
            // 注意：mdb_cursor_del 会自动将游标移动到“下一项”
            if (mdb_cursor_del(cursor, 0) != MDB_SUCCESS) {
                mdb_cursor_close(cursor);
                return false;
            }

            // 4. 获取“下一项”（因为刚才删除了，游标已经指过去了，所以用 GET_CURRENT）
            // 如果删除的是最后一项，GET_CURRENT 会返回 MDB_NOTFOUND，循环结束
            rc = mdb_cursor_get(cursor, &pkey, &data, MDB_GET_CURRENT);
        }

        mdb_cursor_close(cursor);
        return true;
        });
}

std::vector<std::string> LMDBClient::GetKeys(const std::string& prefix) {
    std::vector<std::string> keys;
    ExecuteReadTransaction([&](MDB_txn* txn) {
        MDB_cursor* cursor;
        if (mdb_cursor_open(txn, dbi_, &cursor) != MDB_SUCCESS) return false;

        MDB_val key, data;
        MDB_cursor_op op = MDB_FIRST;

        if (!prefix.empty()) {
            MDB_val pkey{ prefix.size(), const_cast<char*>(prefix.data()) };
            if (mdb_cursor_get(cursor, &pkey, &data, MDB_SET_RANGE) != MDB_SUCCESS) {
                mdb_cursor_close(cursor);
                return true;
            }
            // MDB_SET_RANGE 后 key 已经指向了 >= prefix 的第一个
            // 此时 pkey 已经被修改为数据库里的 key，需要取出来判断
            if (mdb_cursor_get(cursor, &key, &data, MDB_GET_CURRENT) != MDB_SUCCESS) {
                mdb_cursor_close(cursor);
                return true;
            }
            op = MDB_NEXT; // 下一次循环用 NEXT
        }
        else {
            if (mdb_cursor_get(cursor, &key, &data, MDB_FIRST) != MDB_SUCCESS) {
                mdb_cursor_close(cursor);
                return true;
            }
        }

        // 循环
        do {
            std::string key_str(static_cast<char*>(key.mv_data), key.mv_size);
            if (!prefix.empty()) {
                if (key_str.substr(0, prefix.size()) != prefix) break;
            }
            keys.push_back(std::move(key_str));
        } while (mdb_cursor_get(cursor, &key, &data, MDB_NEXT) == MDB_SUCCESS);

        mdb_cursor_close(cursor);
        return true;
        });
    return keys;
}
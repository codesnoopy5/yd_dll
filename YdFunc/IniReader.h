#ifndef INI_READER_H
#define INI_READER_H

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <algorithm>
#include <optional>

class IniReader {


public:
    IniReader() = default;

    // 加载并解析 INI 文件
    bool load(const std::string& filename) {
        std::ifstream file(filename);
        if (!file.is_open()) return false;

        std::string line, currentSection = "default";
        while (std::getline(file, line)) {
            // 1. 去除首尾空格
            trim(line);

            // 2. 跳过空行和注释
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;

            // 3. 处理 Section [section_name]
            if (line[0] == '[' && line.back() == ']') {
                currentSection = line.substr(1, line.length() - 2);
                trim(currentSection);
            }
            // 4. 处理 Key=Value
            else {
                size_t delimiterPos = line.find('=');
                if (delimiterPos != std::string::npos) {
                    std::string key = line.substr(0, delimiterPos);
                    std::string value = line.substr(delimiterPos + 1);
                    trim(key);
                    trim(value);
                    settings[currentSection][key] = value;
                }
            }
        }
        file.close();
        return true;
    }

    // 核心改进：返回 optional
    std::optional<std::string> getString(const std::string& section, const std::string& key) const {
        auto secIt = settings.find(section);
        if (secIt != settings.end()) {
            auto keyIt = secIt->second.find(key);
            if (keyIt != secIt->second.end()) {
                return keyIt->second; // 成功取值
            }
        }
        return std::nullopt; // 键值对不存在
    }



private:
    // 存储结构：Map<Section, Map<Key, Value>>
    std::map<std::string, std::map<std::string, std::string>> settings;

    // 辅助函数：去除字符串首尾空格
    void trim(std::string& s) const {
        if (s.empty()) return;
        s.erase(0, s.find_first_not_of(" \t\r\n"));
        s.erase(s.find_last_not_of(" \t\r\n") + 1);
    }
};

#endif
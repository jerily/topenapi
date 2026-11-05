#pragma once
#include <string>
#include <vector>

namespace topenapi {
inline std::string trim_ascii_copy(const std::string &value) {
    const auto first = value.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n\f\v") - first + 1);
}
inline std::string lowercase_ascii_copy(std::string value) {
    for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}
inline std::vector<std::string> split_on_char(const std::string &value, char separator) {
    std::vector<std::string> result;
    size_t start = 0;
    while (true) {
        const auto end = value.find(separator, start);
        result.push_back(value.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) return result;
        start = end + 1;
    }
}
}

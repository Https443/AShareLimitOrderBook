#include "commonUtil.h"

int compareDouble(double value1, double value2)
{
    if (std::fabs(value1 - value2) < 1e-12)
    {
        return 0;
    }
    else if (value1 > value2 + 1e-12)
    {
        return 1;
    }
    else
    {
        return -1;
    }
}


bool isNaNOrInf(double value)
{
    return std::isnan(value) || std::isinf(value);
}


// YYYYMMDDHHMMSSsss获取HHMMSSsss
long getMarketDataTime(long originalTime)
{
    return originalTime % 1000000000;  // 取后9位 HHMMSSsss
}

// 解析单行CSV，返回字段列表（处理引号和内部逗号）
void parse_csv_line(const std::string &line, std::vector<std::string> &data)
{
    data.clear();

    std::string current_field;
    bool in_quotes = false;
    size_t n = line.size();
    for (size_t i = 0; i < n; ++i)
    {
        char ch = line[i];
        if (ch == '"')
        {
            // 双引号转义: "" -> "
            if (in_quotes && i + 1 < n && line[i + 1] == '"')
            {
                current_field.push_back('"');
                ++i;
            }
            else
            {
                in_quotes = !in_quotes;
            }
        }
        else if (ch == ',' && !in_quotes)
        {
            // 逗号作为分隔符
            data.push_back(current_field);
            current_field.clear();
        }
        else
        {
            // 普通字符，直接添加到当前字段
            current_field.push_back(ch);
        }
    }
    data.push_back(current_field);

    // 添加对最后一个字段末尾空白字符（包括换行符）的处理
    if (!data.empty()) {
        // 移除字段末尾的空白字符，特别是换行符
        for (auto& field : data) {
            // 删除字符串末尾的空白字符
            field.erase(field.find_last_not_of(" \t\n\r\f\v") + 1);
            // 删除字符串开头的空白字符
            field.erase(0, field.find_first_not_of(" \t\n\r\f\v"));
        }
    }

    (void)in_quotes;
}

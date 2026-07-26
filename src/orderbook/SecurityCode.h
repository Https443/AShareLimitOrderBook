#pragma once

#include <cstdint>
#include <string_view>
#include "MatchTypes.h"

namespace marketdata
{
namespace orderbook
{
    constexpr uint8_t kShenzhenMarket = 0;
    constexpr uint8_t kShanghaiMarket = 1;

    namespace security_code_detail
    {
        [[nodiscard]] inline constexpr bool isSixDigitCode(std::string_view code) noexcept
        {
            if (code.size() != 6)
            {
                return false;
            }

            for (const char value : code)
            {
                if (value < '0' || value > '9')
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] inline constexpr bool hasPrefix(
            std::string_view code,
            std::string_view prefix) noexcept
        {
            return code.starts_with(prefix);
        }

        [[nodiscard]] inline constexpr bool isInRange(
            std::string_view code,
            std::string_view begin,
            std::string_view end) noexcept
        {
            return code >= begin && code <= end;
        }
    }

    [[nodiscard]] inline constexpr bool isEtf(
        uint8_t exchange,
        std::string_view code) noexcept
    {
        using namespace security_code_detail;

        if (!isSixDigitCode(code))
        {
            return false;
        }

        if (exchange == kShenzhenMarket)
        {
            // 159900 是 ETF 申赎现金替代虚拟证券，不是可交易 ETF。
            return (hasPrefix(code, "158") || hasPrefix(code, "159"));
        }

        if (exchange != kShanghaiMarket)
        {
            return false;
        }

        return hasPrefix(code, "510") ||
               hasPrefix(code, "511") ||
               hasPrefix(code, "512") ||
               hasPrefix(code, "513") ||
               hasPrefix(code, "515") ||
               hasPrefix(code, "516") ||
               hasPrefix(code, "517") ||
               hasPrefix(code, "518") ||
               hasPrefix(code, "520") ||
               hasPrefix(code, "526") ||
               hasPrefix(code, "530") ||
               hasPrefix(code, "551") ||
               hasPrefix(code, "560") ||
               hasPrefix(code, "561") ||
               hasPrefix(code, "562") ||
               hasPrefix(code, "563") ||
               hasPrefix(code, "581") ||
               hasPrefix(code, "587") ||
               hasPrefix(code, "588") ||
               hasPrefix(code, "589");
    }

    [[nodiscard]] inline constexpr bool isMainBoard(
        uint8_t exchange,
        std::string_view code) noexcept
    {
        using namespace security_code_detail;

        if (!isSixDigitCode(code))
        {
            return false;
        }

        if (exchange == kShenzhenMarket)
        {
            return code[0] == '0' &&
                   code[1] == '0' &&
                   code[2] >= '0' &&
                   code[2] <= '4';
        }

        if (exchange == kShanghaiMarket)
        {
            return hasPrefix(code, "600") ||
                   hasPrefix(code, "601") ||
                   hasPrefix(code, "603") ||
                   hasPrefix(code, "605");
        }

        return false;
    }

    [[nodiscard]] inline constexpr bool isChiNextBoard(
        uint8_t exchange,
        std::string_view code) noexcept
    {
        using namespace security_code_detail;
        return exchange == kShenzhenMarket &&
               isSixDigitCode(code) &&
               hasPrefix(code, "30");
    }

    [[nodiscard]] inline constexpr bool isStarMarket(
        uint8_t exchange,
        std::string_view code) noexcept
    {
        using namespace security_code_detail;
        return exchange == kShanghaiMarket &&
               isSixDigitCode(code) &&
               (hasPrefix(code, "688") || hasPrefix(code, "689"));
    }

    [[nodiscard]] inline constexpr bool isConvertibleBond(
        uint8_t exchange,
        std::string_view code) noexcept
    {
        using namespace security_code_detail;

        if (!isSixDigitCode(code))
        {
            return false;
        }

        if (exchange == kShenzhenMarket)
        {
            return hasPrefix(code, "123") ||
                   hasPrefix(code, "127") ||
                   hasPrefix(code, "128");
        }

        if (exchange == kShanghaiMarket)
        {
            return hasPrefix(code, "110") ||
                   hasPrefix(code, "111") ||
                   hasPrefix(code, "113") ||
                   hasPrefix(code, "118");
        }

        return false;
    }

    [[nodiscard]] inline constexpr MarketType findMarketType(
        uint8_t exchange,
        std::string_view code) noexcept
    {
        if (isMainBoard(exchange, code))
        {
            return MarketType::MAIN;
        }
        else if (isChiNextBoard(exchange, code))
        {
            return MarketType::CYB;
        }
        else if (isStarMarket(exchange, code))
        {
            return MarketType::KCB;
        }
        else if (isEtf(exchange, code))
        {
            return MarketType::ETF;
        }
        else if (isConvertibleBond(exchange, code))
        {
            return MarketType::CONVERTIBLE_BOND;
        }
        else
        {
            return MarketType::NONE;
        }
    }
}
}

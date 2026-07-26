#pragma once

#include <cstdint>

namespace marketdata
{
namespace orderbook
{
    enum class ExchangeType : uint8_t
    {
        UNKNOWN = 0,
        SH,
        SZ
    };

    enum class MarketType : uint8_t
    {
        MAIN,
        CYB,
        KCB,
        ETF,
        CONVERTIBLE_BOND,
        NONE,
    };

    enum class PriceCageMode : uint8_t
    {
        DISABLED = 0,
        PENDING,
        REJECT
    };

    // 交易阶段
    enum class TradingPhase : uint8_t
    {
        PRE_OPEN = 0,
        OPEN_CALL_AUCTION,
        OPEN_CALL_MATCH,
        CONTINUOUS_TRADING,
        CLOSE_CALL_AUCTION,
        CLOSE_CALL_MATCH,
        CLOSED
    };

    // 模拟成交记录
    struct MatchRecord
    {
        char side = '-';
        int32_t exchange = 0;
        int32_t channelNo = 0;
        int64_t matchId = 0;
        int64_t bidOrderId = 0;
        int64_t offerOrderId = 0;
        int64_t price = 0;
        int64_t volume = 0;
        int64_t datetime = 0;
    };
}
}

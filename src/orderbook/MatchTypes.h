#pragma once

#include <cstdint>

namespace marketdata
{
    enum class ExchangeType : uint8_t
    {
        UNKNOWN = 0,
        SH,
        SZ
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
        int64_t match_id = 0;
        int32_t channel_no = 0;
        int64_t bid_order_id = 0;
        int64_t offer_order_id = 0;
        int64_t price = 0;
        int64_t volume = 0;
        int64_t datetime = 0;
        char side = '-';
    };
}

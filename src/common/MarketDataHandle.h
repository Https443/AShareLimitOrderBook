#pragma once
#include "MarketDataStruct.h"
#include "util/Impl.h"
#include <memory>
#include "util/Config.h"
#include "util/MysqlConnectionPool.h"
#include "common/MDTools.h"
#include <algorithm>
#include <cstring>
// #include "algorithm/QuantCalender.h"

class TradingDayCalendar;

namespace marketdata
{
    class StrategyContext
    {
    public:
        std::shared_ptr<Config> mCtxConfig;
        std::string mName;
        std::string mDate;
        std::string mConfigPath;
        std::shared_ptr<MysqlConnectionCommonPool> mDatabasePool;
        std::shared_ptr<std::unordered_map<long, std::string>> mSubjectIdMap;
        std::vector<std::string> mCodes;
        std::shared_ptr<TradingDayCalendar> mTradingDayCalendar;
    };

    /// 策略事件处理函数
    class StrategyInterface : public Impl
    {
    public:
        ///
        /// \brief initialize 策略初始化
        ///
        virtual void FUNCTION_CALL_MODE initialize(std::shared_ptr<StrategyContext> context) = 0;
        ///
        /// \brief onStrategyStart 策略开始
        /// \remark 初始化完成后，立即调用此回调函数，在此回调函数中可以报单，不能读取外部文件
        virtual void FUNCTION_CALL_MODE onStrategyStart() {}
        ///
        /// \brief handleTick tick行情接收回调
        /// \param snapshot tick数据，MDSnapshot类型
        ///
        virtual void FUNCTION_CALL_MODE handleTick(const MDSnapshot *snapshot) {}
        ///
        /// \brief handleData k线行情接收回调
        /// \param kline k线数据，MDKLine类型
        ///
        virtual void FUNCTION_CALL_MODE handleData(const MDKLine *kline) {}
        ///
        /// \brief handleOrderRecord 逐笔委托接收回调
        /// \param order 逐笔委托，MDOrder类型
        ///
        virtual void FUNCTION_CALL_MODE handleOrderRecord(const uint16_t shardId, const Order *order) {}
        ///
        /// \brief handleTradeRecord 逐笔成交接收回调
        /// \param trade 逐笔成交，MDTrade类型
        ///
        virtual void FUNCTION_CALL_MODE handleTradeRecord(const uint16_t shardId, const Trade *trade) {}
        ///
        /// \brief handleRapidSnapshot 逐笔成交接收回调
        /// \param trade 逐笔成交，MDTrade类型
        ///
        virtual void FUNCTION_CALL_MODE handleRapidSnapshotRecord(const uint16_t shardId, const MDRapidSnapshot *pSnapshot) {}
        ///
        /// \brief handleReplayEvent MDReplayServer 的原始逐笔与处理后快照。
        ///
        /// 新策略应重载此接口并直接使用 rapidSnapshot，不应在策略内再次重建订单簿。
        /// 默认实现兼容历史策略：仍分发原始逐笔，并提供由 rapidSnapshot 适配出的十档快照。
        virtual void FUNCTION_CALL_MODE handleReplayEvent(const MDMergeData *event)
        {
            if (event == nullptr)
            {
                return;
            }

            if (event->dataType == static_cast<uint8_t>(MixedRecordType::ORDER))
            {
                handleOrderRecord(event->shardId, &event->order);
            }
            else if (event->dataType == static_cast<uint8_t>(MixedRecordType::TRADE))
            {
                handleTradeRecord(event->shardId, &event->trade);
            }

            handleRapidSnapshotRecord(event->shardId, &event->rapidSnapshot);
        }
        ///
        /// \brief onStrategyEnd 策略停止回调
        ///
        virtual void FUNCTION_CALL_MODE onStrategyEnd() {}
    };
}


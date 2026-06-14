#pragma once
#include "MarketDataStruct.h"
#include "util/Impl.h"
#include <memory>
#include "util/Config.h"
#include "util/MysqlConnectionPool.h"
#include "common/MDTools.h"
// #include "algorithm/QuantCalender.h"

class TradingDayCalendar;

namespace marketdata
{
    class StrategyContext
    {
    public:
        std::shared_ptr<Config> m_ctx_config;
        std::string m_name;
        std::string m_date;
        std::string m_configPath;
        std::shared_ptr<MysqlConnectionCommonPool> m_databasePool;
        std::shared_ptr<std::unordered_map<long, std::string>> m_subjectIdMap;
        std::vector<std::string> m_codes;
        std::shared_ptr<TradingDayCalendar> m_tradingDayCalendar;
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
        virtual void FUNCTION_CALL_MODE handleOrderRecord(const Order *order) {}
        ///
        /// \brief handleTradeRecord 逐笔成交接收回调
        /// \param trade 逐笔成交，MDTrade类型
        ///
        virtual void FUNCTION_CALL_MODE handleTradeRecord(const Trade *trade) {}
        ///
        /// \brief onReadyStopStrategy 策略准备停止回调
        /// \remark 策略停止时，首先调用此回调，然后再调用onStrategyEnd。此回调中允许交易，不允许和外部系统交互，可以在此回调中撤掉在途订单
        virtual void FUNCTION_CALL_MODE onReadyStopStrategy() {}
        ///
        /// \brief onStrategyEnd 策略停止回调
        ///
        virtual void FUNCTION_CALL_MODE onStrategyEnd() {}
    };
}
// typedef marketdata::StrategyInterface *(*GET_PLUGIN_FUNC)();
// extern "C" marketdata::StrategyInterface *getStrategyHandler();
// typedef void (*DESTROY_PLUGIN_FUNC)(marketdata::StrategyInterface*);
// extern "C" void destroyStrategyHandler(marketdata::StrategyInterface* strategy);

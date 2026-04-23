#pragma once
#include "MarketDataStruct.h"
#include <memory>
#include "util/Config.h"
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
        std::shared_ptr<std::unordered_map<long, std::string>> m_subjectIdMap;
        std::vector<std::string> m_codes;
        std::shared_ptr<TradingDayCalendar> m_tradingDayCalendar;
    };

    /// 策略事件处理函数
    class StrategyInterface
    {
    public:
        ///
        /// \brief initialize 策略初始化
        ///
        virtual void initialize(std::shared_ptr<StrategyContext> context) = 0;
        ///
        /// \brief onStrategyStart 策略开始
        /// \remark 初始化完成后，立即调用此回调函数，在此回调函数中可以报单，不能读取外部文件
        virtual void onStrategyStart() {}
        ///
        /// \brief handleTick tick行情接收回调
        /// \param snapshot tick数据，MDSnapshot类型
        ///
        virtual void handleTick(const MDSnapshot *snapshot) {}
        ///
        /// \brief handleData k线行情接收回调
        /// \param kline k线数据，MDKLine类型
        ///
        virtual void handleData(const MDKLine *kline) {}
        ///
        /// \brief handleOrderRecord 逐笔委托接收回调
        /// \param order 逐笔委托，MDOrder类型
        ///
        virtual void handleOrderRecord(const MDOrder *order) {}
        ///
        /// \brief handleTradeRecord 逐笔成交接收回调
        /// \param trade 逐笔成交，MDTrade类型
        ///
        virtual void handleTradeRecord(const MDTrade *trade) {}
        ///
        /// \brief handleOrderReport 订单回报推送
        /// \param order 订单
        ///
        virtual void handleOrderReport(const Order *order) {}
        ///
        /// \brief handleExecutionReport 成交回报推送
        /// \param execution 成交
        ///
        virtual void handleExecutionReport(const Execution *execution) {}

        // ///
        // /// \brief handleEtfEstimateInfo ETF预估信息推送回调
        // /// \param estimateInfo 实时ETF预估信息
        // /// \remark 需要先将订阅的ETF在ETF套利界面上添加到常用ETF列表中才可使用
        // virtual void handleEtfEstimateInfo(const EtfEstimateInfo *estimateInfo){}
        ///
        /// \brief onStrategyParamsChange 策略参数变更推送
        /// \param params 参数
        /// \param filePath 参数文件路径，仅通过导入文件修改参数时有效
        ///
        virtual void onStrategyParamsChange(const char *params, const char *filePath) {}

        ///
        /// \brief onReadyStopStrategy 策略准备停止回调
        /// \remark 策略停止时，首先调用此回调，然后再调用onStrategyEnd。此回调中允许交易，不允许和外部系统交互，可以在此回调中撤掉在途订单
        virtual void onReadyStopStrategy() {}
        ///
        /// \brief onStrategyEnd 策略停止回调
        ///
        virtual void onStrategyEnd() {}
        ///
        /// \brief onCustomMsg 自定义消息接收回调
        /// \param msgType 自定义消息类型，10001-65536之间的自定义值
        /// \param msg 消息体
        ///
        virtual void onCustomMsg(int msgType, const char *msg) {}

        ///
        /// \brief onFundUpdate 资金更新
        /// \param context
        /// \param fundInfo
        ///
        virtual void onFundUpdate(const FundInfo *fundInfo) {}

        ///
        /// \brief onPositionUpdate 持仓更新
        /// \param context
        /// \param posInfo
        ///
        virtual void onPositionUpdate(const Position *posInfo) {}

        ///
        /// \brief handleFundFlow 实时资金流向回调
        /// \param context
        /// \param fundFlow 实时资金流向数据
        ///
        virtual void handleFundFlow(const FundFlow *fundFlow) {}

        // ///
        // /// \brief onRecvOrderReply 报撤单异步响应
        // /// \param context
        // /// \param orderReply 异步响应数据
        // ///
        // virtual void onRecvOrderReply(const OrderReply &orderReply){}
    };
}


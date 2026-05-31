#ifndef MARKETDATA_STRUCT_H
#define MARKETDATA_STRUCT_H

#include <stdint.h>
#include <memory>

namespace marketdata {
    /**
     * @name 数据长度定义
     * @{ */
    class ConstField
    {
    public:
        static const uint32_t kIPMaxLen = 24;            // IP地址的最大长度
        static const uint32_t kUsernameLen = 32;         // 用户名的最大长度
        static const uint32_t kUMSItemLen = 8;           // 服务项信息的最大个数
        static const uint32_t kChannelExternCfgLen = 16; // 通道扩展配置的最大个数
        static const uint32_t kPasswordLen = 64;         // 用户名的最大长度
        static const uint32_t kSecurityCodeLen = 16;     // 证券代码最大长度
        static const uint32_t rSecurityCodeLen = 8;
        static const uint32_t kFutureSecurityCodeLen = 32; // 期货证券代码最大长度
        static const uint32_t kSecurityNameLen = 32;       // 证券名称最大长度
        static const uint32_t kPositionLevelLen = 10;      // 行情档位
        static const uint32_t kPositionParidLevelLen = 20;      // 行情档位
        static const uint32_t kPathLen = 255;              // 文件路径最大长度
        static const uint32_t kConfirmIdLen = 8;           // 定价行情约定号 为空表示意向行情 否则为定价行情
        static const uint32_t kContactorLen = 12;          // 联系人
        static const uint32_t kContactInfoLen = 30;        // 联系方式
        static const uint32_t kTradingPhaseCodeLen = 8;    // 交易状态标志
        static const uint32_t kContractIDLen = 32;         // 合约交易代码
        static const uint32_t kContractSymbolLen = 32;     // 期权合约简称
        static const uint32_t kUnderlyingTypeLen = 3;      // 标的证券类型
        static const uint32_t kSecurityStatusFlagLen = 8;  // 期权合约状态信息标签
        static const uint32_t kExChangeInstIDLen = 31;
        static const uint32_t kMaxMarketType = 255;
        static const uint32_t kTradingStatusLen = 8;
        static const uint32_t kSecurityStatusLen = 8;
        static const uint32_t kMDStreamIDMaxLen = 6;
        static const uint32_t kTypesLen = 8;
        static const uint32_t kDateLen = 10;
        static const uint32_t kTimeLen = 10;
        static const uint32_t kSecurityAbbreviationLen = 64; // 证券简称最大长度
    };

    typedef char SecurityCodeType[ConstField::rSecurityCodeLen];
    /**  @} */

    #pragma pack(push, 8)
    struct MDOrder
    {
        uint8_t side;       // 买卖方向 (深圳市场:1-买 2-卖 G-借入 F-出借, 上海市场:B–买单 S–卖单)
        uint8_t order_type; // 订单类别 (深圳市场:1-市价 2-限价 U-本方最优, 上海市场:A–新增委托订单 D–删除委托订单，即撤单)
        int32_t channel_no; // 频道号
        // char    md_stream_id[ConstField::kMDStreamIDMaxLen];       // 行情类别
        char security_code[ConstField::rSecurityCodeLen]; // 证券代码
        // int32_t market_type;                                       // 市场类型
        int64_t datetime; // 时间（YYYYMMDDHHMMSSsss)
        int64_t price;    // 委托价格，实际值需除以1000000
        int64_t volume;   // 深圳市场:委托数量, 上海市场:剩余委托数量
        // int64_t order_no;                                           // 原始订单号(上海市场有效, 上交所在新增、删除订单时用以标识订单的唯一编号)
        int64_t appl_seq_num; // 频道索引
        int64_t biz_index;    // 业务序号
    };
    // 1+1+4+8+8+8+8+8+8=54-> 54+2=56

    struct MDTrade
    {
        uint8_t side;       // 买卖方向 (仅上海有效 B-外盘，主动买 S-内盘，主动卖 N-未知)(无效数据使用'-'填充)
        uint8_t exec_type;  // 成交类型 (仅深圳有效 4-撤销 F-成交)(无效数据使用'-'填充)
        int32_t channel_no; // 频道号
        // char    md_stream_id[ConstField::kMDStreamIDMaxLen];     // 行情类别
        char security_code[ConstField::rSecurityCodeLen]; // 证券代码
        // int32_t market_type;                                     // 市场类型
        int64_t datetime; // 时间（YYYYMMDDHHMMSSsss)
        int64_t price;    // 成交价格，实际值需除以1000000, 深交所撤单时价格为0
        int64_t volume;   // 成交数量
        // int64_t value_trade;                                     // 成交金额，实际值需除以1000000
        int64_t appl_seq_num;       // 频道编号
        int64_t bid_appl_seq_num;   // 买方委托索引
        int64_t offer_appl_seq_num; // 卖方委托索引
        int64_t biz_index;          // 业务序号
    };
    // 1+1+4+8+8+8+8+8+8+8+8=70-> 70+2=72

    struct MDSnapshot
    {
        char security_code[ConstField::rSecurityCodeLen];    // 证券代码
        int64_t orig_time;                                   // 时间（YYYYMMDDHHMMSSsss)
        int64_t last_price;                                  // 最新价，实际值需除以1000000
        int64_t num_trades;                                  // 成交笔数
        int64_t volume;                                      // 成交量
        int64_t turnover;                                    // 成交额，实际值需除以1000000
        int64_t total_trade;                                 // 成交总笔数
        int64_t total_volume;                                // 成交总量
        int64_t total_turnover;                              // 成交总金额，实际值需除以1000000
        int64_t total_bid_volume;                            // 委托买入总量
        int64_t total_offer_volume;                          // 委托卖出总量
        int64_t weighted_avg_bid_price;                      // 加权平均为委买价格，实际值需除以1000000
        int64_t weighted_avg_offer_price;                    // 加权平均为委卖价格，实际值需除以1000000
        int64_t bid_price[ConstField::kPositionLevelLen];    // 申买价，实际值需除以1000000
        int64_t bid_volume[ConstField::kPositionLevelLen];   // 申买量
        int64_t offer_price[ConstField::kPositionLevelLen];  // 申卖价，实际值需除以1000000
        int64_t offer_volume[ConstField::kPositionLevelLen]; // 申卖量
        int64_t bid_order[ConstField::kPositionLevelLen];    // 委买挂单数量
        int64_t offer_order[ConstField::kPositionLevelLen];  // 委卖挂单数量
    };
    // 8+8+8+8+8+8+8+8+8+8+8+8+8+(10*8)+(10*8)+(10*8)+(10*8)+(10*8)+(10*8)=584

    // 合成快照
    struct MDRapidSnapshot
    {
        char security_code[ConstField::rSecurityCodeLen];    // 证券代码
        int64_t orig_time;                                   // 时间（YYYYMMDDHHMMSSsss)
        int64_t id;                                          // 快照对应的逐笔id
        int64_t last_price;                                  // 最新价，实际值需除以1000000
        int64_t pre_close_price;                             // 昨收价，实际值需除以1000000
        int64_t open_price;                                  // 开盘价，实际值需除以1000000, 暂未更新
        int64_t high_price;                                  // 最高价，实际值需除以1000000
        int64_t low_price;                                   // 最低价，实际值需除以1000000
        int64_t close_price;                                 // 收盘价，实际值需除以1000000, 暂未更新
        int64_t volume;                                      // 成交量
        int64_t turnover;                                    // 成交额，实际值需除以1000000
        int64_t high_limited;                                // 涨停价，实际值需除以1000000
        int64_t low_limited;                                 // 跌停价，实际值需除以1000000
        int64_t bid_price[ConstField::kPositionParidLevelLen];    // 申买价，实际值需除以1000000
        int64_t bid_volume[ConstField::kPositionParidLevelLen];   // 申买量
        int64_t offer_price[ConstField::kPositionParidLevelLen];  // 申卖价，实际值需除以1000000
        int64_t offer_volume[ConstField::kPositionParidLevelLen]; // 申卖量
        int64_t bid_order[ConstField::kPositionParidLevelLen];    // 委买挂单数量
        int64_t offer_order[ConstField::kPositionParidLevelLen];  // 委卖挂单数量
    };
    // 8+(11*9)+(20*8)+(20*8)+(20*8)+(20*8)+(20*8)+(20*8)=132*8=

    /**
     * @name 现货快照数据信息结构定义
     * @{ */
    struct MDSnapshotV2 : MDSnapshot
    {
        int32_t market_type; // 市场类型
        int32_t channel_no;  // 频道代码
        char md_stream_id[ConstField::kMDStreamIDMaxLen];
        char instrument_status[ConstField::kTradingPhaseCodeLen];  // 当前品种交易状态
        char trading_phase_code[ConstField::kTradingPhaseCodeLen]; // 产品实时阶段及标志
        //************************************上海现货快照交易状态***************************************************************
        // 该字段为8位字符串，左起每位表示特定的含义，无定义则填空格。
        // 第0位：‘S’表示启动（开市前）时段，‘C’表示集合竞价时段，‘T’表示连续交易时段，
        // ‘E’表示闭市时段 ，‘P’表示临时停牌,
        // ‘M’表示可恢复交易的熔断（盘中集合竞价）,‘N’表示不可恢复交易的熔断（暂停交易至闭市）
        // ‘U’表示收盘集合竞价
        // 第1位：‘0’表示此产品不可正常交易，‘1’表示此产品可正常交易。
        // 第2位：‘0’表示未上市，‘1’表示已上市
        // 第3位：‘0’表示此产品在当前时段不接受进行新订单申报，‘1’ 表示此产品在当前时段可接受进行新订单申报。

        //************************************深圳现货快照交易状态***************************************************************
        // 第 0位：‘S’= 启动（开市前）‘O’= 开盘集合竞价‘T’= 连续竞价‘B’= 休市‘C’= 收盘集合竞价‘E’= 已闭市‘H’= 临时停牌‘A’= 盘后交易‘V’=波动性中断
        // 第 1位：‘0’= 正常状态 ‘1’= 全天停牌
        int64_t pre_close_price;              // 昨收价，实际值需除以1000000
        int64_t open_price;                   // 开盘价，实际值需除以1000000
        int64_t high_price;                   // 最高价，实际值需除以1000000
        int64_t low_price;                    // 最低价，实际值需除以1000000
        int64_t close_price;                  // 收盘价，实际值需除以1000000
        int64_t IOPV;                         // IOPV净值估产，实际值需除以1000000
        int64_t yield_to_maturity;            // 到期收益率，实际值需除以1000
        int64_t high_limited;                 // 涨停价，实际值需除以1000000
        int64_t low_limited;                  // 跌停价，实际值需除以1000000
        int64_t price_earning_ratio1;         // 市盈率1，实际值需除以1000000
        int64_t price_earning_ratio2;         // 市盈率2，实际值需除以1000000
        int64_t change1;                      // 升跌1（对比昨收价），实际值需除以1000000
        int64_t change2;                      // 升跌2（对比上一笔），实际值需除以1000000
        int64_t pre_close_iopv;               // 基金T-1日收盘时刻IOPV, 实际值需除以1000000
        int64_t alt_weighted_avg_bid_price;   // 债券加权平均委买价格, 实际值需除以1000000
        int64_t alt_weighted_avg_offer_price; // 债券加权平均委卖价格, 实际值需除以1000000
        int64_t etf_buy_number;               // ETF 申购笔数
        int64_t etf_buy_amount;               // ETF 申购数量,实际值需除以100
        int64_t etf_buy_money;                // ETF 申购金额,实际值需除以100000
        int64_t etf_sell_number;              // ETF 赎回笔数
        int64_t etf_sell_amount;              // ETF 赎回数量,实际值需除以100
        int64_t etf_sell_money;               // ETF 赎回金额,实际值需除以100000
        int64_t total_warrant_exec_volume;    // 权证执行的总数量,实际值需除以100
        int64_t war_lower_price;              // 债券质押式回购品种加权平均价, 实际值需除以1000000
        int64_t war_upper_price;              // 权证涨停价格, 实际值需除以1000000
        int64_t withdraw_buy_number;          // 买入撤单笔数
        int64_t withdraw_buy_amount;          // 买入撤单数量,实际值需除以100
        int64_t withdraw_buy_money;           // 买入撤单金额,实际值需除以100000
        int64_t withdraw_sell_number;         // 卖出撤单笔数
        int64_t withdraw_sell_amount;         // 卖出撤单数量,实际值需除以100
        int64_t withdraw_sell_money;          // 卖出撤单金额,实际值需除以100000
        int64_t total_bid_number;             // 买入总笔数
        int64_t total_offer_number;           // 卖出总笔数
        int32_t bid_trade_max_duration;       // 买入委托成交最大等待时间
        int32_t offer_trade_max_duration;     // 卖出委托成交最大等待时间
        int32_t num_bid_orders;               // 买方委托价位数
        int32_t num_offer_orders;             // 卖方委托价位数
        int64_t last_trade_time;              // 最近成交时间（为YYYYMMDDHHMMSSsss 仅上海00文件，LDDS生效）
    };

    struct MDKLine
    {
        int64_t datetime;                                 // 时间（YYYYMMDDHHMMSSsss)
        int64_t open;                                     // k线周期内的开盘价，实际值需除以1000000
        int64_t close;                                    // k线周期内的收盘价，实际值需除以1000000
        int64_t high;                                     // k线周期内的最高价，实际值需除以1000000
        int64_t low;                                      // k线周期内的最低价，实际值需除以1000000
        int64_t total_volume;                             // k线周期内的成交总量
        int64_t total_money;                              // k线周期内的总成交金额，实际值需除以1000000
        int64_t num_trades;                               // k线周期内的总成交笔数
        char security_code[ConstField::rSecurityCodeLen]; // 证券代码
    };
    // 8 * 9 = 72

    /**  @} */
    #pragma pack(pop)

    
    #pragma pack(push, 1)
    struct Order
    {
        int status;                                       // 状态, 一个OrderStatus命名空间中定义，随着订单执行状态改变，包含在订单执行回报中
        uint64_t add_time;                                // 订单创建时间, datetime.datetime对象，为后台时间，包含在订单执行回报中
        int amount;                                       // 下单数量, 不管是买还是卖, 都是正数，下单立即返回，包含在订单执行回报中
        int filled;                                       // 已经成交的股票数量, 正数，包含在订单执行回报中
        char security_code[ConstField::rSecurityCodeLen]; // 股票代码，同下单请求的security参数，下单立即返回，包含在订单执行回报中
        uint64_t order_id;                                // 订单ID，为M-Quant系统内部生成的订单id，非委托编号，下单/撤单立即返回，包含在订单执行回报中
        double price;                                     // 委托价格，包含在订单执行回报中
        double avg_cost;                                  // 平均成交价格, 已经成交的股票的平均成交价格(一个订单可能分多次成交)，包含在订单执行回报中
        int side;                                         // 买卖方向,下单立即返回，包含在订单执行回报中,OrderSide命名空间中定义，如OrderSide::BUY
        int action;                                       // 开平行为，OrderAction命名空间中定义，期货有效
        int invest_type;                                  // 投资类型，InvestType命名空间中定义，报单立即返回，期货有效
        int close_direction;                              // 平仓类型，CloseDirection命名空间中定义，报单立即返回，期货有效

        int batch_no;            // 批次号，包含在订单执行回报中
        uint64_t orig_order_id;  // 原始订单号，撤单订单有效，目前保留字段，撤单接口的返回值中该字段有效
        char price_type[4];      // 价格类型，限价单或各种类型市价单等
        char cancel_info[256];   // 废单原因，废单有效
        int withdraw_amount;     // 撤单数量，包含在订单执行回报中
        double business_balance; // 已成交金额，包含在订单执行回报中
        double clear_balance;    // 成交费用，目前保留字段
        char entrust_prop[4];    // 委托属性，包含在订单执行回报中
                                // "0"// 买卖
                                // "N"// ETF申赎
                                // "f": // /债券质押
        int entrust_type;        // 委托类型，包含在订单执行回报中，在EntrustType命名空间中定义
        char algo_inst_id[64];   // 算法实例ID，由MQuant启动的算法实例相关订单中有此字段，包含在订单执行回报中
        char error_code[8];      // 错误码
        char inst_id[64];        // Mquant实例ID，仅查询订单返回
        char trader_name[32];    // 交易员名
        char entrust_no[64];     // 柜台委托编号，2019.11.19添加
        int covered_flag;        // 期权备兑标志，OptionCoveredFlag命名空间中定义，2019.11.19添加，期权有效
        int last_amount;         // 本次成交数量，仅推送有效，2020.3.2添加
        double last_price;       // 本次成交价格，仅推送有效，2020.3.2添加
        char fund_account[20];
        char security_exchange[4]; // 市场,SecurityExchangeType命名空间中定义
    };

    struct Execution
    {
        uint64_t time;                                    // 成交时间，查询及推送返回
        int amount;                                       // 成交数量，查询及推送返回
        double price;                                     // 成交价格，查询及推送返回
        char trade_id[64];                                // 柜台成交编号，2019.11.19新增，查询及推送返回
        uint64_t order_id;                                // 订单id，查询及推送返回
        char security_code[ConstField::rSecurityCodeLen]; // 股票代码，下单立即返回，查询及推送返回
        int entrust_type;                                 // 委托类型，EntrustType命名空间中定义，查询及推送返回
        double business_balance;                          // 成交金额，查询及推送返回
        double cost_balance;                              // 成交费用，目前保留字段
        uint64_t orig_order_id;                           // 原始订单号，撤单订单有效，目前保留字段
        int side;                                         // 买/卖方向,OrderSide命名空间中定义，查询及推送返回
        char algo_inst_id[64];                            // 算法实例ID，由MQuant启动的算法实例相关订单中有此字段，包含在订单执行回报中
        char inst_id[64];                                 // Mquant实例ID，仅查询成交返回
        char trader_name[32];                             // 交易员，查询及推送返回
        int action;                                       // 开平行为，OrderAction命名空间中定义，期货有效
        int invest_type;                                  // 投资类型，InvestType命名空间中定义，报单立即返回，期货有效
        int close_direction;                              // 平仓类型，CloseDirection命名空间中定义，报单立即返回，期货有效
        char entrust_no[64];                              // 柜台委托编号， 2019.11.19新增
        char priceType[4];                                // 价格类型，限价单或各种类型市价单等
        char fund_account[20];
        char security_exchange[4]; // 市场,SecurityExchangeType命名空间中定义
    };
    /// 单只标的的持仓信息
    struct Position
    {
        char security_code[ConstField::rSecurityCodeLen]; // 标的代码
        double price;                                     // 最新行情价格
        double hold_cost;                                 // 持仓成本价
        uint64_t init_time;                               // 建仓时间，格式为 datetime.datetime,该字段目前保留
        uint64_t transact_time;                           // 最后交易时间，格式为 datetime.datetime,该字段目前保留
        int64_t total_amount;                             // 总仓位
        int64_t closeable_amount;                         // 可卖出的仓位
        int64_t locked_amount;                            // 冻结仓位，期货、期权中包含多头冻结+空头冻结
        double value;                                     // 标的价值，计算方法是: price * (total_amount + locked_amount) ，仅支持A股
        int64_t redemption_num;                           // 可申赎数
        int position_prop;                                // 仓位类型，PositionProp命名空间中定义，股、债、基为多仓，期货分多仓和空仓
        int64_t init_amount;                              // 期初数量，当日不变
        //////以下字段期货专用
        double open_cost;     // 开仓均价，股票和hold_cost相同，期货=开仓成本/（总持仓*合约乘数）
        int64_t today_amount; // 今天开的仓位
        int64_t old_amount;   // 昨持仓
        double occupy_margin; // 占用保证金
        // 开仓：交易前已占用保证金+开仓价*开仓数量*合约乘数*保证金比例；
        // 平仓：交易前已占用保证金—开仓价*平今仓数量*合约乘数*保证金比例—昨结算价*平昨仓数量*合约乘数*保证金比例
        double close_pos_profit;      // 平仓盈亏（盯市），只有平仓的合约才会计入平仓盈亏,老仓采用昨结算价计算
        double close_profit_by_trade; // 平仓盈亏（逐笔）,只有平仓的合约才会计入平仓盈亏,老仓采用开仓价计算
        double commission;            // 手续费
        int contract_multiplier;      // 合约乘数
        int invest_type;              // 投资类型，InvestType命名空间中定义，报单立即返回，期货有效
        int option_hold_type;         // 期权持仓类型,OptionHoldType命名空间中定义
        char fund_account[20];        // 资金账号
        char stock_account[20];       // 股东账号
        char security_exchange[4];    // 市场,SecurityExchangeType命名空间中定义
    };

    struct FundInfo
    {
        double available_cash;    // 可用资金, 可用来购买证券的资金
        double locked_cash;       // 挂单锁住资金，冻结资金
        double total_value;       // 总资产, 包括现金, 保证金, 仓位的总价值, 可用来计算收益
        double starting_cash;     // 初始资金, 暂时不可用
        double positions_value;   // 持仓价值, 股票基金才有持仓价值, 期货为0,暂时不可用
        double settled_cash;      // 新增字段，期初资金
        double hold_cash;         // 当前资金余额
        double market_value;      // 证券市值
        double transferable_cash; // 可取资金余额

        // 以下字段为期货专用
        double available_margin;      // 可用保证金,期货可用
        double occupied_margin;       // 占用保证金，期货可用
        double pre_balance;           // 期初权益，期货可用
        double current_balance;       // 客户权益，期货可用
        double risk_level;            // 占用保证金/客户权益
        double holding_profit;        // 盯市盈亏，以持仓成本计算的浮动盈亏
        double close_pos_profit;      // 平仓盈亏（盯市），只有平仓的合约才会计入平仓盈亏,老仓采用昨结算价计算
        double close_profit_by_trade; // 平仓盈亏（逐笔）,只有平仓的合约才会计入平仓盈亏,老仓采用开仓价计算
        double commission;            // 手续费
        double frozen_commission;     // 冻结手续费
        double freezed_deposit;       // 委托冻结保证金
        char fund_account[20];        // 资金账号
        double hk_available_cash;     // 港股通可用资金
    };

    struct FundFlowDetial
    {
        double in_flow_value;  // 流入金额
        int64_t in_flow_qty;   // 流入股数
        double out_flow_value; // 流出金额
        int64_t out_flow_qty;  // 流出股数
    };

    struct FundFlow
    {
        char security_code[ConstField::rSecurityCodeLen];
        uint64_t datetime;
        FundFlowDetial super_large_order;
        FundFlowDetial large_order;
        FundFlowDetial middle_order;
        FundFlowDetial small_order;
        FundFlowDetial main_order;
    };

    #pragma pack(pop)

    enum class DataType
    {
        ORDER = 0,
        TRADE = 1,
        SNAPSHOT = 2,
        UNKNOWN = 3
    };

    struct RealtimeData
    {
        DataType dataType;
        const char *rawData;
    };

    struct RealtimeIndex
    {
        DataType dataType;
        uint64_t index;
    };

    enum class DataState
    {
        ALL_AVAILABLE,
        ORDER_SNAPSHOT_ONLY,
        TRADE_SNAPSHOT_ONLY,
        ORDER_TRADE_ONLY,
        ORDER_ONLY,
        TRADE_ONLY,
        SNAPSHOT_ONLY,
        UNKONWN
    };

    enum class Side
    {
        BUY = 0,    // 买 深交所/上交所
        SELL = 1,   // 卖 深交所/上交所
        BORROW = 2, // 借入 深交所
        LOAN = 3,   // 借出 深交所
        UNKNOWN = 4 // 未知 上交所
    };

    enum class OrderType
    {
        MARKET = 0,      // 市价 深交所
        LIMIT = 1,       // 限价 深交所
        MARKET_BEST = 2, // 本方最优 深交所
        ADDED = 10,      // 新增委托订单 上交所
        CANCEL = 11      // 撤销委托订单 上交所
    };

    enum class TradeType
    {
        DEAL = 0,  // 成交 深交所
        CANCEL = 1 // 撤销 深交所
    };

    enum class MixedRecordType : uint8_t
    {
        ORDER = 0,
        TRADE = 1,
        END = 255
    };

    #pragma pack(push, 1)
    struct MDMixedRecord
    {
        uint8_t data_type; // MixedRecordType
        union
        {
            MDOrder order;
            MDTrade trade;
        };
    };
    // 1+max(56,72)=73
    static_assert(sizeof(MDMixedRecord) == 73, "MDMixedRecord size must be 73 bytes");

    // 文件头结构
    struct FileHeader
    {
        uint32_t version = 1;   // 文件格式版本
        uint64_t data_offset;   // 数据起始位置
        uint64_t meta_offset;   // 元数据索引位置
        uint32_t total_stocks;  // 总股票数量
        uint64_t total_records; // 总记录条数
    };
    // 4+8+8+4+8 = 32

    // 数据块头
    struct RecordBlockHeader
    {
        uint64_t original_size;                        // 原始数据大小
        uint64_t compressed_size;                      // 压缩后大小
        uint64_t record_count;                         // 本块记录数
        uint64_t mmap_expand_size;                     // mmap分配大小，包含BlockHeader和压缩后数据总大小，与内存页4k对齐
    };
    // 8+8+8+8 = 32

    struct BlockHeader
    {
        uint64_t original_size;                        // 原始数据大小
        uint64_t compressed_size;                      // 压缩后大小
        uint64_t record_count;                         // 本块记录数
        uint64_t mmap_expand_size;                     // mmap分配大小，包含BlockHeader和压缩后数据总大小，与内存页4k对齐
        char stock_code[ConstField::rSecurityCodeLen]; // 股票代码
    };
    // 8+8+8+8+8 = 40

    // 元数据索引项
    struct RecordStockMeta
    {
        SecurityCodeType stock_code;
        uint32_t order_count;    // order记录数
        uint32_t trade_count;    // trade记录数
    };
    // 8+4+4 = 16
    struct StockMeta
    {
        char stock_code[ConstField::rSecurityCodeLen];
        uint64_t offset;          // 数据块起始位置
        uint64_t compressed_size; // 压缩后总大小
        uint64_t original_size;   // 原始数据总大小
        uint32_t record_count;    // 总记录数
    };
    // 8+8+8+8+4 = 36
    #pragma pack(pop)


    constexpr uint32_t kFileMagic  = 0x5441444D; // "MDAT"
    constexpr uint32_t kBlockMagic = 0x4B4C424D; // "MBLK"
    constexpr uint32_t kIndexMagic = 0x5844494D; // "MIDX"

    enum class CodecType : uint32_t {
        NONE = 0,
        ZSTD = 1,
        LZ4  = 2,
    };

    enum FileFlags : uint32_t {
        FILE_FLAG_NONE            = 0,
        FILE_FLAG_HAS_FILE_FOOTER = 1u << 0,
        FILE_FLAG_FIXED_ALIGN     = 1u << 1,
    };

    struct alignas(64) DataFileHeader {
        uint16_t version;             // 文件格式版本
        uint16_t header_size;         // sizeof(FileHeader)，固定64
        uint32_t magic;               // 文件魔数，固定 kFileMagic
        
        uint32_t flags;               // FileFlags
        uint32_t align_size;          // block对齐粒度，例如4096；0表示不对齐

        uint32_t default_codec;       // 默认压缩算法，见 CodecType
        uint32_t schema_version;      // payload记录格式版本

        uint32_t trading_day;         // 交易日，例如 20260419
        uint32_t market_id;           // 市场/数据源ID

        uint32_t complete_flag;       // 1=正常封口，0=未完成/异常中断
        uint32_t header_crc32;        // FileHeader CRC，计算时本字段置0

        uint64_t first_block_offset;  // 第一个block的文件偏移
        uint64_t logical_end_offset;  // 有效数据结束位置（最后一个有效字节下一位）
        uint64_t block_count;         // 成功写入的block数量
    };

    struct alignas(64) DataBlockHeader {
        uint16_t version;                // block格式版本
        uint16_t header_size;            // sizeof(BlockHeader)，固定64
        uint32_t magic;                  // block魔数，固定 kBlockMagic
        
        uint32_t block_total_size;       // block总大小（含padding）
        uint32_t record_count;           // block内总记录数

        uint32_t channel_count;          // ChannelHeader个数
        uint32_t symbol_count;           // block内涉及symbol数量（统计值）

        uint32_t channel_headers_offset; // 相对block起始offset
        uint32_t channel_headers_size;   // ChannelHeader区总字节数

        uint32_t payload_offset;         // 相对block起始offset
        uint32_t raw_size;               // 解压后payload大小
        uint32_t compressed_size;        // 压缩后payload大小

        uint32_t payload_crc32;          // 压缩后payload CRC
        uint32_t header_crc32;           // BlockHeader CRC，计算时本字段置0

        uint32_t block_id;               // block编号，文件内递增
        uint64_t file_offset;            // block在文件中的起始偏移，便于校验/恢复
    };

    struct alignas(64) DataChannelHeader {
        int32_t channel_id;             // channel编号
        uint32_t record_count;           // 本channel在block内的记录数

        int64_t seq_begin;              // 本block内该channel最小seq
        int64_t seq_end;                // 本block内该channel最大seq

        int64_t ts_begin_ns;            // block最小时间戳
        int64_t ts_end_ns;              // block最大时间戳

        uint8_t reserved[24];
    };

    static_assert(sizeof(DataFileHeader)   == 64, "FileHeader size mismatch");
    static_assert(sizeof(DataBlockHeader)  == 64, "BlockHeader size mismatch");
    static_assert(sizeof(DataChannelHeader)== 64, "ChannelHeader size mismatch");

    struct alignas(64) IndexFileHeader {
        uint16_t version;                // idx文件格式版本
        uint16_t header_size;            // sizeof(IndexFileHeader)，固定64
        uint32_t magic;                  // idx文件魔数，固定 kIndexMagic

        uint32_t schema_version;         // 对应dat中的schema_version
        uint32_t trading_day;            // 交易日，例如 20260419

        uint32_t market_id;              // 市场/数据源ID
        uint32_t complete_flag;          // 1=正常写完，0=未完成/异常中断

        uint32_t block_index_offset;     // BlockIndexEntry区起始offset
        uint32_t block_index_size;       // BlockIndexEntry区总字节数

        uint32_t symbol_header_offset;   // SymbolHeader区起始offset
        uint32_t symbol_header_size;     // SymbolHeader区总字节数

        uint32_t symbol_block_offset;    // symbol block index数组区起始offset
        uint32_t symbol_block_size;      // symbol block index数组区总字节数

        uint32_t header_crc32;           // IndexFileHeader CRC，计算时本字段置0
        uint32_t reserved0;              // 补齐到8字节边界
    };

    struct alignas(64) IndexBlockEntry {
        uint32_t block_total_size;       // block总大小（含padding）
        uint32_t record_count;           // block内总记录数

        uint32_t channel_count;          // block内channel数量
        uint32_t symbol_count;           // block内symbol数量

        uint64_t ts_begin_ns;            // block起始时间戳
        uint64_t ts_end_ns;              // block结束时间戳

        uint64_t block_id;               // block编号，对应dat中的BlockHeader.block_id
        uint64_t file_offset;            // 该block在dat中的起始offset
    };

    struct alignas(32) IndexSymbolHeader {
        int32_t channel_id;              // symbol所属channel
        uint32_t total_record_count;     // 该symbol在整个文件中的总记录数

        uint32_t block_index_count;      // 该symbol出现的block数量
        uint32_t reserved0;              // 补齐到8字节边界

        uint64_t block_index_offset;     // 该symbol对应的uint32_t block_index数组在idx中的起始offset
        char security_code[8];           // symbol code
    };

    // 指向block在vector的位置
    using IndexSymbolBlockIndex = uint32_t;

    static_assert(sizeof(IndexFileHeader)  == 64, "IndexFileHeader size mismatch");
    static_assert(sizeof(IndexBlockEntry)  == 64, "BlockIndexEntry size mismatch");
    static_assert(sizeof(IndexSymbolHeader)  == 32, "SymbolHeader size mismatch");
    static_assert(sizeof(IndexSymbolBlockIndex) == 4, "SymbolBlockIndex size mismatch");

}

#endif

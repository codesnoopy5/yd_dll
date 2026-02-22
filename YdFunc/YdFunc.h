#pragma once

// 本接口文件适用于易得V3.1.0以及以上版本

#include <float.h>

#pragma pack(1)

#ifdef __cplusplus
extern "C"
{
#endif //__cplusplus

#define MAX_NUM_DLLPARAM 32
#define YDDLL_HEADTAG    0xf32cea12

#define time_ms unsigned int

#define IS_VALID_DOUBLE(x) (x < DBL_MAX && x > DBL_MAX*-1)

#define NUM_QUOTE 5

    // K线历史数据
    struct STKHISTORY
    {
        time_ms  m_time;            // 时间 UCT 1970.1.1开始秒数
        float    m_fOpen;           // 开盘价
        float    m_fHigh;           // 最高价
        float    m_fLow;            // 最低价
        float    m_fClose;          // 收盘价
        float    m_fVolume;         // 成交量
        float    m_fAmount;         // 成交额
        int      m_nBelongDate;     // 所属交易日 YYYYMMDD
        union
        {
            struct
            {
                float   m_fHold;            // 持仓量,仅期货有效
                float   m_fSettlePrice;     // 结算价,仅期货有效
            };
            struct
            {
                WORD    m_wAdvance;         // 涨数,仅大盘有效
                WORD    m_wDecline;         // 跌数,仅大盘有效
                WORD    m_wEqual;           // 平盘数,仅大盘有效
            };
            struct
            {
                float   m_fStroke;          // 成交笔数,仅股票有效
            };
            struct
            {
                float m_fFixPriceVol;       // 盘后固定价格成交量,仅科创板有效
                float m_fFixPriceAmount;    // 盘后固定价格成交额,仅科创板有效
            };
        };
    };

    // 分笔成交数据
    struct STKTICK
    {
        time_ms  m_time;
        float    m_fPrice;
        float    m_fVolume;
        float    m_fAmount;
        float    m_fBuyPrice[NUM_QUOTE];
        float    m_fSellPrice[NUM_QUOTE];
        float    m_fBuyVol[NUM_QUOTE];
        float    m_fSellVol[NUM_QUOTE];
        WORD     m_wMsTime;
        WORD     m_wAttrib;
        int      m_nBelongDate;    // 所属交易日
        union
        {
            float   m_fHold;       // 持仓量,仅期货有效
            float   m_fStroke;     // 成交笔数,仅股票有效
        };
    };

    // 基础数据类型枚举
    enum ENUM_DATATYPE
    {
        SECOND_DATA = 0,    // 秒线
        SEC5_DATA,          // 5秒线
        TICK_DATA,          // 分笔成交
        MIN_DATA,           // 分时
        MIN1_DATA,          // 1分钟
        MIN5_DATA,          // 5分钟
        MIN15_DATA,         // 15分钟
        MIN30_DATA,         // 30分钟
        MIN60_DATA,         // 60分钟
        DAY_DATA,           // 日线
        WEEK_DATA,          // 周线
        MONTH_DATA,         // 周线
        SEASON_DATA,        // 季线
        HALFYEAR_DATA,      // 半年线
        YEAR_DATA,          // 年线
    };

    // 数据类型
    struct DATA_TYPE
    {
        ENUM_DATATYPE m_baseType : 16;  // 基础数据类型
        DWORD         m_nUnit : 16;  // 倍数 如：m_baseType=DAY_DATA, m_nUnit=2， 表示2日线; 如：m_baseType=DAY_DATA, m_nUnit<=1, 表示日线

        DATA_TYPE(ENUM_DATATYPE baseType, WORD nUnit = 0)
        {
            m_baseType = baseType;
            if (nUnit == 1) nUnit = 0;
            m_nUnit = nUnit;
        }
    };

    // 公式参数数据
    struct YDPARAMDATA
    {
        // 入参的数据可以是单值数据，也可以是序列数据，二者选其一
        // 也可以通过m_pszText传入字符串

        const double  m_dSingleData;  // 单值数据

        const double* m_pdData;       // 序列数据(对于单值数据无效，为0)
        const int     m_nSize;        // 序列数据大小(对于单值数据无效，为0)
        const int     m_nBegin;       // 对于序列数据有效起始位置(对于单值数据无效，为0)
        const int     m_nEnd;         // 对于序列数据有效结束位置(对于单值数据无效，为0)

        const char* m_pszText;      // 也可以传入字符串

        double GetData(int nN) const    // 获取数据
        {
            if (m_pdData != NULL)
                return m_pdData[nN];

            return m_dSingleData;
        }

        BOOL IsValidData(int nN) const  // 是否有效数据
        {
            if (m_pdData != NULL)
                return nN >= m_nBegin && nN <= m_nEnd && nN >= 0 && IS_VALID_DOUBLE(m_pdData[nN]);
            return IS_VALID_DOUBLE(m_dSingleData);
        }
    };

    struct DLLCALCINFO
    {
        const DWORD         m_dwHeadTag;                // 头标识 = YDDLL_HEADTAG
        const DWORD         m_dwSize;                   // 结构大小
        const DWORD         m_dwVersion;                // 调用软件版本(V2.1.1 : 200010001)
        const char          m_strStkLabel[16];          // 股票代码
        const BOOL          m_bIndex;                   // 指数

        const BOOL          m_bRunByBar;                // 运算模式m_bRunByBar==TRUE表示逐K线运算模式，否则为序列运算模式
        const BOOL          m_bOnlyCalcLastBar;         // 逐K线运算模式下，在盘中实时行情到来时是否只刷新最后一根K线
        const BOOL          m_bInstantCalc;             // 是否是盘中即时行情触发的计算

        const int           m_nCurBarPos;               // 当前计算位置, 此次调用只执行当前位置的计算
        const DATA_TYPE     m_dataType;                 // 数据类型
        const int           m_nPower;                   // 除权: 0:不除权 1:向前除权  2:向后除权
        const int           m_nNumData;                 // 数据数量(m_pStkHistData或m_pStkTick的数据大小)
        const STKHISTORY* m_pStkHistData;             // 历史K线数据, 当m_dataType==DATA_TYPE(TICK_DATA, 0)时为NULL
        const STKTICK* m_pStkTickData;             // 分笔成交数据，仅当m_dataType==DATA_TYPE(TICK_DATA, 0)时有效，否则为NULL

        const int           m_nNumParam;                // 参数个数
        const YDPARAMDATA* m_pParam[MAX_NUM_DLLPARAM]; // 参数数据，最大32个参数

        double* m_pResultBuf;               // 结果缓冲区，也是返回值，大小等于m_nNumData
        char* m_pszResultText;            // 结果也可以返回一个字符串

        const double* m_pdFinData;                // 32项财务数据
    };

#pragma pack()

    // 导出函数声明
    __declspec(dllexport) int WINAPI ACCOUNT_ALL(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI AUTO_CANCEL(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI AUTO_TRADE(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI ASK_BID(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI STOCK_POSITIONS(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI ADD_TO_BLOCK(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI IS_IN_BLOCK(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI RESET_STATUS(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI GET_KEY(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI ADD_KEY(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI DEL_KEY(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI GET_BLOCK_SIZE(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI TODAY_ENTRUSTS(DLLCALCINFO* pData);
    __declspec(dllexport) int WINAPI SET_KEY(DLLCALCINFO* pData);

#ifdef __cplusplus
}
#endif //__cplusplus
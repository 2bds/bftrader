#include "mdsm.h"
#include "ThostFtdcMdApi.h"
#include "ctpmgr.h"
#include "encode_utils.h"
#include "file_utils.h"
#include "logger.h"
#include "servicemgr.h"
#include <QDateTime>
#include <QDir>
#include <QMap>
#include <QTimer>

///////////
class MdSmSpi : public CThostFtdcMdSpi {
public:
    explicit MdSmSpi(MdSm* sm)
        : sm_(sm)
    {
    }

private:
    void OnFrontConnected() override
    {
        info(__FUNCTION__);
        emit sm()->statusChanged(MDSM_CONNECTED);
    }

    // 如果网络异常，会直接调用OnFrontDisconnected，需要重置状态数据=
    // 网络错误当再次恢复时候，会自动重连重新走OnFrontConnected
    void OnFrontDisconnected(int nReason) override
    {
        info(QString().sprintf("MdSmSpi::OnFrontDisconnected,nReason=0x%x",
            nReason));

        emit sm()->statusChanged(MDSM_DISCONNECTED);
    }

    // errorId=7，msg=CTP:还没有初始化=
    void OnRspUserLogin(CThostFtdcRspUserLoginField* pRspUserLogin,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override
    {
        info(__FUNCTION__);
        if (bIsLast) {
            if (isErrorRsp(pRspInfo, nRequestID)) {
                emit sm()->statusChanged(MDSM_LOGINFAIL);
            } else {
                emit sm()->statusChanged(MDSM_LOGINED);
            }
        }
    }

    // logout在tdapi里面是有效的,mdapi无效不用处理=
    void OnRspUserLogout(CThostFtdcUserLogoutField* pUserLogout,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override {}

    void OnRspError(CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override
    {
        if (bIsLast) {
            info(__FUNCTION__);
            isErrorRsp(pRspInfo, nRequestID);
        }
    }

    // 订阅成功了也会调用,目前是不管啥都返回订阅成功=
    void OnRspSubMarketData(
        CThostFtdcSpecificInstrumentField* pSpecificInstrument,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override
    {
        if (!isErrorRsp(pRspInfo, nRequestID) && pSpecificInstrument) {
            QString iid = pSpecificInstrument->InstrumentID;
            got_ids_ << iid;
        }

        if (bIsLast && got_ids_.length()) {
            QString ids;
            for (auto id : got_ids_) {
                ids = ids + id + ";";
            }
            info(QString().sprintf("total sub ids:%d,%s", got_ids_.length(),
                ids.toUtf8().constData()));
        }
    }

    void OnRspUnSubMarketData(
        CThostFtdcSpecificInstrumentField* pSpecificInstrument,
        CThostFtdcRspInfoField* pRspInfo,
        int nRequestID,
        bool bIsLast) override {}

    void OnRtnDepthMarketData(
        CThostFtdcDepthMarketDataField* pDepthMarketData) override
    {
        QString id = pDepthMarketData->InstrumentID;
        auto rb = g_sm->ctpMgr()->getRingBuffer(id);
        if (!isValidTick(rb, pDepthMarketData)) {
            return;
        }
        auto preTick = (CThostFtdcDepthMarketDataField*)rb->get(rb->head());
        void* curTick = rb->put(pDepthMarketData);
        emit sm()->gotTick(curTick, preTick);
    }

private:
    bool isErrorRsp(CThostFtdcRspInfoField* pRspInfo, int reqId)
    {
        if (pRspInfo && pRspInfo->ErrorID != 0) {
            info(QString().sprintf(
                "<==错误，reqid=%d,errorId=%d，msg=%s", reqId, pRspInfo->ErrorID,
                gbk2utf16(pRspInfo->ErrorMsg).toUtf8().constData()));
            return true;
        }
        return false;
    }

    MdSm* sm() { return sm_; }

    void resetData()
    {
        got_ids_.clear();
        g_sm->ctpMgr()->freeRingBuffer();
    }

    void info(QString msg) { g_sm->logger()->info(msg); }

    // 每次收盘/断网/崩溃/退出等等,都会清空ringbufer，需要重来一下下面的逻辑=
    // 1.总成交量为0的：无效=
    // 2.确定第一个有效tick：交易日期+时间，和当前时间偏离在3分钟之外：无效=
    // 3.确定后续tick是否有效：和上一个tick相比，总成交量不变的：无效=
    bool isValidTick(RingBuffer* rb, CThostFtdcDepthMarketDataField* curTick)
    {
        if (curTick->Volume == 0) {
            return false;
        }
        auto preTick = (CThostFtdcDepthMarketDataField*)rb->get(rb->head());
        if (!preTick) {
            // ActionDay 指当时的日期
            // TradingDay 是指当时的交易日期，夜盘算下一个交易的=
            QDateTime curDateTime = QDateTime::currentDateTime();
            QString tickDateTimeStr = QString().sprintf("%s %s.%03d", curTick->ActionDay, curTick->UpdateTime, curTick->UpdateMillisec);
            QDateTime tickDateTime = QDateTime::fromString(tickDateTimeStr, "yyyyMMdd hh:mm:ss.zzz");
            qint64 delta = qAbs(curDateTime.msecsTo(tickDateTime));
            if (delta >= 3 * 60 * 1000) {
                return false;
            }
        } else {
            if (preTick->Volume == curTick->Volume) {
                return false;
            }
        }
        return true;
    }

private:
    MdSm* sm_;
    QStringList got_ids_;

    friend MdSm;
};

///////////
MdSm::MdSm(QObject* parent)
    : QObject(parent)
{
}

MdSm::~MdSm() {}

bool MdSm::init(QString userId,
    QString password,
    QString brokerId,
    QString frontMd,
    QString flowPathMd)
{
    userId_ = userId;
    password_ = password;
    brokerId_ = brokerId;
    frontMd_ = frontMd;
    flowPathMd_ = flowPathMd;

    // check
    if (userId_.length() == 0 || password_.length() == 0 || brokerId_.length() == 0 || frontMd_.length() == 0 || flowPathMd_.length() == 0) {
        return false;
    }
    return true;
}

void MdSm::start()
{
    info(__FUNCTION__);

    if (mdapi_ != nullptr) {
        qFatal("mdapi_!=nullptr");
        return;
    }

    QDir dir;
    dir.mkpath(flowPathMd_);
    mdapi_ = CThostFtdcMdApi::CreateFtdcMdApi(flowPathMd_.toStdString().c_str());
    mdspi_ = new MdSmSpi(this);
    mdapi_->RegisterSpi(mdspi_);
    mdapi_->RegisterFront((char*)qPrintable(frontMd_));
    mdapi_->Init();
}

void MdSm::stop()
{
    info(__FUNCTION__);

    if (mdapi_ == nullptr) {
        qFatal("mdapi_==nullptr");
        return;
    }

    this->mdapi_->RegisterSpi(nullptr);
    this->mdapi_->Release();

    this->mdapi_ = nullptr;
    delete this->mdspi_;
    this->mdspi_ = nullptr;
    emit this->statusChanged(MDSM_STOPPED);
}

void MdSm::info(QString msg)
{
    g_sm->logger()->info(msg);
}

QString MdSm::version()
{
    return CThostFtdcMdApi::GetApiVersion();
}

void MdSm::resetData()
{
    this->mdspi_->resetData();
}

void MdSm::login(unsigned int delayTick, QString robotId)
{
    info(__FUNCTION__);
    QTimer::singleShot(delayTick, this, [=] {
        if (!g_sm->ctpMgr()->running()) {
            info("ctpmgr stop,ignore MdSm::login");
            return;
        }
        CThostFtdcReqUserLoginField req;
        memset(&req, 0, sizeof(req));
        strncpy(req.BrokerID, brokerId_.toStdString().c_str(), sizeof(req.BrokerID) - 1);
        strncpy(req.UserID, userId_.toStdString().c_str(), sizeof(req.UserID) - 1);
        strncpy(req.Password, password_.toStdString().c_str(), sizeof(req.Password) - 1);
        int result = mdapi_->ReqUserLogin(&req, ++reqId_);
        info(QString().sprintf("CmdMdLogin,reqId=%d,result=%d", reqId_, result));
        //  被流控，一秒后重来=
        if (result == -3) {
            login(retryAfterMsec_, robotId);
        } else if (result == 0) {
            //发单成功，发信号，<reqId,robotId>，便于上层跟踪=
            emit requestSent(reqId_, robotId);
        }
    });
}

void MdSm::subscrible(QStringList ids,
    unsigned int delayTick,
    QString robotId)
{
    info(__FUNCTION__);
    QTimer::singleShot(delayTick, this, [=] {
        if (!g_sm->ctpMgr()->running()) {
            info("ctpmgr stop,ignore MdSm::login");
            return;
        }
        QList<std::string> std_ids;
        char** cids = new char*[ids.length()];
        for (int i = 0; i < ids.length(); i++) {
            std_ids.append(ids.at(i).toStdString());
            cids[i] = (char*)std_ids.at(i).c_str();
        }
        int result = mdapi_->SubscribeMarketData(cids, ids.length());
        delete[] cids;
        info(QString().sprintf("CmdMdSubscrible,result=%d", result));
        if (result == -3) {
            subscrible(ids, retryAfterMsec_, robotId);
        } else if (result == 0) {
            g_sm->ctpMgr()->initRingBuffer(sizeof(CThostFtdcDepthMarketDataField), ids);
        }
    });
}

#pragma once
#include "hibiki/tuner.h"

namespace hibiki {

// Hardware boundary. The default backend retains the original Eagle behavior.
class TunerBackend {
public:
    virtual ~TunerBackend() = default;
    virtual const char* name() const = 0;
    virtual std::string last_error() const { return {}; }
    virtual int Init(const TunerConfig&) = 0;
    virtual int OpenTransport(const TunerConfig&) { return 1; }
    virtual int Close() = 0;
    virtual int SetPara(uint32_t) = 0;
    virtual int SetWorkMode(int) = 0;
    virtual int IsLock() = 0;
    virtual int GetQuality() = 0;
    virtual int GetStrength() = 0;
    virtual int ReadTS(uint8_t*, uint32_t) = 0;
    virtual int AddPid(int, int, int) = 0;
    virtual int RemovePid(int, int, int) = 0;
    virtual int ResetPid() = 0;
    virtual int PIDOnOff(bool) = 0;
    virtual int GetSplitMode() = 0;
    virtual int i2c_read(uint8_t, uint8_t, uint8_t*, int) = 0;
    virtual int i2c_write(uint8_t, uint8_t, uint8_t*, int) = 0;
    virtual bool ReadStatus(AtscStatus&) = 0;
    virtual bool WaitLock(int, AtscStatus*) = 0;
    virtual uint8_t ts_endpoint() const = 0;
    virtual bool initialized() const = 0;
};

} // namespace hibiki

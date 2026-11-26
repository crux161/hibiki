#include "hibiki/tuner.h"
#include "hibiki/tuner_backend.h"
#include "eagle_backend.h"
#include "em28xx_backend.h"

namespace hibiki {
std::atomic<bool> g_running{true};

Tuner::Tuner() : backend_(std::make_unique<EagleBackend>()) {}
Tuner::Tuner(std::unique_ptr<TunerBackend> backend) : backend_(std::move(backend)), injected_(true) {}
Tuner::~Tuner() { Close(); }

void Tuner::select_backend(const TunerConfig& cfg) {
    if (!injected_ && (!backend_ || kind_ != cfg.backend)) {
        Close();
        kind_ = cfg.backend;
        if (cfg.backend == TunerBackendKind::Em28xx) backend_ = std::make_unique<Em28xxBackend>();
        else backend_ = std::make_unique<EagleBackend>();
    }
}
int Tuner::Init(const TunerConfig& cfg) {
    select_backend(cfg);
    return backend_ ? backend_->Init(cfg) : 1;
}
int Tuner::OpenTransport(const TunerConfig& cfg) {
    select_backend(cfg);
    return backend_ ? backend_->OpenTransport(cfg) : 1;
}
int Tuner::Close() { return backend_ ? backend_->Close() : 0; }
int Tuner::SetPara(uint32_t f) { return backend_ ? backend_->SetPara(f) : 1; }
int Tuner::SetWorkMode(int m) { return backend_ ? backend_->SetWorkMode(m) : 0; }
int Tuner::IsLock() { return backend_ ? backend_->IsLock() : 0; }
int Tuner::GetQuality() { return backend_ ? backend_->GetQuality() : 0; }
int Tuner::GetStrength() { return backend_ ? backend_->GetStrength() : 0; }
int Tuner::ReadTS(uint8_t* p, uint32_t n) { return backend_ ? backend_->ReadTS(p, n) : 0; }
int Tuner::AddPid(int p, int t, int o) { return backend_ ? backend_->AddPid(p, t, o) : 0; }
int Tuner::RemovePid(int p, int t, int o) { return backend_ ? backend_->RemovePid(p, t, o) : 0; }
int Tuner::ResetPid() { return backend_ ? backend_->ResetPid() : 0; }
int Tuner::PIDOnOff(bool on) { return backend_ ? backend_->PIDOnOff(on) : 0; }
int Tuner::GetSplitMode() { return backend_ ? backend_->GetSplitMode() : 0; }
int Tuner::i2c_read(uint8_t s, uint8_t r, uint8_t* p, int n) {
    return backend_ ? backend_->i2c_read(s, r, p, n) : 1;
}
int Tuner::i2c_write(uint8_t s, uint8_t r, uint8_t* p, int n) {
    return backend_ ? backend_->i2c_write(s, r, p, n) : 1;
}
bool Tuner::ReadStatus(AtscStatus& out) { return backend_ && backend_->ReadStatus(out); }
bool Tuner::WaitLock(int ms, AtscStatus* out) { return backend_ && backend_->WaitLock(ms, out); }
uint8_t Tuner::ts_endpoint() const { return backend_ ? backend_->ts_endpoint() : 0x82; }
bool Tuner::initialized() const { return backend_ && backend_->initialized(); }
const char* Tuner::backend_name() const { return backend_ ? backend_->name() : "fx2-eagle"; }
std::string Tuner::last_error() const { return backend_ ? backend_->last_error() : std::string(); }

// -------- ATSC channel plan --------
uint32_t atsc_channel_freq_hz(int ch) {
    // Channel center = lower edge + 3 MHz, per resources/atsc.pdf (the ATSC
    // pilot sits ~0.309441 MHz above the lower edge, 2.690559 MHz below center).
    // Using the exact center maximizes carrier-recovery margin on weak signals.
    if (ch >= 2  && ch <= 4)  return 57000000u  + (uint32_t)(ch - 2)  * 6000000u;
    if (ch >= 5  && ch <= 6)  return 79000000u  + (uint32_t)(ch - 5)  * 6000000u;
    if (ch >= 7  && ch <= 13) return 177000000u + (uint32_t)(ch - 7)  * 6000000u;
    if (ch >= 14 && ch <= 51) return 473000000u + (uint32_t)(ch - 14) * 6000000u;
    return 0;
}


} // namespace hibiki

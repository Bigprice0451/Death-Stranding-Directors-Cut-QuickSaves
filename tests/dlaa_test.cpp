#include "dlaa_ngx.hpp"
#include "graphics_diagnostics.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
using namespace qs::dlaa_detail;
namespace {
void check(bool value, const char *reason) { if (!value) throw std::runtime_error(reason); }
constexpr U32 failure = 0xbad00005;
struct FakeDriver {
    std::unordered_map<std::string,U32> p;
    std::vector<int> queries;
    bool deny_dlaa{}, return_scaled{}, deny_preset{}, fail_create{};
    int create_mode{};
    U32 create_preset{};
};
void set_u(void *driver, const char *k, U32 value) {
    auto &d = *static_cast<FakeDriver *>(driver);
    if (d.deny_preset && std::string(k) == preset_key && value == preset_k) return;
    d.p[k] = value;
}
void set_i(void *d, const char *k, int value) { set_u(d,k,value); }
U32 get_u(void *driver, const char *k, U32 *value) {
    auto &p = static_cast<FakeDriver *>(driver)->p;
    auto it = p.find(k);
    if (it == p.end()) return failure;
    *value = it->second;
    return 1;
}
U32 get_i(void *d, const char *k, int *value) { U32 u{}; const U32 result = get_u(d,k,&u); *value=u; return result; }
const Params api{set_i, set_u, get_i, get_u};
bool query(void *driver, U32 w, U32 h, int mode, bool, U32 *rw, U32 *rh, float *sharpness) {
    auto &d = *static_cast<FakeDriver *>(driver);
    d.queries.push_back(mode);
    if (mode == dlaa && d.deny_dlaa) return false;
    const int divisor = mode == dlaa && !d.return_scaled ? 1 : 2;
    if (rw) *rw = w/divisor;
    if (rh) *rh = h/divisor;
    if (sharpness) *sharpness = mode == dlaa ? 0.0f : 0.5f;
    return true;
}
U32 create_driver(void *, U32, void *driver, void **handle) {
    auto &d = *static_cast<FakeDriver *>(driver);
    d.create_mode = d.p[quality_key];
    d.create_preset = d.p.count(preset_key) ? d.p.at(preset_key) : 0;
    if (d.fail_create) return failure;
    *handle = driver;
    return 1;
}
}
int main() {
    // A failed or idle renderer must still leave useful evidence. A heartbeat
    // is rate limited independently of frame count or successful DLAA output.
    qs::GraphicsDiagnosticClock clock;
    check(clock.take(0) && !clock.take(14999) && clock.take(15000) && !clock.take(15001),
        "graphics diagnostic heartbeat interval");
    qs::DlaaStatus diagnostics;
    diagnostics.hooks_ready=true; diagnostics.evaluation_failures=3;
    diagnostics.evaluations=100; diagnostics.creations=2; diagnostics.releases=1;
    diagnostics.history_resets=7; diagnostics.last_frame_reset=true;
    diagnostics.last_result=failure; diagnostics.last_evaluation_at=14500;
    diagnostics.detail="NGX reported an evaluation error.";
    auto report=qs::graphics_diagnostic_text(diagnostics,15000);
    check(report.find("failures=3")!=std::string::npos &&
        report.find("evaluations=100")!=std::string::npos &&
        report.find("history_resets=7; last_frame_reset=1")!=std::string::npos &&
        report.find("creations=2; releases=1")!=std::string::npos &&
        report.find("result=0xbad00005")!=std::string::npos &&
        report.find("evaluation_age_ms=500")!=std::string::npos &&
        report.find(diagnostics.detail)!=std::string::npos,
        "failed frame evidence omitted from graphics diagnostics");
    diagnostics={};
    report=qs::graphics_diagnostic_text(diagnostics,15000);
    check(report.find("evaluations=0; failures=0")!=std::string::npos &&
        report.find("evaluation_age_ms=unavailable")!=std::string::npos,
        "idle graphics diagnostics implied a successful frame");
    const Size out{3360,1440}; Size answer;
    FakeDriver d;
    float sharpness = -1; bool native = false;
    check(optimal(query,&d,out,quality,false,&answer.w,&answer.h,&sharpness,true,native) && native && same(out,answer), "native sizing");
    for (bool denial : {false,true}) {
        d.queries.clear(); d.return_scaled = !denial; d.deny_dlaa = denial;
        check(optimal(query,&d,out,quality,true,&answer.w,&answer.h,&sharpness,true,native) && !native &&
            answer.w==1680 && answer.h==720 && sharpness==0.5f && d.queries==std::vector<int>{dlaa,quality}, "unsupported or scaled fallback");
    }
    for (int mode : {0,1,2,3,4}) {
        d.queries.clear();
        optimal(query,&d,out,mode,true,&answer.w,&answer.h,&sharpness,mode!=quality,native);
        check(!native && d.queries==std::vector<int>{mode}, "preserve disabled / other modes");
    }
    d.p = {{"Width",3360},{"Height",1440},{"OutWidth",3360},{"OutHeight",1440},{quality_key,quality},{preset_key,10}};
    Created created; void *handle=nullptr;
    check(success(create(api,create_driver,nullptr,1,&d,&handle,true,out,created)) && created.preset_requested && handle==&d &&
        d.create_mode==dlaa && d.create_preset==preset_k && d.p[quality_key]==quality && d.p[preset_key]==10, "scoped preset and mode");
    d.fail_create=true; created={};
    check(!success(create(api,create_driver,nullptr,1,&d,&handle,true,out,created)) && d.p[quality_key]==quality && d.p[preset_key]==10, "restore after failed creation");
    d.fail_create=false; d.deny_preset=true; created={};
    create(api,create_driver,nullptr,1,&d,&handle,true,out,created);
    check(!created.preset_requested && d.create_mode==quality && d.create_preset==10, "discard partial override");
    d.deny_preset=false; d.p.erase(preset_key); created={};
    create(api,create_driver,nullptr,1,&d,&handle,true,out,created);
    check(created.preset_requested && d.p[preset_key]==0, "unset preset restored to default");
    for (bool on : {false,true}) {
        d.p["Width"] = on ? 1600 : 3360;
        created={}; create(api,create_driver,nullptr,1,&d,&handle,on,out,created);
        check(!created.preset_requested && d.create_mode==quality && d.create_preset==0, "off or scaled inputs preserve vanilla creation");
    }
    check(native_frame(out,{3392,1440},out,out) && !native_frame(out,{2240,960},out,out) &&
        !native_frame({2240,960},out,out,out) && !native_frame(out,out,{3200,1440},out), "real frame validation");
    std::cout << "Passed Quality sizing, fallback, preset readback, vanilla restoration and evaluated-frame checks.\n";
}

#include "Sensor.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "sensor3d.h"
#include "sensor3ddefines.h"

using Clock = std::chrono::steady_clock;

static const int kConnectTimeoutMs = 5000;
static const int kScanTimeoutMs = 20000;
static const int kPollPeriodMs = 2000;

const std::vector<ParamDef>& paramDefs() {
    static const std::vector<ParamDef> defs = {
        {"ExposureTimeLimit", "Limit ekspozycji [µs]", ParamKind::Int, true, "%.0f"},
        {"ExposureTime", "Czas ekspozycji [µs]", ParamKind::Int, true, "%.0f"},
        {"LEDPower", "Moc LED [%]", ParamKind::Int, true, "%.0f"},
        {"Gain", "Wzmocnienie (gain)", ParamKind::Int, true, "%.0f"},
        {"LEDPattern", "Liczba wzorów na skan", ParamKind::Pattern, false, "%.0f"},
        {"Zmin", "Z min [mm]", ParamKind::Float, false, "%.1f"},
        {"Zmax", "Z max [mm]", ParamKind::Float, false, "%.1f"},
        {"ContrastComparisonFilterMinPhase", "Filtr kontrastu (0-1)", ParamKind::Float, true, "%.2f"},
        {"HDRMode", "HDR obrazu intensywności", ParamKind::Bool, false, "%.0f"},
        {"SubSampling", "Subsampling (mniej punktów)", ParamKind::Bool, false, "%.0f"},
        {"LEDActivate", "Projektor LED aktywny", ParamKind::Bool, false, "%.0f"},
        {"EnableExtendedMeasuringRange", "Rozszerzony zakres pomiarowy", ParamKind::Bool, false, "%.0f"},
        {"SDKSOutlierFilter", "Filtr punktów odstających", ParamKind::Bool, false, "%.0f"},
        {"SDKMedianFilter", "Filtr medianowy (wygładzanie)", ParamKind::Bool, false, "%.0f"},
    };
    return defs;
}

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static const char* errText(int rc) {
    switch (rc) {
        case SENSOR3D_OK: return "OK";
        case SENSOR3D_INVALIDSENSORHANDLE: return "nieprawidłowy uchwyt czujnika";
        case SENSOR3D_SENSORNOTCONNECTED: return "czujnik rozłączony";
        case SENSOR3D_TIMEOUT: return "przekroczony czas";
        case SENSOR3D_CONFIGURATIONERROR: return "błąd odczytu konfiguracji";
        case SENSOR3D_ARGUMENTNULLPOINTER: return "pusty argument";
        case SENSOR3D_ARGUMENTOUTOFRANGE: return "wartość poza zakresem";
        case SENSOR3D_RETURNBUFFERTOOSMALL: return "za mały bufor";
        case SENSOR3D_COMMANDNOTFOUND: return "nieznana komenda";
        case SENSOR3D_SOCKETCOMMUNICATIONERROR: return "błąd komunikacji sieciowej";
        case SENSOR3D_BUSY: return "czujnik zajęty (trwa akwizycja)";
        case SENSOR3D_DATASTREAMERROR: return "błąd strumienia danych";
        default: return "nieznany błąd";
    }
}

static std::string nowStr(const char* fmt) {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char b[64];
    std::strftime(b, sizeof(b), fmt, &tm);
    return b;
}

static std::string fmtStr(const char* fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    return b;
}

bool PointCloud::isValid(size_t i) const {
    const Point3& p = points[i];
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) return false;
    // Czujnik oznacza piksele bez pomiaru przez Z = -1 (zakres roboczy Z jest zawsze dodatni).
    return p.z > 0.0;
}

SensorController::SensorController() : rbuf_(0x20000) {
    st_.params.resize(paramDefs().size());
    worker_ = std::thread([this] { workerLoop(); });
}

SensorController::~SensorController() {
    {
        std::lock_guard<std::mutex> lk(m_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (h_) {
        Sensor3D_WriteData(h_, "SetAcquisitionStop\r");
        Sensor3D_Disconnect(h_);
        h_ = nullptr;
    }
}

void SensorController::post(const std::string& text, std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(m_);
        jobs_.emplace_back(text, std::move(fn));
        pending_++;
        st_.busy = true;
        if (st_.busyText.empty()) st_.busyText = text;
    }
    cv_.notify_all();
}

void SensorController::workerLoop() {
    auto lastPoll = Clock::now();
    for (;;) {
        std::pair<std::string, std::function<void()>> job;
        bool have = false;
        {
            std::unique_lock<std::mutex> lk(m_);
            cv_.wait_for(lk, std::chrono::milliseconds(200), [this] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            if (!jobs_.empty()) {
                job = std::move(jobs_.front());
                jobs_.pop_front();
                st_.busyText = job.first;
                have = true;
            }
        }
        if (have) {
            job.second();
            std::lock_guard<std::mutex> lk(m_);
            pending_--;
            if (pending_ == 0) {
                st_.busy = false;
                st_.busyText.clear();
            }
            lastPoll = Clock::now() - std::chrono::milliseconds(kPollPeriodMs);
        } else if (h_ && Clock::now() - lastPoll >= std::chrono::milliseconds(kPollPeriodMs)) {
            poll();
            lastPoll = Clock::now();
        }
    }
}

void SensorController::log(const std::string& msg) {
    std::lock_guard<std::mutex> lk(m_);
    st_.log.push_back(nowStr("%H:%M:%S") + "  " + msg);
    if (st_.log.size() > 300) st_.log.erase(st_.log.begin(), st_.log.begin() + 100);
}

void SensorController::setBusyText(const std::string& t) {
    std::lock_guard<std::mutex> lk(m_);
    st_.busyText = t;
}

bool SensorController::read(const char* cmd, std::string& out) {
    if (!h_) return false;
    rbuf_[0] = 0;
    int rc = Sensor3D_ReadData(h_, cmd, rbuf_.data(), (int)rbuf_.size(), 0);
    if (rc != SENSOR3D_OK) return false;
    out = rbuf_.data();
    return true;
}

bool SensorController::readNum(const std::string& cmd, double& out) {
    std::string s;
    if (!read(cmd.c_str(), s) || s.empty()) return false;
    char* end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return false;
    out = v;
    return true;
}

bool SensorController::write(const std::string& cmd) {
    if (!h_) return false;
    std::string c = cmd + "\r";
    int rc = Sensor3D_WriteData(h_, c.c_str());
    if (rc != SENSOR3D_OK) {
        log(fmtStr("Błąd komendy %s: %s (%d)", cmd.c_str(), errText(rc), rc));
        return false;
    }
    return true;
}

void SensorController::readInfo() {
    SensorInfo i;
    double v = 0;
    read("GetOrderNumber", i.order);
    read("GetSerialNumber", i.serial);
    read("GetFirmwareVersion", i.firmware);
    read("GetProducer", i.producer);
    if (readNum("GetPixelXMax", v)) i.pxW = (int)v;
    if (readNum("GetPixelYMax", v)) i.pxH = (int)v;
    readNum("GetMeasuringRangeX", i.rangeX);
    readNum("GetMeasuringRangeY", i.rangeY);
    readNum("GetMeasuringRangeZ", i.rangeZ);
    readNum("GetZstart", i.zStart);
    readNum("GetZend", i.zEnd);
    std::lock_guard<std::mutex> lk(m_);
    st_.info = i;
}

void SensorController::readParams() {
    const auto& defs = paramDefs();
    double zStart, zEnd;
    {
        std::lock_guard<std::mutex> lk(m_);
        zStart = st_.info.zStart;
        zEnd = st_.info.zEnd;
    }
    std::vector<ParamValue> vals(defs.size());
    for (size_t k = 0; k < defs.size(); k++) {
        const ParamDef& d = defs[k];
        ParamValue pv;
        std::string base = std::string("Get") + d.name;
        pv.valid = readNum(base, pv.value);
        if (d.sensorRange) {
            readNum(base + "Min", pv.minV);
            readNum(base + "Max", pv.maxV);
        } else if (d.kind == ParamKind::Float) {
            pv.minV = zStart;
            pv.maxV = zEnd;
        } else if (d.kind == ParamKind::Bool) {
            pv.minV = 0;
            pv.maxV = 1;
        } else {
            pv.minV = 16;
            pv.maxV = 28;
        }
        vals[k] = pv;
    }
    std::lock_guard<std::mutex> lk(m_);
    st_.params = vals;
    st_.paramsGen++;
}

void SensorController::doDisconnect(const char* reason) {
    if (h_) {
        Sensor3D_WriteData(h_, "SetAcquisitionStop\r");
        Sensor3D_Disconnect(h_);
        h_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(m_);
        st_.conn = ConnState::Disconnected;
        st_.tempValid = false;
        st_.statusBits = 0;
    }
    log(reason);
}

void SensorController::poll() {
    int bits = 0;
    double t = 0;
    bool ok = Sensor3D_GetSensorStatus(h_, &bits) == SENSOR3D_OK && (bits & SENSOR_CONNECTED);
    ok = ok && readNum("GetOperatingTemperature", t);
    if (!ok) {
        if (++pollFails_ >= 2) {
            pollFails_ = 0;
            doDisconnect("Utracono połączenie z czujnikiem.");
        }
        return;
    }
    pollFails_ = 0;
    bool wasHot;
    {
        std::lock_guard<std::mutex> lk(m_);
        wasHot = (st_.statusBits & SENSOR_OVERHEATED) != 0;
        st_.statusBits = bits;
        st_.temperature = t;
        st_.tempValid = true;
    }
    if ((bits & SENSOR_OVERHEATED) && !wasHot) log("UWAGA: czujnik zgłasza przegrzanie!");
}

void SensorController::connect(const std::string& ip) {
    {
        std::lock_guard<std::mutex> lk(m_);
        if (st_.conn != ConnState::Disconnected) return;
        st_.conn = ConnState::Connecting;
    }
    post("Łączenie...", [this, ip] {
        log("Łączenie z " + ip + " ...");
        h_ = Sensor3D_Connect(ip.c_str(), kConnectTimeoutMs);
        int bits = 0;
        if (h_ && (Sensor3D_GetSensorStatus(h_, &bits) != SENSOR3D_OK || !(bits & SENSOR_CONNECTED))) {
            Sensor3D_Disconnect(h_);
            h_ = nullptr;
        }
        if (!h_) {
            {
                std::lock_guard<std::mutex> lk(m_);
                st_.conn = ConnState::Disconnected;
            }
            log("Nie udało się połączyć z " + ip + ". Sprawdź kabel, zasilanie i adres IP karty sieciowej.");
            return;
        }
        if (bits & SENSOR_ACQUISITION_STARTED) write("SetAcquisitionStop");
        readInfo();
        readParams();
        pollFails_ = 0;
        poll();
        SensorInfo i;
        {
            std::lock_guard<std::mutex> lk(m_);
            st_.conn = ConnState::Connected;
            i = st_.info;
        }
        char ver[64] = "";
        Sensor3D_GetVersion(ver, sizeof(ver));
        log(fmtStr("Połączono: %s, S/N %s, firmware %s, SDK %s", i.order.c_str(), i.serial.c_str(),
                   i.firmware.c_str(), ver));
    });
}

void SensorController::disconnect() {
    post("Rozłączanie...", [this] {
        if (h_) doDisconnect("Rozłączono.");
    });
}

void SensorController::refreshParams() {
    post("Odczyt parametrów...", [this] {
        if (!h_) return;
        readParams();
        log("Odczytano parametry z czujnika.");
    });
}

void SensorController::applyParams(const std::vector<double>& values, const std::vector<bool>& changed) {
    post("Zapis parametrów...", [this, values, changed] {
        if (!h_) return;
        const auto& defs = paramDefs();
        int sent = 0, failed = 0;
        std::vector<size_t> order;
        size_t iMin = defs.size(), iMax = defs.size();
        for (size_t k = 0; k < defs.size() && k < values.size(); k++) {
            order.push_back(k);
            if (std::string(defs[k].name) == "Zmin") iMin = k;
            if (std::string(defs[k].name) == "Zmax") iMax = k;
        }
        // Czujnik wymaga Zmin < Zmax w każdej chwili: gdy okno przesuwa się w górę zakresu,
        // najpierw trzeba podnieść Zmax.
        if (iMin < order.size() && iMax < order.size() && changed[iMin] && changed[iMax]) {
            double curMax;
            {
                std::lock_guard<std::mutex> lk(m_);
                curMax = st_.params[iMax].value;
            }
            if (values[iMin] >= curMax) std::swap(order[iMin], order[iMax]);
        }
        for (size_t k : order) {
            if (!changed[k]) continue;
            std::string cmd = std::string("Set") + defs[k].name + "=" + fmtStr(defs[k].fmt, values[k]);
            if (write(cmd)) {
                sent++;
                log("Ustawiono " + cmd.substr(3));
            } else {
                failed++;
            }
        }
        readParams();
        if (failed) log(fmtStr("Parametry: %d ustawiono, %d odrzucono.", sent, failed));
    });
}

static uint32_t rgba(double r, double g, double b) {
    auto c = [](double v) { return (uint32_t)(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
    return c(r) | (c(g) << 8) | (c(b) << 16) | 0xFF000000u;
}

static uint32_t jet(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return rgba(1.5 - std::fabs(4 * t - 3), 1.5 - std::fabs(4 * t - 2), 1.5 - std::fabs(4 * t - 1));
}

void SensorController::buildPreview(const PointCloud& pc) {
    Preview pv;
    if (pc.kept > 0) {
        const size_t maxPts = 200000;
        size_t stride = (pc.kept + maxPts - 1) / maxPts, seen = 0;
        double z0 = pc.minV[2], zr = std::max(1e-9, pc.maxV[2] - pc.minV[2]);
        pv.pts.reserve(pc.kept / stride + 1);
        for (int i = 0; i < pc.numPoints; i++) {
            if (!pc.keep[i]) continue;
            if (seen++ % stride) continue;
            const Point3& p = pc.points[i];
            pv.pts.push_back({(float)p.x, (float)p.y, (float)p.z, jet((p.z - z0) / zr)});
        }
        pv.cx = (float)((pc.minV[0] + pc.maxV[0]) * 0.5);
        pv.cy = (float)((pc.minV[1] + pc.maxV[1]) * 0.5);
        pv.cz = (float)((pc.minV[2] + pc.maxV[2]) * 0.5);
        double dx = pc.maxV[0] - pc.minV[0], dy = pc.maxV[1] - pc.minV[1], dz = pc.maxV[2] - pc.minV[2];
        pv.radius = (float)std::max(1.0, 0.5 * std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    bool organized = pc.width > 0 && pc.height > 0 && (size_t)pc.width * pc.height == (size_t)pc.numPoints;
    if (organized && pc.kept > 0) {
        int step = std::max(1, (int)((pc.width + 899) / 900));
        pv.w = pc.width / step;
        pv.h = pc.height / step;
        pv.depth.assign((size_t)pv.w * pv.h, 0xFF202020u);
        pv.intensity.assign((size_t)pv.w * pv.h, 0xFF000000u);
        uint16_t imax = 1;
        for (int y = 0; y < pv.h; y++)
            for (int x = 0; x < pv.w; x++)
                imax = std::max(imax, pc.intensity[(size_t)(y * step) * pc.width + x * step]);
        double z0 = pc.minV[2], zr = std::max(1e-9, pc.maxV[2] - pc.minV[2]);
        for (int y = 0; y < pv.h; y++) {
            for (int x = 0; x < pv.w; x++) {
                size_t i = (size_t)(y * step) * pc.width + x * step;
                double g = (double)pc.intensity[i] / imax;
                pv.intensity[(size_t)y * pv.w + x] = rgba(g, g, g);
                if (pc.keep[i]) pv.depth[(size_t)y * pv.w + x] = jet((pc.points[i].z - z0) / zr);
            }
        }
    }
    std::lock_guard<std::mutex> lk(m_);
    preview_ = std::move(pv);
    previewGen_++;
}

void SensorController::scan() {
    post("Skanowanie...", [this] {
        if (!h_) return;
        int w, h;
        {
            std::lock_guard<std::mutex> lk(m_);
            w = st_.info.pxW;
            h = st_.info.pxH;
        }
        if (w <= 0 || h <= 0) {
            log("Brak rozdzielczości czujnika - skan przerwany.");
            return;
        }
        size_t n = (size_t)w * h;
        auto pc = std::make_shared<PointCloud>();
        pc->points.resize(n);
        pc->intensity.resize(n);

        int bits = 0;
        if (Sensor3D_GetSensorStatus(h_, &bits) == SENSOR3D_OK && (bits & SENSOR_ACQUISITION_STARTED))
            write("SetAcquisitionStop");
        if (!write("SetSensorMode=4") || !write("SetTriggerSource=1")) return;
        if (!write("SetAcquisitionStart")) return;

        POINT_CLOUD buf;
        buf.point = reinterpret_cast<POINT3D*>(pc->points.data());
        buf.intensity = pc->intensity.data();
        buf.confidence = nullptr;
        int bufSize = (int)((sizeof(POINT3D) + sizeof(uint16_t)) * n);
        ROI roi{};
        int count = 0;

        auto t0 = Clock::now();
        Sleep(100);
        bool trig = write("SetTriggerSoftware");
        int rc = trig ? Sensor3D_GetPointCloud(h_, &buf, bufSize, &count, &roi, kScanTimeoutMs) : SENSOR3D_TIMEOUT;
        double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        write("SetAcquisitionStop");

        if (rc != SENSOR3D_OK) {
            log(fmtStr("Skan nieudany: %s (%d)", errText(rc), rc));
            return;
        }
        setBusyText("Przetwarzanie chmury...");
        pc->numPoints = std::clamp(count, 0, (int)n);
        pc->width = roi.width;
        pc->height = roi.height;
        pc->scanMs = ms;
        pc->timestamp = nowStr("%Y-%m-%d %H:%M:%S");
        {
            std::lock_guard<std::mutex> lk(m_);
            cloud_ = pc;
        }
        analyze(*pc);
        log(fmtStr("Skan OK: %d punktów (%ux%u), z pomiarem %zu (%.1f%%), w obrysie palety %zu, czas %.0f ms",
                   pc->numPoints, pc->width, pc->height, pc->valid,
                   pc->numPoints ? 100.0 * pc->valid / pc->numPoints : 0.0, pc->kept, ms));
        if (pc->numPoints > 0 && pc->valid < (size_t)pc->numPoints / 20)
            log("UWAGA: mniej niż 5% punktów ma pomiar. Sprawdź, czy obiekt jest w zakresie Z czujnika i czy projektor LED jest aktywny.");
    });
}

void SensorController::analyze(PointCloud& pc) {
    PalletCfg cfg;
    {
        std::lock_guard<std::mutex> lk(m_);
        cfg = pallet_;
    }
    const double hx = cfg.length * 0.5 + cfg.margin, hy = cfg.width * 0.5 + cfg.margin;
    pc.keep.assign(pc.points.size(), 0);
    pc.valid = pc.kept = 0;
    for (int a = 0; a < 3; a++) pc.minV[a] = pc.maxV[a] = 0;
    for (int i = 0; i < pc.numPoints; i++) {
        if (!pc.isValid(i)) continue;
        pc.valid++;
        const Point3& p = pc.points[i];
        if (cfg.crop && (std::fabs(p.x) > hx || std::fabs(p.y) > hy)) continue;
        const double v[3] = {p.x, p.y, p.z};
        for (int a = 0; a < 3; a++) {
            if (pc.kept == 0 || v[a] < pc.minV[a]) pc.minV[a] = v[a];
            if (pc.kept == 0 || v[a] > pc.maxV[a]) pc.maxV[a] = v[a];
        }
        pc.keep[i] = 1;
        pc.kept++;
    }
    buildPreview(pc);
    std::lock_guard<std::mutex> lk(m_);
    st_.hasCloud = true;
    st_.cloudGen++;
    st_.cloudPoints = pc.numPoints;
    st_.cloudValid = pc.valid;
    st_.cloudKept = pc.kept;
    st_.cloudW = pc.width;
    st_.cloudH = pc.height;
    for (int a = 0; a < 3; a++) {
        st_.cloudMin[a] = pc.minV[a];
        st_.cloudMax[a] = pc.maxV[a];
    }
    st_.scanMs = pc.scanMs;
    st_.cloudTime = pc.timestamp;
}

void SensorController::setPallet(const PalletCfg& cfg) {
    std::shared_ptr<PointCloud> pc;
    {
        std::lock_guard<std::mutex> lk(m_);
        pallet_ = cfg;
        pc = cloud_;
    }
    if (!pc) return;
    post("Przeliczanie chmury...", [this] {
        std::shared_ptr<PointCloud> c;
        {
            std::lock_guard<std::mutex> lk(m_);
            c = cloud_;
        }
        if (c) analyze(*c);
    });
}

void SensorController::saveTxt(const std::wstring& path, bool withIntensity, int step) {
    post("Zapis pliku...", [this, path, withIntensity, step] {
        std::shared_ptr<PointCloud> pc;
        {
            std::lock_guard<std::mutex> lk(m_);
            pc = cloud_;
        }
        if (!pc || pc->kept == 0) {
            log("Brak punktów do zapisania.");
            return;
        }
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) {
            log("Nie można utworzyć pliku: " + utf8(path));
            return;
        }
        std::vector<char> buf(4 << 20);
        size_t pos = 0, written = 0;
        bool ok = true;
        int st = std::max(1, step);
        for (int i = 0; i < pc->numPoints && ok; i += st) {
            if (!pc->keep[i]) continue;
            const Point3& p = pc->points[i];
            if (withIntensity)
                pos += snprintf(buf.data() + pos, 160, "%.3f %.3f %.3f %u\n", p.x, p.y, p.z, (unsigned)pc->intensity[i]);
            else
                pos += snprintf(buf.data() + pos, 160, "%.3f %.3f %.3f\n", p.x, p.y, p.z);
            written++;
            if (pos > buf.size() - 256) {
                ok = fwrite(buf.data(), 1, pos, f) == pos;
                pos = 0;
                setBusyText(fmtStr("Zapis pliku... %d%%", (int)(100.0 * i / pc->numPoints)));
            }
        }
        if (ok && pos) ok = fwrite(buf.data(), 1, pos, f) == pos;
        ok = (fclose(f) == 0) && ok;
        if (ok)
            log(fmtStr("Zapisano %zu punktów do %s", written, utf8(path).c_str()));
        else
            log("Błąd zapisu pliku (brak miejsca na dysku?): " + utf8(path));
    });
}

Snapshot SensorController::snapshot() {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

bool SensorController::fetchPreview(uint64_t& seenGen, Preview& out) {
    std::lock_guard<std::mutex> lk(m_);
    if (seenGen == previewGen_) return false;
    seenGen = previewGen_;
    out = preview_;
    return true;
}

bool SensorController::idle() {
    std::lock_guard<std::mutex> lk(m_);
    return pending_ == 0;
}

// Obsługa czujnika wenglor ShapeDrive G4 (MLBS115) przez Sensor3d.dll.
// Wszystkie wywołania SDK idą z jednego wątku roboczego, GUI tylko zleca zadania
// i odczytuje migawkę stanu.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class ParamKind { Int, Float, Bool, Pattern };

struct ParamDef {
    const char* name;      // nazwa komendy bez przedrostka Get/Set
    const char* label;     // opis w GUI
    ParamKind kind;
    bool sensorRange;      // zakres z Get<name>Min / Get<name>Max
    const char* fmt;       // format wartości wysyłanej do czujnika
};

const std::vector<ParamDef>& paramDefs();

struct ParamValue {
    double value = 0, minV = 0, maxV = 0;
    bool valid = false;
};

struct SensorInfo {
    std::string order, serial, firmware, producer;
    int pxW = 0, pxH = 0;
    double rangeX = 0, rangeY = 0, rangeZ = 0, zStart = 0, zEnd = 0;
};

struct Point3 { double x, y, z; };

// Skanowany obiekt: paleta leżąca na przenośniku pod czujnikiem.
// Długość wzdłuż osi X czujnika, szerokość wzdłuż Y, wysokość wzdłuż Z (w stronę czujnika).
struct PalletCfg {
    double length = 1200, width = 800, height = 150;  // mm
    double baseDist = 2050;   // odległość czujnik - powierzchnia rolek [mm] (zmierzone skanem 2026-11-17)
    double margin = 50;       // zapas wokół palety [mm]
    bool crop = true;         // odrzucaj punkty poza obrysem palety (X/Y)
};

struct ViewPoint { float x, y, z; uint32_t color; };

struct PointCloud {
    uint32_t width = 0, height = 0;     // ROI zwrócone przez czujnik
    int numPoints = 0;
    std::vector<Point3> points;
    std::vector<uint16_t> intensity;
    std::vector<uint8_t> keep;          // 1 = punkt poprawny i w obrysie palety
    size_t valid = 0;                   // punkty z pomiarem
    size_t kept = 0;                    // punkty po przycięciu do palety
    double minV[3] = {0, 0, 0}, maxV[3] = {0, 0, 0};
    double scanMs = 0;
    std::string timestamp;

    bool isValid(size_t i) const;
};

enum class ConnState { Disconnected, Connecting, Connected };

struct Snapshot {
    ConnState conn = ConnState::Disconnected;
    bool busy = false;
    std::string busyText;
    bool tempValid = false;
    double temperature = 0;
    int statusBits = 0;
    SensorInfo info;
    std::vector<ParamValue> params;
    uint64_t paramsGen = 0;
    // ostatni skan
    bool hasCloud = false;
    uint64_t cloudGen = 0;
    int cloudPoints = 0;
    size_t cloudValid = 0;
    size_t cloudKept = 0;
    uint32_t cloudW = 0, cloudH = 0;
    double cloudMin[3] = {0, 0, 0}, cloudMax[3] = {0, 0, 0};
    double scanMs = 0;
    std::string cloudTime;
    std::vector<std::string> log;
};

struct Preview {
    int w = 0, h = 0;
    std::vector<uint32_t> depth;      // RGBA, mapa głębi Z
    std::vector<uint32_t> intensity;  // RGBA, obraz intensywności
    std::vector<ViewPoint> pts;       // przerzedzona chmura do widoku 3D
    float cx = 0, cy = 0, cz = 0;     // środek chmury
    float radius = 1;                 // promień obejmujący chmurę
};

class SensorController {
public:
    SensorController();
    ~SensorController();

    void connect(const std::string& ip);
    void disconnect();
    void scan();
    void refreshParams();
    // values: pełna lista w kolejności paramDefs(); changed: które wysłać
    void applyParams(const std::vector<double>& values, const std::vector<bool>& changed);
    void saveTxt(const std::wstring& path, bool withIntensity, int step);
    // Ustawia wymiary palety; istniejąca chmura jest przeliczana na nowo.
    void setPallet(const PalletCfg& cfg);

    Snapshot snapshot();
    // Zwraca true i wypełnia out, gdy jest podgląd nowszy niż seenGen.
    bool fetchPreview(uint64_t& seenGen, Preview& out);
    bool idle();

private:
    void post(const std::string& text, std::function<void()> fn);
    void workerLoop();
    void poll();
    void log(const std::string& msg);
    void setBusyText(const std::string& t);

    bool read(const char* cmd, std::string& out);
    bool readNum(const std::string& cmd, double& out);
    bool write(const std::string& cmd);
    void readInfo();
    void readParams();
    void doDisconnect(const char* reason);
    void buildPreview(const PointCloud& pc);
    void analyze(PointCloud& pc);      // przycięcie do palety, statystyki, podgląd, publikacja

    PalletCfg pallet_;
    void* h_ = nullptr;                // uchwyt SDK, tylko wątek roboczy
    std::vector<char> rbuf_;
    int pollFails_ = 0;

    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::string, std::function<void()>>> jobs_;
    bool quit_ = false;
    int pending_ = 0;
    Snapshot st_;
    std::shared_ptr<PointCloud> cloud_;
    Preview preview_;
    uint64_t previewGen_ = 0;
    std::thread worker_;
};

std::string utf8(const std::wstring& w);

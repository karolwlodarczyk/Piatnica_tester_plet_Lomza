// Skaner wenglor MLBS115 - GUI (Dear ImGui + DirectX 11).
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include "Sensor.h"

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static bool g_occluded = false;
static UINT g_resizeW = 0, g_resizeH = 0;

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static void createRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void cleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool createDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &level, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                           &sd, &g_swap, &g_dev, &level, &g_ctx);
    if (hr != S_OK) return false;
    createRenderTarget();
    return true;
}

static void cleanupDevice() {
    cleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
}

static LRESULT WINAPI wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeW = (UINT)LOWORD(lParam);
            g_resizeH = (UINT)HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// Tekstura podglądu
struct Texture {
    ID3D11ShaderResourceView* srv = nullptr;
    int w = 0, h = 0;
    void release() {
        if (srv) { srv->Release(); srv = nullptr; }
        w = h = 0;
    }
    void upload(const std::vector<uint32_t>& px, int width, int height) {
        release();
        if (px.empty() || width <= 0 || height <= 0) return;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = height;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sub{};
        sub.pSysMem = px.data();
        sub.SysMemPitch = width * 4;
        ID3D11Texture2D* tex = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&d, &sub, &tex)) || !tex) return;
        g_dev->CreateShaderResourceView(tex, nullptr, &srv);
        tex->Release();
        w = width;
        h = height;
    }
};

static std::wstring askSavePath(HWND owner) {
    wchar_t file[MAX_PATH];
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    wcsftime(file, MAX_PATH, L"chmura_%Y%m%d_%H%M%S.txt", &tm);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Chmura punktów TXT (*.txt)\0*.txt\0Wszystkie pliki (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.lpstrTitle = L"Zapisz chmurę punktów";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return {};
    return file;
}

// Tryb bez okna: połącz, skanuj, zapisz, wypisz log do pliku. Używany do testów.
static int runSelfTest(const std::string& ip, const std::wstring& outPath, const std::wstring& logPath) {
    SensorController ctrl;
    auto waitIdle = [&] {
        Sleep(50);
        while (!ctrl.idle()) Sleep(50);
    };
    ctrl.connect(ip);
    waitIdle();
    int rc = 1;
    if (ctrl.snapshot().conn == ConnState::Connected) {
        ctrl.scan();
        waitIdle();
        if (ctrl.snapshot().hasCloud) {
            ctrl.saveTxt(outPath, true, 1);
            waitIdle();
            rc = 0;
        }
        ctrl.disconnect();
        waitIdle();
    }
    Snapshot s = ctrl.snapshot();
    if (FILE* f = _wfopen(logPath.c_str(), L"wb")) {
        for (auto& l : s.log) fprintf(f, "%s\n", l.c_str());
        fprintf(f, "temp=%.1f valid=%d\n", s.temperature, (int)s.tempValid);
        for (size_t k = 0; k < s.params.size(); k++)
            fprintf(f, "%s=%g [%g..%g]\n", paramDefs()[k].name, s.params[k].value, s.params[k].minV, s.params[k].maxV);
        if (s.hasCloud)
            fprintf(f, "kept %zu\n", s.cloudKept);
        if (s.hasCloud)
            fprintf(f, "cloud %d pts valid %zu roi %ux%u X[%.1f %.1f] Y[%.1f %.1f] Z[%.1f %.1f]\n", s.cloudPoints,
                    s.cloudValid, s.cloudW, s.cloudH, s.cloudMin[0], s.cloudMax[0], s.cloudMin[1], s.cloudMax[1],
                    s.cloudMin[2], s.cloudMax[2]);
        fclose(f);
    }
    return rc;
}

// Widok 3D chmury: rzut równoległy, obrót myszą, zoom kółkiem. Chmura jest zawsze
// wyśrodkowana w panelu (środek obrotu = środek chmury).
struct View3D {
    float yaw = 0.6f, pitch = 1.0f, zoom = 1.0f;

    void draw(const Preview& pv, const PalletCfg& pal, float scale) {
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 20 || avail.y < 20) return;
        ImGui::InvisibleButton("view3d", avail, ImGuiButtonFlags_MouseButtonLeft);
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            yaw += io.MouseDelta.x * 0.01f;
            pitch = std::clamp(pitch - io.MouseDelta.y * 0.01f, 0.0f, 1.5708f);
        }
        if (ImGui::IsItemHovered()) {
            if (io.MouseWheel != 0) zoom = std::clamp(zoom * (1.0f + 0.12f * io.MouseWheel), 0.2f, 40.0f);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) { yaw = 0.6f; pitch = 1.0f; zoom = 1.0f; }
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p1(p0.x + avail.x, p0.y + avail.y);
        dl->PushClipRect(p0, p1, true);
        dl->AddRectFilled(p0, p1, IM_COL32(14, 14, 16, 255));

        const float cxs = p0.x + avail.x * 0.5f, cys = p0.y + avail.y * 0.5f;
        const float k = std::min(avail.x, avail.y) * 0.46f / pv.radius * zoom;
        const float cyw = std::cos(yaw), syw = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
        // Oś Z czujnika jest skierowana w dół, więc w widoku odwracamy ją: wyższe punkty są wyżej.
        auto proj = [&](float x, float y, float z) {
            float dx = x - pv.cx, dy = y - pv.cy, dz = -(z - pv.cz);
            float x1 = dx * cyw - dy * syw, y1 = dx * syw + dy * cyw;
            return ImVec2(cxs + x1 * k, cys + (y1 * cp - dz * sp) * k);
        };

        const float ps = std::max(1.0f, std::round(1.5f * scale));
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        const size_t chunk = 8000;
        for (size_t i = 0; i < pv.pts.size(); i += chunk) {
            size_t n = std::min(chunk, pv.pts.size() - i);
            dl->PrimReserve((int)n * 6, (int)n * 4);
            for (size_t j = i; j < i + n; j++) {
                const ViewPoint& q = pv.pts[j];
                ImVec2 a = proj(q.x, q.y, q.z);
                dl->PrimRectUV(a, ImVec2(a.x + ps, a.y + ps), uv, uv, q.color);
            }
        }

        // Obrys palety (prostopadłościan o zadanych wymiarach) dla odniesienia.
        const float hx = (float)pal.length * 0.5f, hy = (float)pal.width * 0.5f;
        const float zb = (float)pal.baseDist, zt = (float)(pal.baseDist - pal.height);
        const ImVec2 c[8] = {proj(-hx, -hy, zb), proj(hx, -hy, zb), proj(hx, hy, zb), proj(-hx, hy, zb),
                             proj(-hx, -hy, zt), proj(hx, -hy, zt), proj(hx, hy, zt), proj(-hx, hy, zt)};
        const ImU32 col = IM_COL32(255, 255, 255, 110);
        for (int e = 0; e < 4; e++) {
            dl->AddLine(c[e], c[(e + 1) % 4], col);
            dl->AddLine(c[4 + e], c[4 + (e + 1) % 4], col);
            dl->AddLine(c[e], c[4 + e], col);
        }
        dl->AddText(ImVec2(p0.x + 8 * scale, p1.y - ImGui::GetTextLineHeight() - 6 * scale), IM_COL32(170, 170, 170, 255),
                    "Przeciągnij: obrót   Kółko: zoom   Dwuklik: reset   Biała ramka: obrys palety");
        dl->PopClipRect();
    }
};

static int paramIndex(const char* name) {
    const auto& defs = paramDefs();
    for (size_t k = 0; k < defs.size(); k++)
        if (std::string(defs[k].name) == name) return (int)k;
    return -1;
}

static void statusDot(ImU32 color, float scale) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float r = 6.0f * scale, hgt = ImGui::GetTextLineHeight();
    ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + hgt * 0.5f), r, color);
    ImGui::Dummy(ImVec2(r * 2, hgt));
    ImGui::SameLine();
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    if (__argc >= 5 && std::wstring(__wargv[1]) == L"--selftest")
        return runSelfTest(utf8(__wargv[2]), __wargv[3], __wargv[4]);

    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc{sizeof(wc), CS_CLASSDC, wndProc, 0, 0, hInst, nullptr, LoadCursor(nullptr, IDC_ARROW),
                   nullptr, nullptr, L"WenglorScanner", nullptr};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Skaner wenglor MLBS115", WS_OVERLAPPEDWINDOW, 100, 60, 1500, 950,
                              nullptr, nullptr, hInst, nullptr);
    if (!createDevice(hwnd)) {
        cleanupDevice();
        MessageBoxW(hwnd, L"Nie udało się zainicjować DirectX 11.", L"Błąd", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    float scale = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui::GetStyle().FrameRounding = 3.0f * scale;
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0};
    const char* fontPath = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(fontPath) != INVALID_FILE_ATTRIBUTES)
        io.Fonts->AddFontFromFileTTF(fontPath, 18.0f * scale, nullptr, ranges);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    {
        SensorController ctrl;
        char ip[64] = "192.168.100.1";
        std::vector<double> edit(paramDefs().size(), 0.0);
        uint64_t seenParams = ~0ull, seenPreview = 0;
        Preview preview;
        Texture texDepth, texInt;
        int previewMode = 0;
        bool saveIntensity = true;
        int saveStep = 1;
        size_t lastLogSize = 0;
        PalletCfg pallet;
        View3D view3d;
        ctrl.setPallet(pallet);
        // "--autoscan [ip]": od razu po starcie połącz się i wykonaj jeden skan.
        if (__argc >= 2 && std::wstring(__wargv[1]) == L"--autoscan") {
            if (__argc >= 3) snprintf(ip, sizeof(ip), "%s", utf8(__wargv[2]).c_str());
            ctrl.connect(ip);
            ctrl.scan();
        }

        bool done = false;
        while (!done) {
            MSG msg;
            while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT) done = true;
            }
            if (done) break;
            if (g_occluded && g_swap->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
                Sleep(10);
                continue;
            }
            g_occluded = false;
            if (g_resizeW != 0 && g_resizeH != 0) {
                cleanupRenderTarget();
                g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
                g_resizeW = g_resizeH = 0;
                createRenderTarget();
            }

            Snapshot s = ctrl.snapshot();
            if (s.paramsGen != seenParams) {
                seenParams = s.paramsGen;
                for (size_t k = 0; k < edit.size(); k++) edit[k] = s.params[k].value;
            }
            if (ctrl.fetchPreview(seenPreview, preview)) {
                texDepth.upload(preview.depth, preview.w, preview.h);
                texInt.upload(preview.intensity, preview.w, preview.h);
            }
            const bool connected = s.conn == ConnState::Connected;
            const bool canAct = connected && !s.busy;

            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();

            const ImGuiViewport* vp = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(vp->WorkPos);
            ImGui::SetNextWindowSize(vp->WorkSize);
            ImGui::Begin("main", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus);

            // ---------------- lewy panel ----------------
            ImGui::BeginChild("left", ImVec2(470 * scale, 0), ImGuiChildFlags_Borders);
            ImGui::PushItemWidth(170 * scale);

            ImGui::SeparatorText("Połączenie");
            ImGui::BeginDisabled(s.conn != ConnState::Disconnected);
            ImGui::InputText("Adres IP", ip, sizeof(ip));
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (s.conn == ConnState::Disconnected) {
                if (ImGui::Button("Połącz", ImVec2(110 * scale, 0))) ctrl.connect(ip);
            } else {
                ImGui::BeginDisabled(s.busy);
                if (ImGui::Button("Rozłącz", ImVec2(110 * scale, 0))) ctrl.disconnect();
                ImGui::EndDisabled();
            }
            if (connected) {
                statusDot(IM_COL32(60, 200, 90, 255), scale);
                ImGui::Text("Połączono");
            } else if (s.conn == ConnState::Connecting) {
                statusDot(IM_COL32(230, 190, 50, 255), scale);
                ImGui::Text("Łączenie...");
            } else {
                statusDot(IM_COL32(220, 70, 70, 255), scale);
                ImGui::Text("Rozłączono");
            }
            const bool hot = (s.statusBits & 0x8) != 0;
            if (connected && s.tempValid) {
                ImGui::SameLine(200 * scale);
                if (hot)
                    ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Temperatura: %.1f °C  PRZEGRZANIE", s.temperature);
                else
                    ImGui::Text("Temperatura: %.1f °C", s.temperature);
            }

            ImGui::SeparatorText("Urządzenie");
            if (connected) {
                ImGui::Text("Model: %s    S/N: %s    FW: %s", s.info.order.c_str(), s.info.serial.c_str(),
                            s.info.firmware.c_str());
                ImGui::Text("Matryca: %d x %d px", s.info.pxW, s.info.pxH);
                ImGui::Text("Zakres: X %.0f  Y %.0f  Z %.0f-%.0f mm", s.info.rangeX, s.info.rangeY, s.info.zStart,
                            s.info.zEnd);
            } else {
                ImGui::TextDisabled("Brak połączenia z czujnikiem.");
            }

            ImGui::SeparatorText("Skanowanie");
            ImGui::BeginDisabled(!canAct);
            if (ImGui::Button("SKANUJ", ImVec2(-1, 48 * scale))) ctrl.scan();
            ImGui::EndDisabled();
            if (s.busy)
                ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.3f, 1.0f), "%s", s.busyText.c_str());
            else
                ImGui::TextDisabled("Gotowy");
            if (s.hasCloud) {
                ImGui::Text("Ostatni skan: %s  (%.0f ms)", s.cloudTime.c_str(), s.scanMs);
                ImGui::Text("Punkty: %d (%u x %u)", s.cloudPoints, s.cloudW, s.cloudH);
                ImGui::Text("Z pomiarem: %zu (%.1f%%),  w obrysie palety: %zu", s.cloudValid,
                            s.cloudPoints ? 100.0 * s.cloudValid / s.cloudPoints : 0.0, s.cloudKept);
                if (s.cloudKept > 0) {
                    ImGui::Text("X: %.1f ... %.1f mm", s.cloudMin[0], s.cloudMax[0]);
                    ImGui::Text("Y: %.1f ... %.1f mm", s.cloudMin[1], s.cloudMax[1]);
                    ImGui::Text("Z: %.1f ... %.1f mm", s.cloudMin[2], s.cloudMax[2]);
                }
            }

            ImGui::SeparatorText("Paleta");
            {
                bool edited = false;
                auto field = [&](const char* label, double* v, double lo, double hi) {
                    ImGui::InputDouble(label, v, 10.0, 100.0, "%.0f");
                    *v = std::clamp(*v, lo, hi);
                    edited |= ImGui::IsItemDeactivatedAfterEdit();
                };
                field("Długość (oś X) [mm]", &pallet.length, 10.0, 5000.0);
                field("Szerokość (oś Y) [mm]", &pallet.width, 10.0, 5000.0);
                field("Wysokość max [mm]", &pallet.height, 1.0, 2000.0);
                field("Odległość czujnik-podłoże [mm]", &pallet.baseDist, 100.0, 10000.0);
                field("Zapas wokół palety [mm]", &pallet.margin, 0.0, 1000.0);
                edited |= ImGui::Checkbox("Przytnij chmurę do obrysu palety (X/Y)", &pallet.crop);
                if (edited) ctrl.setPallet(pallet);

                const double zTop = pallet.baseDist - pallet.height;
                if (connected && (zTop < s.info.zStart || pallet.baseDist > s.info.zEnd))
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f),
                                       "Paleta (Z %.0f-%.0f) wychodzi poza zakres\nczujnika %.0f-%.0f mm!", zTop,
                                       pallet.baseDist, s.info.zStart, s.info.zEnd);
                if (connected && (pallet.length > s.info.rangeX || pallet.width > s.info.rangeY))
                    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f), "Paleta większa niż pole widzenia %.0f x %.0f mm!",
                                       s.info.rangeX, s.info.rangeY);
                ImGui::BeginDisabled(!canAct);
                if (ImGui::Button("Ustaw okno Z czujnika wg palety", ImVec2(-1, 0))) {
                    int iMin = paramIndex("Zmin"), iMax = paramIndex("Zmax");
                    if (iMin >= 0 && iMax >= 0) {
                        std::vector<bool> ch(edit.size(), false);
                        edit[iMin] = std::clamp(zTop - pallet.margin, s.info.zStart, s.info.zEnd);
                        edit[iMax] = std::clamp(pallet.baseDist + pallet.margin, s.info.zStart, s.info.zEnd);
                        ch[iMin] = ch[iMax] = true;
                        ctrl.applyParams(edit, ch);
                    }
                }
                ImGui::EndDisabled();
            }

            ImGui::SeparatorText("Parametry");
            ImGui::BeginDisabled(!canAct);
            const auto& defs = paramDefs();
            std::vector<bool> changed(defs.size(), false);
            bool anyChanged = false;
            for (size_t k = 0; k < defs.size(); k++) {
                const ParamDef& d = defs[k];
                const ParamValue& pv = s.params[k];
                ImGui::PushID((int)k);
                ImGui::BeginDisabled(!pv.valid);
                switch (d.kind) {
                    case ParamKind::Int: {
                        int v = (int)std::lround(edit[k]);
                        if (pv.maxV - pv.minV <= 1000)
                            ImGui::SliderInt(d.label, &v, (int)pv.minV, (int)pv.maxV);
                        else
                            ImGui::InputInt(d.label, &v, 1000, 10000);
                        if (pv.maxV > pv.minV) v = std::clamp(v, (int)pv.minV, (int)pv.maxV);
                        edit[k] = v;
                        break;
                    }
                    case ParamKind::Float: {
                        if (pv.maxV - pv.minV <= 1.0) {
                            float v = (float)edit[k];
                            ImGui::SliderFloat(d.label, &v, (float)pv.minV, (float)pv.maxV, "%.2f");
                            edit[k] = std::round(v * 100.0) / 100.0;
                        } else {
                            double v = edit[k];
                            ImGui::InputDouble(d.label, &v, 10.0, 100.0, "%.1f");
                            if (pv.maxV > pv.minV) v = std::clamp(v, pv.minV, pv.maxV);
                            edit[k] = v;
                        }
                        break;
                    }
                    case ParamKind::Bool: {
                        bool v = edit[k] != 0.0;
                        ImGui::Checkbox(d.label, &v);
                        edit[k] = v ? 1.0 : 0.0;
                        break;
                    }
                    case ParamKind::Pattern: {
                        int v = edit[k] < 22 ? 16 : 28;
                        ImGui::TextUnformatted(d.label);
                        ImGui::SameLine();
                        ImGui::RadioButton("16 (szybciej)", &v, 16);
                        ImGui::SameLine();
                        ImGui::RadioButton("28 (dokładniej)", &v, 28);
                        edit[k] = v;
                        break;
                    }
                }
                ImGui::EndDisabled();
                changed[k] = pv.valid && std::fabs(edit[k] - pv.value) > 1e-6;
                if (changed[k]) {
                    anyChanged = true;
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.3f, 1.0f), "*");
                }
                if (d.sensorRange && pv.valid && d.kind == ParamKind::Int && pv.maxV - pv.minV > 1000 &&
                    ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Zakres: %.0f - %.0f", pv.minV, pv.maxV);
                ImGui::PopID();
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(!anyChanged);
            if (ImGui::Button("Zastosuj zmiany", ImVec2(200 * scale, 0))) ctrl.applyParams(edit, changed);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Odczytaj z czujnika", ImVec2(200 * scale, 0))) ctrl.refreshParams();
            ImGui::EndDisabled();
            if (anyChanged) ImGui::TextDisabled("* zmienione, jeszcze niewysłane do czujnika");

            ImGui::SeparatorText("Zapis chmury");
            ImGui::Checkbox("Zapisuj intensywność (X Y Z I)", &saveIntensity);
            ImGui::InputInt("Co N-ty punkt", &saveStep);
            saveStep = std::clamp(saveStep, 1, 1000);
            ImGui::BeginDisabled(s.busy || !s.hasCloud || s.cloudKept == 0);
            if (ImGui::Button("Zapisz chmurę do TXT...", ImVec2(-1, 36 * scale))) {
                std::wstring path = askSavePath(hwnd);
                if (!path.empty()) ctrl.saveTxt(path, saveIntensity, saveStep);
            }
            ImGui::EndDisabled();

            ImGui::PopItemWidth();
            ImGui::EndChild();

            // ---------------- prawy panel ----------------
            ImGui::SameLine();
            ImGui::BeginChild("right", ImVec2(0, 0));
            float logH = 190 * scale;
            ImGui::BeginChild("preview", ImVec2(0, -logH), ImGuiChildFlags_Borders);
            ImGui::RadioButton("Chmura 3D", &previewMode, 0);
            ImGui::SameLine();
            ImGui::RadioButton("Mapa głębi Z", &previewMode, 1);
            ImGui::SameLine();
            ImGui::RadioButton("Intensywność", &previewMode, 2);
            Texture& tex = previewMode == 1 ? texDepth : texInt;
            if (previewMode == 0) {
                if (!preview.pts.empty()) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("   %zu pkt w widoku, niebieski = %.0f mm (bliżej czujnika), czerwony = %.0f mm",
                                        preview.pts.size(), s.cloudMin[2], s.cloudMax[2]);
                    view3d.draw(preview, pallet, scale);
                } else {
                    ImGui::TextDisabled(s.hasCloud ? "Brak punktów do wyświetlenia w obrysie palety."
                                                   : "Chmura pojawi się po pierwszym skanie.");
                }
            } else if (tex.srv) {
                if (previewMode == 1 && s.cloudKept > 0) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("   niebieski = %.0f mm (bliżej), czerwony = %.0f mm (dalej)", s.cloudMin[2],
                                        s.cloudMax[2]);
                }
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float k = std::min(avail.x / tex.w, avail.y / tex.h);
                if (k > 0) {
                    ImVec2 size(tex.w * k, tex.h * k);
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - size.x) * 0.5f);
                    ImGui::Image((ImTextureID)(intptr_t)tex.srv, size);
                }
            } else {
                ImGui::TextDisabled(s.hasCloud ? "Brak podglądu dla tego skanu (chmura pusta lub nieuporządkowana)."
                                               : "Podgląd pojawi się po pierwszym skanie.");
            }
            ImGui::EndChild();

            ImGui::BeginChild("log", ImVec2(0, 0), ImGuiChildFlags_Borders);
            for (const auto& l : s.log) ImGui::TextUnformatted(l.c_str());
            if (s.log.size() != lastLogSize) {
                ImGui::SetScrollHereY(1.0f);
                lastLogSize = s.log.size();
            }
            ImGui::EndChild();
            ImGui::EndChild();

            ImGui::End();

            ImGui::Render();
            const float clear[4] = {0.08f, 0.08f, 0.09f, 1.0f};
            g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
            g_ctx->ClearRenderTargetView(g_rtv, clear);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            HRESULT hr = g_swap->Present(1, 0);
            g_occluded = (hr == DXGI_STATUS_OCCLUDED);
        }
        texDepth.release();
        texInt.release();
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    cleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

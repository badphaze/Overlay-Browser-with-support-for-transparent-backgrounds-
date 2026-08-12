#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <wrl.h>
#include <thread>
#include <atomic>
#include <string>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#include "WebView2.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")

using namespace Microsoft::WRL;

#define WM_OVERLAY_TOGGLE (WM_USER + 1)

HWND g_hwnd = NULL;
HWND g_hPreviousFocus = NULL;
ID3D11Device* g_pd3dDevice = NULL;
ID3D11DeviceContext* g_pd3dDeviceContext = NULL;
IDXGISwapChain* g_pSwapChain = NULL;
ID3D11RenderTargetView* g_mainRenderTargetView = NULL;

ComPtr<ICoreWebView2Controller> g_controller = nullptr;
ComPtr<ICoreWebView2> g_webview = nullptr;

bool g_isInteractive = false;
std::atomic<bool> g_running{ true };
char g_urlBuffer[1024] = "google.com";

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };

    if (D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
        return false;

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
    return true;
}

void CleanupDeviceD3D() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

// --- ОБНОВЛЕНИЕ ГРАНИЦ WEBVIEW2 ---
void UpdateWebViewBounds(HWND hwnd) {
    if (!g_controller) return;

    RECT rc;
    GetClientRect(hwnd, &rc);

    if (g_isInteractive) {
        RECT bounds = { 0, 38, rc.right, rc.bottom - 24 };
        g_controller->put_Bounds(bounds);
    }
    else {
        RECT bounds = { 0, 0, rc.right, rc.bottom };
        g_controller->put_Bounds(bounds);
    }
}

// --- СБРОС СОСТОЯНИЯ ВВОДА И ВОССТАНОВЛЕНИЕ ФОКУСА ---
void ResetImGuiInputState() {
    ImGui::ClearActiveID();
    ImGuiIO& io = ImGui::GetIO();
    io.ClearEventsQueue();
#if IMGUI_VERSION_NUM >= 18700
    io.AddMouseButtonEvent(0, false);
#else
    io.MouseDown[0] = false;
#endif
}

// --- БЕЗОПАСНЫЙ ЗАПУСК ПЕРЕМЕЩЕНИЯ ИЛИ РЕСАЙЗА ОКНА ---
void PerformWindowDragOrResize(HWND hwnd, WPARAM hitTestCode) {
    ImGui::ClearActiveID();
    ReleaseCapture();

    // Запуск системного модального цикла Windows
    SendMessage(hwnd, WM_NCLBUTTONDOWN, hitTestCode, 0);

    // ВОССТАНАВЛИВАЕМ КЛАВИАТУРНЫЙ ФОКУС WIN32, КОТОРЫЙ ТЕРЯЕТСЯ ПОСЛЕ WM_NCLBUTTONDOWN
    SetForegroundWindow(hwnd);
    SetFocus(hwnd);

    ResetImGuiInputState();
}

// --- ПЕРЕКЛЮЧЕНИЕ РЕЖИМА КЛИКАБЕЛЬНОСТИ ---
void ApplyInteractivityState(HWND hwnd, bool interactive) {
    g_isInteractive = interactive;
    LONG_PTR exStyle = GetWindowLongPtr(hwnd, GWL_EXSTYLE);

    if (g_isInteractive) {
        g_hPreviousFocus = GetForegroundWindow();
        exStyle &= ~(WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
        EnableWindow(hwnd, TRUE);

        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }
    else {
        exStyle |= (WS_EX_TRANSPARENT | WS_EX_NOACTIVATE);
        SetWindowLongPtr(hwnd, GWL_EXSTYLE, exStyle);
        EnableWindow(hwnd, FALSE);

        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        if (g_hPreviousFocus && IsWindow(g_hPreviousFocus)) {
            SetForegroundWindow(g_hPreviousFocus);
        }
    }

    ResetImGuiInputState();
    UpdateWebViewBounds(hwnd);
}

// --- ПЕРЕХОД ПО URL ---
void NavigateToUrl(const char* url) {
    std::string urlStr = url;
    if (urlStr.find("http") != 0 && urlStr.find("file") != 0 && !urlStr.empty()) {
        urlStr = "http://" + urlStr;
    }
    wchar_t wUrl[1024];
    MultiByteToWideChar(CP_UTF8, 0, urlStr.c_str(), -1, wUrl, 1024);
    if (g_webview) g_webview->Navigate(wUrl);
}

// --- ПОТОК СКАНА ХОТКЕЕВ ---
void HotkeyWorkerThread() {
    bool homePressed = false;
    while (g_running) {
        if (GetAsyncKeyState(VK_END) & 0x8000) ExitProcess(0);

        if (GetAsyncKeyState(VK_HOME) & 0x8000) {
            if (!homePressed) {
                homePressed = true;
                if (g_hwnd) PostMessage(g_hwnd, WM_OVERLAY_TOGGLE, 0, 0);
            }
        }
        else { homePressed = false; }

        Sleep(20);
    }
}

// --- ИНИЦИАЛИЗАЦИЯ WEBVIEW2 ---
void InitWebView2(HWND hwnd) {
    // Получаем путь к системной временной папке (%TEMP%)
    wchar_t tempPath[MAX_PATH];
    DWORD pathLen = GetTempPathW(MAX_PATH, tempPath);

    std::wstring userDataFolder;
    if (pathLen > 0 && pathLen < MAX_PATH) {
        userDataFolder = std::wstring(tempPath) + L"WebJag_WebView2Data";
    }
    else {
        userDataFolder = L"C:\\Windows\\Temp\\WebJag_WebView2Data";
    }

    // Передаем путь во второй параметр CreateCoreWebView2EnvironmentWithOptions
    CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(result) || !env) return result;

                env->CreateCoreWebView2Controller(hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) return result;

                            g_controller = controller;
                            g_controller->get_CoreWebView2(&g_webview);

                            ComPtr<ICoreWebView2Controller2> controller2;
                            if (SUCCEEDED(g_controller.As(&controller2))) {
                                COREWEBVIEW2_COLOR transparentColor = { 0, 0, 0, 0 };
                                controller2->put_DefaultBackgroundColor(transparentColor);
                            }

                            g_webview->add_SourceChanged(
                                Callback<ICoreWebView2SourceChangedEventHandler>(
                                    [](ICoreWebView2* sender, ICoreWebView2SourceChangedEventArgs* args) -> HRESULT {
                                        LPWSTR uri;
                                        if (SUCCEEDED(sender->get_Source(&uri))) {
                                            WideCharToMultiByte(CP_UTF8, 0, uri, -1, g_urlBuffer, sizeof(g_urlBuffer), NULL, NULL);
                                            CoTaskMemFree(uri);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);

                            UpdateWebViewBounds(hwnd);

                            ComPtr<ICoreWebView2Settings> settings;
                            if (SUCCEEDED(g_webview->get_Settings(&settings))) {
                                settings->put_AreDefaultContextMenusEnabled(TRUE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(TRUE);
                            }

                            const wchar_t* patchScript = LR"(
                            (function() {
                                const origGetContext = HTMLCanvasElement.prototype.getContext;
                                HTMLCanvasElement.prototype.getContext = function(type, attributes) {
                                    if (type === 'webgl' || type === 'webgl2' || type === 'experimental-webgl') {
                                        attributes = attributes || {};
                                        attributes.alpha = true;
                                        attributes.premultipliedAlpha = false;
                                    }
                                    return origGetContext.call(this, type, attributes);
                                };

                                const transparentCSS = `
                                    html, body {
                                        background: transparent !important;
                                        background-color: transparent !important;
                                    }
                                    *:not(img):not(svg):not(canvas):not(video):not(path):not(iframe) {
                                        background-color: transparent !important;
                                    }
                                    :root, [dark], [light] {
                                        --yt-spec-base-background: transparent !important;
                                        --yt-spec-general-background-a: transparent !important;
                                        --yt-spec-general-background-b: transparent !important;
                                        --yt-spec-menu-background: transparent !important;
                                        --yt-spec-raised-background: transparent !important;
                                        --yt-spec-additive-background: transparent !important;
                                        --yt-spec-outline: transparent !important;
                                    }
                                    #radar-viewport, #camera-tilt-rig, canvas, #radar3d, #waiting-overlay, #map-container {
                                        background: transparent !important;
                                        background-color: transparent !important;
                                    }
                                `;

                                function injectCleanStyle(targetRoot) {
                                    if (!targetRoot) return;
                                    if (targetRoot.querySelector && targetRoot.querySelector('#webjag-clean-style')) return;
                                    const style = document.createElement('style');
                                    style.id = 'webjag-clean-style';
                                    style.textContent = transparentCSS;
                                    (targetRoot.head || targetRoot).appendChild(style);
                                }

                                const origAttachShadow = Element.prototype.attachShadow;
                                Element.prototype.attachShadow = function(init) {
                                    const shadowRoot = origAttachShadow.call(this, init);
                                    try {
                                        injectCleanStyle(shadowRoot);
                                    } catch(e) {}
                                    return shadowRoot;
                                };

                                function applyClean() {
                                    injectCleanStyle(document.documentElement);
                                    if (document.head) injectCleanStyle(document.head);
                                    if (document.body) injectCleanStyle(document.body);
                                }

                                applyClean();
                                document.addEventListener('DOMContentLoaded', applyClean);
                                window.addEventListener('load', applyClean);
                                setInterval(applyClean, 400);

                                const observer = new MutationObserver(() => applyClean());
                                const rootNode = document.documentElement || document.body;
                                if (rootNode) {
                                    observer.observe(rootNode, { childList: true, subtree: false });
                                }
                            })();
                            )";

                            g_webview->AddScriptToExecuteOnDocumentCreated(patchScript, nullptr);
                            g_webview->Navigate(L"https://google.com");

                            return S_OK;
                        }).Get());

                return S_OK;
            }).Get());
}

void SetupImGuiStyleAndFont() {
    ImGuiIO& io = ImGui::GetIO();

    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesCyrillic());
    if (!font) {
        io.Fonts->AddFontDefault();
    }
    io.IniFilename = nullptr;
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 12.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 10.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.WindowBorderSize = 2.0f;
    style.FrameBorderSize = 2.0f;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text] = ImVec4(0.93f, 0.91f, 0.98f, 1.00f);
    colors[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.45f, 0.58f, 1.00f);


    colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.07f, 0.12f, 0.5f);
    colors[ImGuiCol_ChildBg] = ImVec4(0.10f, 0.09f, 0.15f, 0.50f);
    colors[ImGuiCol_PopupBg] = ImVec4(0.10f, 0.09f, 0.15f, 0.98f);
    colors[ImGuiCol_Border] = ImVec4(0.26f, 0.20f, 0.38f, 0.40f);

    colors[ImGuiCol_FrameBg] = ImVec4(0.15f, 0.13f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered] = ImVec4(0.21f, 0.18f, 0.31f, 1.00f);
    colors[ImGuiCol_FrameBgActive] = ImVec4(0.27f, 0.22f, 0.40f, 1.00f);


    colors[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.07f, 0.12f, 1.00f);
    colors[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.10f, 0.20f, 1.00f);


    colors[ImGuiCol_Button] = ImVec4(0.45f, 0.22f, 0.82f, 1.00f);
    colors[ImGuiCol_ButtonHovered] = ImVec4(0.52f, 0.28f, 0.90f, 1.00f);
    colors[ImGuiCol_ButtonActive] = ImVec4(0.38f, 0.18f, 0.72f, 1.00f);

    colors[ImGuiCol_Header] = ImVec4(0.24f, 0.18f, 0.38f, 0.75f);
    colors[ImGuiCol_HeaderHovered] = ImVec4(0.33f, 0.25f, 0.50f, 0.85f);
    colors[ImGuiCol_HeaderActive] = ImVec4(0.42f, 0.30f, 0.62f, 1.00f);

    colors[ImGuiCol_CheckMark] = ImVec4(0.68f, 0.45f, 0.98f, 1.00f);
    colors[ImGuiCol_SliderGrab] = ImVec4(0.45f, 0.22f, 0.82f, 1.00f);
    colors[ImGuiCol_SliderGrabActive] = ImVec4(0.58f, 0.32f, 0.95f, 1.00f);
}


LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_isInteractive && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_OVERLAY_TOGGLE:
        ApplyInteractivityState(hwnd, !g_isInteractive);
        return 0;

    case WM_EXITSIZEMOVE:
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
        ResetImGuiInputState();
        return 0;

    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED) {
            if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);

            ID3D11Texture2D* pBackBuffer;
            g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
            if (pBackBuffer) {
                g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
                pBackBuffer->Release();
            }

            UpdateWebViewBounds(hwnd);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// --- ТОЧКА ВХОДА ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW), CS_HREDRAW | CS_VREDRAW, WndProc, 0, 0, hInstance,
                      nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"OverlayClassWV2", nullptr };
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        L"OverlayClassWV2", L"WebJag Overlay",
        WS_POPUP, 100, 100, 500, 500,
        nullptr, nullptr, hInstance, nullptr
    );

    SetLayeredWindowAttributes(g_hwnd, 0, 255, LWA_ALPHA);
    MARGINS margins = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    SetupImGuiStyleAndFont();

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    std::thread hotkeyThread(HotkeyWorkerThread);
    hotkeyThread.detach();

    InitWebView2(g_hwnd);

    MSG msg;
    while (g_running) {
        while (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (!g_running) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();


        if (g_isInteractive) {
            RECT rc;
            GetClientRect(g_hwnd, &rc);
            float width = (float)rc.right;
            float height = (float)rc.bottom;


  
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(width, height));
            //ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.07f, 0.12f, 0.2f));

            ImGui::Begin("##frame", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings );



           

            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(ImVec2(width, 38.0f));
          //  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));

            ImGui::PopStyleColor();
             ImGui::Begin("##TopToolbar", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoSavedSettings);


            ImGui::Button(":: WebJag ::");
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                PerformWindowDragOrResize(g_hwnd, HTCAPTION);
            }

            ImGui::SameLine();

            float inputWidth = width - 100.0f;
            if (inputWidth < 100.0f) inputWidth = 98.0f;

            ImGui::SetNextItemWidth(inputWidth);
            if (ImGui::InputText("##UrlInput", g_urlBuffer, IM_ARRAYSIZE(g_urlBuffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
                NavigateToUrl(g_urlBuffer);
            }

      
            if (ImGui::IsItemClicked() || ImGui::IsItemActivated()) {
                SetFocus(g_hwnd);
            }

            ImGui::End();
           // ImGui::PopStyleVar();

  
            ImGui::SetNextWindowPos(ImVec2(0, height - 34.0f));
            ImGui::SetNextWindowSize(ImVec2(width, 20.0f));
          //  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 3.0f));

            ImGui::Begin("##BottomBar", nullptr,
                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoSavedSettings);


    
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "[HOME] - Edit Mode | [END] - Exit");

            ImGui::SameLine(width - 70);
            ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1.0f), "dubdive");
 
            ImGui::SetCursorPos(ImVec2(width - 24.0f, 10.0f));
            ImGui::InvisibleButton("##ResizeGrip", ImVec2(24.0f, 24.0f));

      
            ImDrawList* drawList = ImGui::GetWindowDrawList();
            drawList->AddTriangleFilled(
                ImVec2(width, height - 16.0f),
                ImVec2(width - 16.0f, height),
                ImVec2(width, height),
                IM_COL32(115, 56, 209, 255) 
            );

            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);
            }

            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                PerformWindowDragOrResize(g_hwnd, HTBOTTOMRIGHT);
            }

            ImGui::End();
            ImGui::End();
           // ImGui::PopStyleVar();

        }

        // Рендеринг
        ImGui::Render();
        const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);

        if (g_isInteractive) {
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }

        g_pSwapChain->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}
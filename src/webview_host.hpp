#pragma once

#include <windows.h>
#include <wrl.h>
#include "WebView2.h"
#include <string>
#include <functional>
#include <queue>
#include <mutex>
#include <iostream>

class WebViewHost {
public:
    using MessageHandler = std::function<void(const std::string&)>;

    WebViewHost() = default;
    ~WebViewHost() { shutdown(); }

    bool initialize(HWND hParent,
                    const std::wstring& userDataFolder,
                    const std::wstring& localAssetFolder,
                    MessageHandler onMessage) {
        m_parentHwnd = hParent;
        m_onMessage = std::move(onMessage);

        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        // S_FALSE or S_OK is fine (already initialized on this thread)

        hr = CreateCoreWebView2EnvironmentWithOptions(
            nullptr,
            userDataFolder.c_str(),
            nullptr,
            Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
                [this, localAssetFolder](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
                    if (FAILED(result) || !env) {
                        std::fprintf(stderr, "[webview] CreateCoreWebView2Environment failed: 0x%08lX\n", result);
                        return S_OK;
                    }
                    m_env = env;
                    m_env->CreateCoreWebView2Controller(
                        m_parentHwnd,
                        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                            [this, localAssetFolder](HRESULT res, ICoreWebView2Controller* controller) -> HRESULT {
                                if (FAILED(res) || !controller) {
                                    std::fprintf(stderr, "[webview] CreateCoreWebView2Controller failed: 0x%08lX\n", res);
                                    return S_OK;
                                }
                                m_controller = controller;
                                m_controller->get_CoreWebView2(&m_webView);

                                setupBridge();
                                setupRouting(localAssetFolder);

                                m_initialized = true;
                                if (m_pendingVisible) setVisible(true);
                                updateBounds();
                                drainPendingMessages();
                                return S_OK;
                            }
                        ).Get()
                    );
                    return S_OK;
                }
            ).Get()
        );

        return SUCCEEDED(hr);
    }

    void resize(int x, int y, int width, int height) {
        m_rect = { x, y, x + width, y + height };
        updateBounds();
    }

    void setVisible(bool visible) {
        m_pendingVisible = visible;
        if (m_controller) {
            m_controller->put_IsVisible(visible ? TRUE : FALSE);
        }
    }

    void postMessageJson(const std::string& jsonUtf8) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_webView) {
            m_pendingOutgoing.push(jsonUtf8);
            return;
        }
        dispatchJson(jsonUtf8);
    }

    void shutdown() {
        if (m_controller) {
            m_controller->Close();
            m_controller = nullptr;
        }
        m_webView = nullptr;
        m_env = nullptr;
        m_initialized = false;
    }

    void pushAction(std::string actionJson) {
        std::lock_guard<std::mutex> lock(m_actionMutex);
        m_actionQueue.push(std::move(actionJson));
    }

    void drainActions(const std::function<void(const std::string&)>& handler) {
        std::queue<std::string> toProcess;
        {
            std::lock_guard<std::mutex> lock(m_actionMutex);
            toProcess.swap(m_actionQueue);
        }
        while (!toProcess.empty()) {
            if (handler) handler(toProcess.front());
            toProcess.pop();
        }
    }

    bool isInitialized() const { return m_initialized; }

private:
    void setupBridge() {
        if (!m_webView) return;
        EventRegistrationToken token;
        m_webView->add_WebMessageReceived(
            Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                    LPWSTR rawJson = nullptr;
                    if (SUCCEEDED(args->get_WebMessageAsJson(&rawJson)) && rawJson) {
                        int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, rawJson, -1, nullptr, 0, nullptr, nullptr);
                        if (sizeNeeded > 0) {
                            std::string utf8Str(sizeNeeded - 1, '\0');
                            WideCharToMultiByte(CP_UTF8, 0, rawJson, -1, &utf8Str[0], sizeNeeded, nullptr, nullptr);
                            pushAction(utf8Str);
                            if (m_onMessage) {
                                m_onMessage(utf8Str);
                            }
                        }
                        CoTaskMemFree(rawJson);
                    }
                    return S_OK;
                }
            ).Get(),
            &token
        );
    }

    void setupRouting(const std::wstring& localAssetFolder) {
        if (!m_webView) return;
        Microsoft::WRL::ComPtr<ICoreWebView2_3> webView3;
        if (SUCCEEDED(m_webView.As(&webView3)) && webView3) {
            webView3->SetVirtualHostNameToFolderMapping(
                L"imagoro.engine",
                localAssetFolder.c_str(),
                COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW
            );
            m_webView->Navigate(L"https://imagoro.engine/index.html");
        } else {
            std::wstring fileUri = L"file:///" + localAssetFolder + L"/index.html";
            m_webView->Navigate(fileUri.c_str());
        }
    }

    void updateBounds() {
        if (m_controller) {
            m_controller->put_Bounds(m_rect);
        }
    }

    void dispatchJson(const std::string& jsonUtf8) {
        int wLen = MultiByteToWideChar(CP_UTF8, 0, jsonUtf8.c_str(), -1, nullptr, 0);
        if (wLen <= 0) return;
        std::wstring wJson(wLen - 1, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, jsonUtf8.c_str(), -1, &wJson[0], wLen);
        m_webView->PostWebMessageAsJson(wJson.c_str());
    }

    void drainPendingMessages() {
        std::lock_guard<std::mutex> lock(m_mutex);
        while (!m_pendingOutgoing.empty()) {
            dispatchJson(m_pendingOutgoing.front());
            m_pendingOutgoing.pop();
        }
    }

    HWND m_parentHwnd = nullptr;
    RECT m_rect{0, 0, 0, 0};
    bool m_initialized = false;
    bool m_pendingVisible = false;
    MessageHandler m_onMessage;

    Microsoft::WRL::ComPtr<ICoreWebView2Environment> m_env;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> m_controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> m_webView;

    std::mutex m_mutex;
    std::queue<std::string> m_pendingOutgoing;

    std::mutex m_actionMutex;
    std::queue<std::string> m_actionQueue;
};

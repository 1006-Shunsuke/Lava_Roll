//-----------------------------------------------------------------------------
// File : App_Init.cpp
// Desc : 初期化の入口・ウィンドウ・Direct3D の初期化。コンストラクタ・InitApp・InitWnd・InitD3D
//-----------------------------------------------------------------------------
#include "App.h"
#include <string>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"


using namespace DirectX;

// ウィンドウクラス名とウィンドウ名の定義
namespace
{
    const auto WindowName = TEXT("SampleWindow");
}

App::App(uint32_t width, uint32_t height, bool editorMode)
    :m_hInst(nullptr) // インスタンスハンドル
    , m_hWnd(nullptr) // ウィンドウハンドル
    , m_Width(width)
    , m_Height(height)
    , m_FenceEvent(nullptr)
    , m_FenceCounter{}
    , m_FrameIndex(0)
    , m_HandleRTV{}
    , m_VBV{}
    , m_Viewport{}
    , m_Scissor{}
{
    m_EditorMode = editorMode;
}

App::~App()
{

}

// 初期化処理
bool App::InitApp()
{
    // COMの初期化。WICで画像を読み込むのに必要
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
    {
        MessageBoxW(nullptr, L"CoInitializeEx で失敗", L"エラー", MB_OK);
        return false;
    }
    // ウィンドウの初期化
    if (!InitWnd())
    {
        MessageBoxW(nullptr, L"InitWnd で失敗", L"エラー", MB_OK);
        return false;
    }
    // Direct3D12の初期化
    if (!InitD3D())
    {
        MessageBoxW(nullptr, L"InitD3D で失敗", L"エラー", MB_OK);
        return false;
    }
    // ImGuiの初期化
    if (!InitImGui())
    {
        MessageBoxW(nullptr, L"InitImGui で失敗", L"エラー", MB_OK);
        return false;
    }
    if (!OnInit())
    {
        MessageBoxW(nullptr, L"OnInit で失敗", L"エラー", MB_OK);
        return false;
    }
    m_Timer.Init();

    return true;
}

bool App::InitWnd() // ウィンドウの初期化処理
{
    auto hInst = GetModuleHandle(nullptr);
    if (hInst == nullptr)
    {
        return false;
    }

    // ウィンドウの設定
    WNDCLASSEX wc = {};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW; // ウィンドウのスタイル：サイズが変わったら再描画する
    wc.lpfnWndProc = WndProc;
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION); // アイコンの設定
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW); // カーソルの設定
    wc.hbrBackground = GetSysColorBrush(COLOR_BACKGROUND); // 背景色の設定
    wc.lpszMenuName = nullptr; // メニューの設定
    wc.lpszClassName = ClassName; // クラス名の設定
    wc.hIconSm = LoadIcon(nullptr, IDI_APPLICATION); // 小さいアイコンの設定

    // ウィンドウの登録
    if (!RegisterClassEx(&wc))
    {
        return false;
    }

    // インスタンスハンドル設定
    m_hInst = hInst;

    // ウィンドウのサイズの調整
    // WS_OVERLAPPEDWINDOW：タイトルバー・閉じるボタンに加えて、枠の伸縮・最大化・最小化ができる普通のウィンドウ
    auto style = WS_OVERLAPPEDWINDOW;

    RECT rc = {};
    rc.right = static_cast<LONG>(m_Width);
    rc.bottom = static_cast<LONG>(m_Height);
    AdjustWindowRect(&rc, style, FALSE);

    // ウィンドウの作成
    m_hWnd = CreateWindowEx(
        0, // 拡張スタイル
        ClassName, // クラス名
        m_EditorMode ? TEXT("Lava Roll - Editor") : TEXT("Lava Roll"), // ウィンドウ名
        style, // ウィンドウスタイル
        CW_USEDEFAULT, // X座標
        CW_USEDEFAULT, // Y座標
        rc.right - rc.left, // 幅
        rc.bottom - rc.top, // 高さ
        nullptr, // 親ウィンドウハンドル
        nullptr, // メニューハンドル
        m_hInst, // インスタンスハンドル
        this // 追加パラメータ：この App のポインタ。WndProc の WM_NCCREATE で受け取って覚えておく
    );
    if (m_hWnd == nullptr)
    {
        return false;
    }
    // ウィンドウの表示
    ShowWindow(m_hWnd, SW_SHOW);
    UpdateWindow(m_hWnd);

    // ウィンドウにフォーカスを設定
    SetFocus(m_hWnd);

    return true;
}

bool App::InitD3D()
{
    // 0.デバッグのチェック
#if defined(DEBUG)||defined(_DEBUG)
    {
        ComPtr<ID3D12Debug>debug;
        auto hr = D3D12GetDebugInterface(IID_PPV_ARGS(debug.GetAddressOf()));

        // デバッグレイヤーを有効化
        if (SUCCEEDED(hr))
        {
            debug->EnableDebugLayer();
        }
    }
#endif
    // 1.デバイスの生成
    auto hr = D3D12CreateDevice(
        nullptr, // デフォルトアダプタ
        D3D_FEATURE_LEVEL_11_0, // 機能レベル
        IID_PPV_ARGS(_device.GetAddressOf())
    );
    if (FAILED(hr))
    {
        return false;
    }
    // 2.コマンドキューの生成
    {
        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDesc.NodeMask = 0;

        hr = _device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&_queue));
        if (FAILED(hr))
        {
            return false;
        }
    }
    // 3.Swap Chain
    // 3.1 DXGI Factoryの生成。スワップチェーンはDXGIの機能なので先に作る
    // ComPtr なので、関数を抜けるときに自動で解放される
    ComPtr<IDXGIFactory4> pFactory;
    hr = CreateDXGIFactory1(IID_PPV_ARGS(pFactory.GetAddressOf()));
    if (FAILED(hr))
    {
        return false;
    }

    // 3.2 Swap Chainの設定
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferDesc.Width = m_Width;
    swapChainDesc.BufferDesc.Height = m_Height;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 60;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
    swapChainDesc.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.OutputWindow = m_hWnd;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

    // 生成
    ComPtr<IDXGISwapChain> pSwapChain;
    hr = pFactory->CreateSwapChain(
        _queue.Get(), // コマンドキュー
        &swapChainDesc, // Swap Chainの設定
        pSwapChain.GetAddressOf() // Swap Chainの実体が入る
    );
    if (FAILED(hr))
    {
        return false;
    }

    // IDXGISwapChain3を取得
    hr = pSwapChain->QueryInterface(IID_PPV_ARGS(&_swapchain));
    if (FAILED(hr))
    {
        return false;
    }
    m_FrameIndex = _swapchain->GetCurrentBackBufferIndex();

    // 4. コマンドアロケータとコマンドリストの生成
    {
        for (auto i = 0u; i < FrameCount; i++)
        {
            hr = _device->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                IID_PPV_ARGS(_cmdAllocator[i].GetAddressOf()));
            if (FAILED(hr))
            {
                return false;
            }
        }
    }
    // 生成
    {
        hr = _device->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            _cmdAllocator[m_FrameIndex].Get(),
            nullptr,
            IID_PPV_ARGS(_cmdList.GetAddressOf()));
        if (FAILED(hr))
        {
            return false;
        }
    }
    _cmdList->Close();

    // 5. RTV用のディスクリプタヒープ
    // 本来,ディスクリプタヒープは引数としてリソースとディスクリプタヒープが必要だが,スワップチェインで作成したバッファをリソースとするため、リソースを作成する必要はない
    // ディスクリプタヒープはRTV用の情報を入れる配列
    // RTV 用で、数はバックバッファと同じ。シェーダーからは見えなくてよい
    hr = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FrameCount, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, _heapRTV);
    if (FAILED(hr))
    {
        return false;
    }
    // RTV を作る。リサイズのときも同じ処理を使うので関数にしてある
    if (!CreateRenderTargetViews())
    {
        return false;
    }

    // 6. フェンスの生成：CPUとGPUの同期に使う
    // 生成
    hr = _device->CreateFence(
        m_FenceCounter[m_FrameIndex],
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&_fence)
    );
    if (FAILED(hr))
    {
        return false;
    }

    m_FenceCounter[m_FrameIndex]++;
    // イベントの生成
    m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (m_FenceEvent == nullptr)
    {
        return false;
    }

    return true;
}

bool App::InitImGui()
{
    // 1. ImGui専用のSRVヒープ。シェーダーから見える
    // 数は64。1.92以降はフォント以外にも使うので余裕を持たせる
    if (FAILED(CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, _heapImGui)))
    {
        return false;
    }
    m_ImGuiDescSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_ImGuiHeapNext = 0;

    // 2. コンテキスト作成とWin32側の初期化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // フォント：1つ目は Inspector 用の標準フォント。2つ目はタイトルと HUD 用に、Windows に入っている Segoe UI（太字）を読む
    // ImGui 1.92 以降は、使うときの大きさで文字を作り直すので、大きくしてもぼやけない
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->AddFontDefault();

        wchar_t winDir[MAX_PATH] = {};
        GetWindowsDirectoryW(winDir, MAX_PATH);
        const std::wstring fontPathW = std::wstring(winDir) + L"\\Fonts\\segoeuib.ttf";
        if (GetFileAttributesW(fontPathW.c_str()) != INVALID_FILE_ATTRIBUTES)     // 見つからないときは標準フォントのまま
        {
            char fontPath[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, fontPathW.c_str(), -1, fontPath, sizeof(fontPath), nullptr, nullptr);
            m_UiFont = io.Fonts->AddFontFromFileTTF(fontPath, 32.0f);
        }

        // ゲームモードでは imgui.ini（ウィンドウの位置の保存）を作らない
        if (!m_EditorMode)
        {
            io.IniFilename = nullptr;
        }
    }
    if (!ImGui_ImplWin32_Init(m_hWnd))
    {
        return false;
    }

    // 3. DX12側の初期化
    ImGui_ImplDX12_InitInfo initInfo;
    initInfo.Device = _device.Get();
    initInfo.CommandQueue = _queue.Get();
    initInfo.NumFramesInFlight = FrameCount;
    initInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;  // InitD3D で作った RTV と同じフォーマットにする
    initInfo.DSVFormat = DXGI_FORMAT_D32_FLOAT;            // Render でDSVを設定しているため
    initInfo.UserData = this;
    initInfo.SrvDescriptorHeap = _heapImGui.Get();
    initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo* i,
        D3D12_CPU_DESCRIPTOR_HANDLE* outCpu, D3D12_GPU_DESCRIPTOR_HANDLE* outGpu)
        {
            auto* app = static_cast<App*>(i->UserData);
            const UINT idx = app->m_ImGuiHeapNext++;
            *outCpu = app->_heapImGui->GetCPUDescriptorHandleForHeapStart();
            *outGpu = app->_heapImGui->GetGPUDescriptorHandleForHeapStart();
            outCpu->ptr += SIZE_T(idx) * app->m_ImGuiDescSize;
            outGpu->ptr += UINT64(idx) * app->m_ImGuiDescSize;
        };
    initInfo.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*,
        D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_GPU_DESCRIPTOR_HANDLE) {};  // 今回は解放しない
    return ImGui_ImplDX12_Init(&initInfo);
}

//-----------------------------------------------------------------------------
// スワップチェインの画像（バックバッファ）を取り出して、RTV をヒープに作る
// 起動時（InitD3D）と、ウィンドウの大きさが変わったとき（OnResize）に呼ぶ
//-----------------------------------------------------------------------------
bool App::CreateRenderTargetViews()
{
    // RTV用ディスクリプタヒープの先頭のCPUディスクリプタハンドルを取得する
    auto rtvHandle = _heapRTV->GetCPUDescriptorHandleForHeapStart();
    // RTVディスクリプタ1個分のバイトサイズを取得,アドレス間の間隔サイズ
    auto rtvDescriptorSize = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (auto i = 0u; i < FrameCount; ++i)
    {
        auto hr = _swapchain->GetBuffer(i, IID_PPV_ARGS(_colorBuffer[i].ReleaseAndGetAddressOf())); // バックバッファのポインタの取得
        if (FAILED(hr))
        {
            return false;
        }
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; // sRGB
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; // 2次元テクスチャとしてアクセス
        rtvDesc.Texture2D.MipSlice = 0;
        rtvDesc.Texture2D.PlaneSlice = 0;
        // 生成
        _device->CreateRenderTargetView(_colorBuffer[i].Get(), &rtvDesc, rtvHandle);
        m_HandleRTV[i] = rtvHandle;
        rtvHandle.ptr += rtvDescriptorSize;
    }
    return true;
}

//-----------------------------------------------------------------------------
// ウィンドウの大きさが変わったときに、画面の大きさに関わるものを全部作り直す
//   1. GPU が今の画像を使い終わるまで待つ
//   2. スワップチェインの画像への参照を手放す（持ったままだと ResizeBuffers が失敗する）
//   3. スワップチェインの画像を新しい大きさで作り直す
//   4. RTV・深度バッファ・ビューポートを作り直す
// 射影行列の縦横比とピッキングは、毎フレーム m_Width / m_Height を読むので自動で合う
//-----------------------------------------------------------------------------
void App::OnResize(uint32_t width, uint32_t height)
{
    // 大きさ 0（最小化）では作れない。同じ大きさなら何もしない
    if (width == 0 || height == 0 || _swapchain == nullptr)
    {
        return;
    }
    if (width == m_Width && height == m_Height)
    {
        return;
    }

    // 1. GPU の処理が全部終わるまで待つ
    WaitGpu();

    // 2. スワップチェインの画像と深度バッファを手放す
    for (auto i = 0u; i < FrameCount; ++i)
    {
        _colorBuffer[i].Reset();
    }
    _depthBuffer.Reset();

    // 3. スワップチェインの画像を作り直す。形式とフラグは今のものをそのまま使う
    DXGI_SWAP_CHAIN_DESC desc = {};
    _swapchain->GetDesc(&desc);
    auto hr = _swapchain->ResizeBuffers(FrameCount, width, height, desc.BufferDesc.Format, desc.Flags);
    if (FAILED(hr))
    {
        OutputDebugStringW(L"[OnResize] ResizeBuffers に失敗しました\n");
        return;
    }

    m_Width  = width;
    m_Height = height;

    // フェンスの値をそろえる。作り直したあとは、どの画像から使い始めるかが変わるため
    // WaitGpu で全部終わっているので、全フレームを「今の値」にしておけば待ちすぎも待たなすぎも起きない
    const auto fenceValue = m_FenceCounter[m_FrameIndex];
    for (auto i = 0u; i < FrameCount; ++i)
    {
        m_FenceCounter[i] = fenceValue;
    }
    m_FrameIndex = _swapchain->GetCurrentBackBufferIndex();

    // 4. RTV・深度バッファ・ビューポートを作り直す
    if (!CreateRenderTargetViews())
    {
        OutputDebugStringW(L"[OnResize] RTV の作り直しに失敗しました\n");
        return;
    }
    if (!CreateDepthResource())
    {
        OutputDebugStringW(L"[OnResize] 深度バッファの作り直しに失敗しました\n");
        return;
    }
    InitViewport();
}

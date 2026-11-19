//-----------------------------------------------------------------------------
// File : App.h
// Desc : アプリケーション本体
//-----------------------------------------------------------------------------
#pragma once

//-----------------------------------------------------------------------------
// Includes
//-----------------------------------------------------------------------------
#include <Windows.h>
#include <cstdint>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include "Timer.h" // 経過時間を測る Timer クラス
#include <vector>
#include <algorithm>   // std::min を使う

struct ImFont;          // ImGui のフォント。中身は imgui.h にあるので、ここでは名前だけ宣言する
#include "Mesh.h"  // メッシュの読み込み：MeshVertex・LoadMesh

//-----------------------------------------------------------------------------
// Linker
//-----------------------------------------------------------------------------
#pragma comment( lib, "d3d12.lib" )
#pragma comment( lib, "dxgi.lib" )
#pragma comment( lib, "d3dcompiler.lib" )

//-----------------------------------------------------------------------------
// Type Alias
//-----------------------------------------------------------------------------
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;


///////////////////////////////////////////////////////////////////////////////
// Transform structure
///////////////////////////////////////////////////////////////////////////////
struct alignas(256) Transform
{
    DirectX::XMMATRIX   World;      // ワールド行列
    DirectX::XMMATRIX   View;       // ビュー行列
    DirectX::XMMATRIX   Proj;       // 射影行列
    DirectX::XMMATRIX   WorldInvTrans; // ワールド行列の逆行列の転置。法線の変換に使う。4つで 64×4 = 256バイト
    DirectX::XMFLOAT4 Color;
    DirectX::XMFLOAT4 UVParams;     // xy: テクスチャを何回くり返して貼るか（UV Scale）、zw: テクスチャをずらす量（UV Scroll × 時間）
    DirectX::XMFLOAT4 Material;     // x: 光る強さ（Emissive）。yzw は今は使わない
};

///////////////////////////////////////////////////////////////////////////////
// SkyParam structure : 背景の空の設定。空の VS の b0 に渡す
///////////////////////////////////////////////////////////////////////////////
struct alignas(256) SkyParam
{
    DirectX::XMMATRIX   InvViewProj;    // (ビュー行列 × 射影行列) の逆行列。画面の位置 → ワールドの位置
    DirectX::XMFLOAT4   CameraPos;      // カメラの位置
    DirectX::XMFLOAT4   TopColor;       // 真上の色
    DirectX::XMFLOAT4   HorizonColor;   // 地平線の色
    DirectX::XMFLOAT4   BottomColor;    // 真下の色
    DirectX::XMFLOAT4   Params;         // x: 空の種類（0 = グラデーション、1 = 画像）、y: 横の回転（1で1周）、z: グラデーションの曲がり具合
};

///////////////////////////////////////////////////////////////////////////////
// LightParam structure : ライトの設定。PS の b1 に渡す
// HLSL の定数バッファは float4 の16バイト単位で並ぶので、float3 + float で1組にする
///////////////////////////////////////////////////////////////////////////////
struct alignas(256) LightParam
{
    DirectX::XMFLOAT3   LightDir;           // 光が進む向き：正規化済み          c0.xyz
    float               LightIntensity;     // 光の強さ                          c0.w
    DirectX::XMFLOAT3   LightColor;         // 光の色                            c1.xyz
    float               Shininess;          // 鏡面反射の鋭さ                    c1.w
    DirectX::XMFLOAT3   CameraPos;          // カメラの位置                      c2.xyz
    float               SpecularIntensity;  // 鏡面反射の強さ                    c2.w
    DirectX::XMFLOAT3   AmbientColor;       // 環境光の色                        c3.xyz
    float               Padding;            // 16バイトにそろえるための詰め物    c3.w
};

///////////////////////////////////////////////////////////////////////////////
// App class
///////////////////////////////////////////////////////////////////////////////
class App
{
public:
    App(uint32_t width, uint32_t height, bool editorMode);  // editorMode が true ならエディタ、false ならゲームとして起動する
    ~App();
    void Run();

private:

    static const uint32_t FrameCount = 2;   // フレームバッファ数

    static constexpr uint32_t MaxTextures = 32;  // 読み込めるテクスチャの最大数。SRV ヒープの大きさになる
    static constexpr uint32_t MaxObjects = 64; // 定数バッファに入る物体の最大数。物体の数そのものではなく「上限」

    // ルートパラメータの番号。ルートシグニチャの何番目の引数かを表す
    // 作るときの CreateRootSignature と使うときの Render で同じ番号を使うために名前を付ける
    // ※ HLSL の register 番号 b0・t0・b1 とは別物
    enum RootParam
    {
        RootParam_Transform = 0,    // b0 : 変換行列。ルートCBVで渡す（物体ごとにアドレスをずらす）
        RootParam_Texture,          // t0 : テクスチャ。ディスクリプタテーブルで渡す
        RootParam_Light,            // b1 : ライト。ルートCBVで渡す
        RootParam_Count,            // ルートパラメータの数
    };

    static constexpr const TCHAR* ClassName = TEXT("SampleWindowClass");   // ウィンドウクラス名。InitWnd と TermWnd で使う

    HINSTANCE   m_hInst;        // インスタンスハンドル
    HWND        m_hWnd;         // ウィンドウハンドル
    uint32_t    m_Width;        // ウィンドウの横幅
    uint32_t    m_Height;       // ウィンドウの縦幅

    ComPtr<ID3D12Device>                   _device;                      // デバイス
    ComPtr<ID3D12CommandQueue>             _queue;                       // コマンドキュー
    ComPtr<IDXGISwapChain3>                _swapchain;                   // スワップチェイン
    ComPtr<ID3D12Resource>                 _colorBuffer[FrameCount];     // カラーバッファ
    ComPtr<ID3D12CommandAllocator>         _cmdAllocator[FrameCount];    // コマンドアロケータ
    ComPtr<ID3D12GraphicsCommandList>      _cmdList;                     // コマンドリスト
    ComPtr<ID3D12Fence>                    _fence;                       // フェンス
    ComPtr<ID3D12DescriptorHeap>           _heapRTV;                     // RTV用のディスクリプタヒープ
    ComPtr<ID3D12Resource>                 _vB;                          // 頂点バッファ
    ComPtr<ID3D12Resource>                 _iB;                          // インデックスバッファ
    ComPtr<ID3D12Resource>                 _cB[FrameCount];              // 定数バッファ
    ComPtr<ID3D12Resource>                 _lightCB[FrameCount];         // ライト用の定数バッファ。フレーム数分
    ComPtr<ID3D12DescriptorHeap>           _heapDSV;                     // DSV用のディスクリプタヒープ
    ComPtr<ID3D12Resource>                 _depthBuffer;                 // 深度バッファ
    ComPtr<ID3D12RootSignature>            _rootSignature;               // ルートシグニチャ
    ComPtr<ID3D12PipelineState>            _pSO;                         // パイプラインステート
    ComPtr<ID3D12PipelineState>            _skyPSO;                      // 背景の空を描くパイプラインステート
    ComPtr<ID3D12Resource>                 _skyCB[FrameCount];           // 空の設定の定数バッファ。フレーム数分
    ComPtr<ID3D12DescriptorHeap>           _heapCBV_SRV_UAV;             // CBV・SRV・UAV用のディスクリプタヒープ。シェーダから見える
    std::vector<ComPtr<ID3D12Resource>>    m_Textures;                   // 読み込んだ全テクスチャ。i 番目の SRV はヒープの i 番目
    D3D12_GPU_DESCRIPTOR_HANDLE            m_TextureHeapStart{};         // SRV ヒープの先頭の GPU ハンドル。描画時に番号 × m_SrvDescSize を足して使う
    UINT                                   m_SrvDescSize = 0;            // SRV 1個分の大きさ。GPU によって違うので問い合わせて入れる



    HANDLE                          m_FenceEvent;                   // フェンスイベント
    uint64_t                        m_FenceCounter[FrameCount];     // フェンスカウンター
    uint32_t                        m_FrameIndex;                   // フレーム番号
    D3D12_CPU_DESCRIPTOR_HANDLE     m_HandleRTV[FrameCount];        // レンダーターゲットビューのCPUディスクリプタ
    D3D12_VERTEX_BUFFER_VIEW        m_VBV;                          // 頂点バッファビュー
    D3D12_CPU_DESCRIPTOR_HANDLE     m_HandleDSV{};                  // 深度バッファのDSVのCPUハンドル。描画時に OMSetRenderTargets と ClearDepthStencilView で使う
    D3D12_VIEWPORT                  m_Viewport;                     // ビューポート
    D3D12_RECT                      m_Scissor;                      // シザー矩形
    D3D12_INDEX_BUFFER_VIEW         m_IBV;                          // インデックスバッファビュー
    Transform* m_pTransform[FrameCount] = {};  // 変換行列用の定数バッファ b0 の書き込み先
    LightParam* m_pLight[FrameCount] = {};     // ライト用定数バッファの書き込み先

    // メッシュの描画範囲
    // 全メッシュの頂点・インデックスは1本ずつのバッファにまとめて入れ、
    // 「インデックスの何番目から何個を、頂点の何番目を0番として描くか」をメッシュごとに覚えておく
    struct DrawRange
    {
        UINT    IndexCount;     // 描くインデックスの数
        UINT    StartIndex;     // インデックスバッファの何番目から読むか
        INT     BaseVertex;     // 読んだインデックスに足す値。このメッシュの頂点の開始位置にあたる
    };
    std::vector<DrawRange>          m_DrawRanges;

    // 決まった番号のモデル。GameObject::model に入れる
    // 2番からあとは、res\model フォルダにあるモデル（起動時に全部読み込む。番号はフォルダの中身で変わる）
    enum ModelId
    {
        Model_Sphere = 0,       // 球：res/model/sphere.obj
        Model_Box,              // 箱：CreateBoxMesh でコードから作る。1×1×1 で中心が原点
        Model_BuiltinCount,     // 決まった番号のモデルの数。これより後ろがフォルダから読んだモデル
    };

    // モデル1個分の範囲。m_DrawRanges の first 番目から count 個の部品でできている
    // 1つのモデルが複数の部品（メッシュ）でできていることがあるので、DrawRange とは分けて持つ
    struct ModelRange
    {
        UINT                first;      // m_DrawRanges の何番目から
        UINT                count;      // 何個の部品か
        std::string         name;       // Inspector の表示と保存ファイルに使う名前。"Sphere"・"Box"・"slope.obj" など
        DirectX::XMFLOAT3   boundsMin;  // モデルをぴったり囲む箱（ローカル空間）の小さい方の角
        DirectX::XMFLOAT3   boundsMax;  // 大きい方の角。当たり判定・着地・クリックでの選択に使う

        // 三角形での当たり判定用（res\model から読み込んだモデルだけ。Sphere と Box は球・箱の計算を使う）
        bool                            useTriangles = false;   // true なら、当たり判定・着地・選択を三角形で行う
        std::vector<DirectX::XMFLOAT3>  triPositions;           // 全部の頂点の位置（ローカル空間）
        std::vector<uint32_t>           triIndices;             // 3個で三角形1枚。triPositions の番号
    };
    std::vector<ModelRange>         m_Models;   // m_Models[ModelId] で引く
    int FindModelByName(const std::string& name) const;     // 名前からモデルの番号を探す。無ければ -1

    // 物体の役割。プレイモードでの扱いが変わる。GameObject::role に入れる
    enum ObjectRole
    {
        Role_None = 0,      // 飾り：見えるだけ。ボールは通り抜ける
        Role_Player,        // プレイヤー：キーで転がすボール。1個だけ使う
        Role_Wall,          // 壁・床：ボールが乗れる（箱でも球でも）。横から当たると止まるのは Step 8
        Role_Item,          // アイテム：回っていて、触れると取れる（Step 8・9）
        Role_Gate,          // ゲート：閉じている間は Wall と同じ。同じグループのアイテムを全部取ると沈んで通れるようになる
        Role_Damage,        // ダメージ：通り抜けるが、触れると少し前にいた安全な場所へ戻される。取っても消えない
        Role_Goal,          // ゴール：通り抜ける。触れるとクリア（"GOAL!" とクリアタイムを表示）
        Role_Count,         // 役割の数
    };

    Timer                           m_Timer; // フレーム間の経過時間を測るタイマー

    // ImGui
    ComPtr<ID3D12DescriptorHeap>    _heapImGui;          // ImGui専用のSRVヒープ。シェーダーから見える
    UINT                            m_ImGuiHeapNext = 0;   // 次に割り当てる位置
    UINT                            m_ImGuiDescSize = 0;   // ディスクリプタ1個のサイズ

    // エディタで編集する状態。Inspectorから書き換えられる
    DirectX::XMFLOAT3               m_Position = { 0, 0, 0 };
    DirectX::XMFLOAT3               m_RotationDeg = { 0, 0, 0 };
    DirectX::XMFLOAT3               m_Scale = { 1, 1, 1 };
    bool                            m_AutoRotate = true;
    float                           m_ClearColor[4] = { 0.2f, 0.2f, 0.25f, 1.0f };

    // カメラとライト。Inspector から書き換えられる
    DirectX::XMFLOAT3               m_CameraPos = { 0.0f, 6.0f, 12.0f };         // カメラの位置。ビュー行列と鏡面反射の両方で使う
    float                           m_CameraYaw   = 0.0f;       // カメラの左右の向き（度）。0 で奥（-Z）を向き、増やすと右を向く
    float                           m_CameraPitch = 0.0f;       // カメラの上下の向き（度）。正で上、負で下を向く。±89 で止める
    bool                            m_CameraMode  = false;      // true のとき、WASDQE・矢印キーで物体ではなくカメラを動かす
    bool                            m_MouseLooking = false;     // 右ドラッグでカメラを回している最中なら true
    float                           m_MouseSensitivity = 0.2f;  // マウスを1ピクセル動かすと何度回るか

    // カメラの初期値と、射影行列の設定
    static constexpr DirectX::XMFLOAT3 DefaultCameraPos    = { 0.0f, 6.0f, 12.0f };  // 最初の位置
    static constexpr DirectX::XMFLOAT3 DefaultCameraTarget = { 0.0f, 0.0f, 0.0f };   // 最初に見る点（原点）
    static constexpr float          CameraFovYDeg = 37.5f;      // 縦の視野角（度）
    static constexpr float          CameraNear    = 1.0f;       // これより近いものは描かない
    static constexpr float          CameraFar     = 1000.0f;    // これより遠いものは描かない
    DirectX::XMFLOAT3               m_LightDir = { -1.0f, -1.0f, -1.0f };       // 光が進む向き。右上手前から奥へ
    float                           m_LightColor[3] = { 1.0f, 1.0f, 1.0f };     // 光の色
    float                           m_LightIntensity = 1.0f;                    // 光の強さ
    float                           m_AmbientColor[3] = { 0.1f, 0.1f, 0.1f };   // 環境光の色
    float                           m_SpecularIntensity = 0.5f;                 // 鏡面反射の強さ
    float                           m_Shininess = 32.0f;                        // 鏡面反射の鋭さ

    // 背景の空。Inspector の Sky 欄で変えられ、ステージのファイルに保存される
    SkyParam*                       m_pSky[FrameCount] = {};                    // 空の定数バッファの書き込み先
    int                             m_SkyMode      = 0;                         // 0 = グラデーション、1 = 画像（パノラマ）
    float                           m_SkyTop[3]     = { 0.02f, 0.0f, 0.0f };    // 真上の色（火山の夜空：ほぼ黒）
    float                           m_SkyHorizon[3] = { 0.8f, 0.25f, 0.05f };   // 地平線の色（溶岩に照らされたオレンジ）
    float                           m_SkyBottom[3]  = { 0.1f, 0.02f, 0.0f };    // 真下の色（暗い赤）
    float                           m_SkyExponent  = 0.5f;                      // グラデーションの曲がり具合。小さいほど地平線の色が上まで広がる
    int                             m_SkyTexture   = 0;                         // 画像のときに使うテクスチャの番号
    float                           m_SkyRotation  = 0.0f;                      // 画像のときに、空を横に回す角度（度）
    float                           m_TotalTime    = 0.0f;                      // 起動してからの時間（秒）。UV Scroll に使う（エディタ中も進む）

    // テクスチャの一覧。番号は m_Textures と同じ（i 番目のファイル = m_Textures[i]）
    std::vector<std::wstring>       m_TextureFiles;             // 読み込んだ画像ファイルのパス
    std::vector<std::string>        m_TextureNames;             // ファイル名。Inspector の表示と保存ファイルに使う。UTF-8
    int                             m_DefaultTexture = 0;       // 既定のテクスチャの番号。テクスチャ未指定の物体に使う

    //=========================================================================
    // private 関数
    //=========================================================================
    bool InitApp();
    void TermApp();
    bool InitWnd();
    void TermWnd();
    void MainLoop();

    bool InitD3D();
    void TermD3D();
    void Render();
    void WaitGpu();
    void Present(uint32_t interval);

    // ウィンドウの大きさが変わったときの処理。App_Init.cpp
    bool CreateRenderTargetViews();             // スワップチェインの画像を取り出して RTV を作る。起動時とリサイズ時に使う
    bool CreateDepthResource();                 // 今の m_Width × m_Height で深度バッファと DSV を作る。起動時とリサイズ時に使う
    void OnResize(uint32_t width, uint32_t height); // スワップチェイン・深度バッファ・ビューポートを新しい大きさで作り直す
    bool     m_ResizePending = false;           // WM_SIZE で大きさが変わったら true。次のフレームの最初に OnResize する
    uint32_t m_PendingWidth  = 0;               // 予約された新しい幅
    uint32_t m_PendingHeight = 0;               // 予約された新しい高さ
    bool     m_Minimized     = false;           // 最小化中は true。大きさが 0 なので描画しない
    void Update(float dt);
    void HandleKeyInput(float dt);
    void MoveCamera(float dt);                  // カメラモードのとき：WASDQE で移動、矢印キーで向きを変える
    void RotateCameraByMouse();                 // 右ボタンを押しながらマウスを動かすと、カメラの向きを変える（いつでも）
    DirectX::XMMATRIX GetViewMatrix() const;    // カメラの位置と向きからビュー行列を作る。描画とピッキングで同じものを使う
    DirectX::XMMATRIX GetProjMatrix() const;    // 視野角と画面の縦横比から射影行列を作る
    void PickObjectByMouse();                   // 左クリックした物体を選ぶ。何もないところなら選択を外す
    void ResetCamera();                         // カメラを最初の位置に戻し、原点を向かせる
    DirectX::XMVECTOR GetCameraForward() const; // カメラが向いている方向（長さ1）

    bool OnInit();
    void OnTerm();

    // OnInit から順番に呼ぶ初期化処理。App_OnInit.cpp
    bool LoadMeshData(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, std::wstring& texturePath);  // 1. メッシュの読み込み
    void AppendModel(const std::vector<Mesh>& meshes, std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices,
                     const std::string& name, bool keepTriangles);                                            // 1-4. モデルをバッファ用の配列に追加し、名前と囲む箱を m_Models に記録する
    bool CreateVertexBuffer(const std::vector<MeshVertex>& vertices);                      // 2. 頂点バッファ
    bool CreateIndexBuffer(const std::vector<uint32_t>& indices);                          // 3. インデックスバッファ
    bool CreateCbvSrvHeap();                                                               // 4. CBV/SRV 用のディスクリプタヒープ
    bool CreateConstantBuffers();                                                          // 5. 定数バッファ：変換行列・ライト
    bool LoadTexture(const std::wstring& modelTexturePath);                                // 6. テクスチャと SRV
    bool CreateRootSignature();                                                            // 7. ルートシグニチャ
    bool CreateDepthBuffer();                                                              // 8. 深度バッファ
    bool CreatePipelineState();                                                            // 9. パイプラインステート
    bool CreateSkyPipeline();                                                              // 9-11. 背景の空のパイプラインステートと定数バッファ
    void InitViewport();                                                                   // 10. ビューポートとシザー矩形

    // テクスチャの読み込み。App_Texture.cpp
    HRESULT LoadTextureResource(const std::wstring& path, UINT slot, ComPtr<ID3D12Resource>& texture);  // 1枚読み込み、ヒープの slot 番目に SRV を作る
    HRESULT CreateBuiltinTexture(UINT slot, ComPtr<ID3D12Resource>& texture);                         // 組み込みの白いテクスチャをコードで作り、slot 番目に SRV を作る
    void    CreateTextureSrv(ID3D12Resource* texture, UINT slot);                                     // テクスチャの SRV をヒープの slot 番目に作る
    std::vector<std::wstring> FindTextureFiles(const std::wstring& folder) const;                     // フォルダの中の画像ファイルを一覧にする。folder は最後に \ が付く
    bool LoadAllTextures(const std::wstring& modelTexturePath); // 0番：組み込みの白 → res\texture の画像 → モデルの指定画像、の順に全部読み込む
    int  FindTextureByName(const std::string& name) const;      // ファイル名からテクスチャの番号を探す。無ければ -1

    // GPU リソース作成の共通処理。App_Helper.cpp
    HRESULT CreateUploadBuffer(UINT64 size, const void* pInitData, ComPtr<ID3D12Resource>& pBuffer);
    std::wstring FindResDirectory() const;      // res フォルダのパスを返す。最後に \ が付く。見つからなければ空
    HRESULT CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE type, UINT count, D3D12_DESCRIPTOR_HEAP_FLAGS flags, ComPtr<ID3D12DescriptorHeap>& pHeap);

    bool InitImGui();   // InitD3D のあとに行う
    void TermImGui();   // TermD3D の前に行う
    void DrawImGui();   // ImGui per-frame UI and draw

    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp);


    // -----------------------------------------------------------------------------
    //ゲームオブジェクト
    struct GameObject
    {
        DirectX::XMFLOAT3 position = { 0, 0, 0 };
        DirectX::XMFLOAT3 rotation = { 0, 0, 0 };
        DirectX::XMFLOAT3 scale    = { 1, 1, 1 };
        DirectX::XMFLOAT4 color = { 1, 1, 1, 1 };
        int model = Model_Sphere;       // どのモデルで描くか。ModelId の番号
        int texture = -1;               // どのテクスチャを使うか。m_Textures の番号。-1 なら既定のテクスチャ（AddObject で決まる）
        int role = Role_None;           // 役割。ObjectRole の番号
        bool active = true;             // false なら、取ったアイテムなので描かない・判定しない。プレイ中だけ使い、保存しない
        int  group = 0;                 // グループ番号。アイテムとゲートで使う。同じ番号のアイテムを全部取ると、同じ番号のゲートが開く。0 はどこにも属さない
        float gateBaseY = 0.0f;         // ゲートの閉じているときの高さ。プレイ開始時に覚える（プレイ中だけ使い、保存しない）
        float gateTime  = -1.0f;        // ゲートが開き始めてからの時間（秒）。負なら閉じている（プレイ中だけ使い、保存しない）

        // 円周に沿って回る（Orbit）。置いた位置が円の上のスタート地点で、半径は「置いた位置と中心の距離」
        bool                orbit       = false;            // true ならプレイ中に回る
        DirectX::XMFLOAT3   orbitCenter = { 0, 0, 0 };      // 円の中心
        float               orbitSpeed  = 90.0f;            // 1秒に何度回るか。マイナスで逆回り
        int                 orbitAxis   = 0;                // 回る軸。0 = Y（水平な円）、1 = X、2 = Z（垂直な円）
        DirectX::XMFLOAT3   orbitOffset = { 0, 0, 0 };      // 中心から見たスタート地点。プレイ開始時に覚える（保存しない）

        // その場で回る（Spin）。プレイ中、Rotation の値に「Speed × 経過時間」を足していく
        bool                spin        = false;            // true ならプレイ中にその場で回る
        float               spinSpeed   = 90.0f;            // 1秒に何度回るか。マイナスで逆回り
        int                 spinAxis    = 0;                // 回る軸。0 = Y（横に回る）、1 = X、2 = Z（縦に回る）

        // 見た目（Material）
        DirectX::XMFLOAT2   uvScale     = { 1, 1 };         // テクスチャを何回くり返して貼るか
        DirectX::XMFLOAT2   uvScroll    = { 0, 0 };         // 1秒にテクスチャをずらす量。流れる溶岩などに使う
        float               emissive    = 0.0f;             // 自分で光る強さ。ライトが当たらない面でも明るく見える
    };

    std::vector<GameObject> m_Objects;
    // Inspectorで編集中のオブジェクト番号
    int m_SelectedObject = -1;      // -1 なら何も選んでいない

    // 物体を1個追加する。MaxObjects を超えるときは追加せず false を返す。App_OnInit.cpp
    // ※ GameObject の定義より後ろで宣言する必要がある
    bool AddObject(const GameObject& object);

    // 物体の追加・複製・削除の予約。Inspector のボタンでは予約だけして、次のフレームの Update の最初に実行する
    // ※ DrawImGui の中では m_Objects の要素への参照を持っているので、その場で push_back・erase すると参照が壊れるため
    enum EditRequest
    {
        Edit_None = 0,      // 予約なし
        Edit_AddSphere,     // 球を追加
        Edit_AddBox,        // 箱を追加
        Edit_Duplicate,     // 選択中の物体を複製
        Edit_Delete,        // 選択中の物体を削除
        Edit_Load,          // ファイルから配置を読み直す
    };
    EditRequest m_PendingEdit = Edit_None;
    bool        m_EditLimitReached = false;     // 上限で追加できなかったとき true。Inspector に表示する

    void ApplyPendingEdit();                                                // 予約された編集を実行する。App_Loop.cpp
    DirectX::XMFLOAT3 GetPositionNextToSelected(const DirectX::XMFLOAT3& newScale) const;  // 選択中の物体の隣（+x側）の位置

    // エディタモードとプレイモードの切り替え。App_Loop.cpp
    // プレイ開始時に配置などを丸ごとコピーして取っておき、終了時に元に戻す（遊んでもステージが壊れないように）
    bool                    m_Playing = false;          // プレイ中なら true
    int                     m_PlayerIndex = -1;         // プレイヤー（役割が Player の物体）の番号
    std::string             m_PlayMessage;              // Inspector に表示する。"No player" など（英語）
    std::vector<GameObject> m_EditSnapshot;             // プレイ開始前の配置
    int                     m_SnapshotSelected = -1;    // プレイ開始前の選択番号
    DirectX::XMFLOAT3       m_SnapshotCameraPos = { 0, 0, 0 };  // プレイ開始前のカメラの位置
    float                   m_SnapshotCameraYaw = 0.0f;         // プレイ開始前のカメラの向き（左右）
    float                   m_SnapshotCameraPitch = 0.0f;       // プレイ開始前のカメラの向き（上下）

    // ボールの物理（Step 6）。プレイ中だけ使う
    DirectX::XMFLOAT3       m_BallVel = { 0, 0, 0 };            // ボールの速度。StartPlay・StopPlay で 0 に戻す
    DirectX::XMFLOAT4       m_BallOrientation = { 0, 0, 0, 1 }; // ボールの向き（クォータニオン）。転がって見える回転に使う
    float                   m_PlayAccel    = 10.0f;             // 加速の強さ。Inspector で調整できる
    float                   m_PlayFriction = 1.5f;              // 摩擦。大きいほどすぐ止まる
    float                   m_PlayMaxSpeed = 8.0f;              // 最高速度（1秒に進む距離）
    void UpdateBall(float dt);          // WASD でボールを転がす。カメラの向きが「前」。Space でジャンプ

    // ジャンプと重力。地面は「ボールの真下にある箱の上面」
    float                   m_BallVelY      = 0.0f;     // 上下の速度。上が正
    bool                    m_OnGround      = false;    // 地面に乗っていれば true。ジャンプできるのはこのときだけ
    bool                    m_PrevSpace     = false;    // 前のフレームで Space を押していたか。「押した瞬間」を見分けるため
    float                   m_PlayJumpSpeed = 8.0f;     // ジャンプで飛び出す速さ
    float                   m_PlayGravity   = 20.0f;    // 重力の強さ
    DirectX::XMFLOAT3       m_RespawnPos    = { 0, 0, 0 };  // 落ちたときに戻る場所。プレイ開始時のボールの位置
    float                   m_KillY         = -20.0f;   // これより下に落ちたらスタート地点に戻す
    float                   m_CameraRefY    = 0.0f;     // 追従カメラが基準にする高さ。着地したときだけ更新する（ジャンプで上下しないように）
    // (x, fromY, z) から真下にある、役割が Wall の物体の一番高い面を探す。groundNormal にはその面の向き（上向き）が入る
    bool FindGroundHeight(float x, float z, float fromY, float& groundY, DirectX::XMFLOAT3* groundNormal = nullptr) const;
    DirectX::XMFLOAT3       m_GroundNormal  = { 0, 1, 0 };  // 乗っている面の向き。坂を転がり落ちる加速に使う
    float                   m_PlayMaxSlope  = 45.0f;        // 上れる坂の最大の傾き（度）。これより急な面は壁として扱う

    // 壁との横の当たり判定（Step 8）
    float                   m_PlayBounce    = 0.3f;     // 壁にぶつかったときの跳ね返りの強さ。0 で沿って滑る、1 で同じ速さで跳ね返る
    void ResolveWallCollisions(DirectX::XMFLOAT3& pos, float radius, float stepHeight);  // Wall にめり込んでいたら押し戻し、壁に向かう速度を消す
    // ボール（中心 p・半径 radius）が物体にめり込んでいれば true。押し戻す向き n・量 push・物体の上面の高さ topY を返す
    // 壁の当たり判定とアイテムの判定の両方で使う
    bool ComputeContact(const GameObject& object, const DirectX::XMFLOAT3& p, float radius,
                        DirectX::XMFLOAT3& n, float& push, float& topY) const;

    // アイテム（Step 8・9）。触れたら消えて、取った数が増える
    int                     m_ItemCount = 0;            // 取ったアイテムの数
    int                     m_ItemTotal = 0;            // ステージにあるアイテムの数（プレイ開始時に数える）
    void CollectItems();                                // ボールが触れたアイテムを取る
    void UpdateGates(float dt);                         // 同じグループのアイテムを全部取ったゲートを、下へ沈めて開く
    void UpdateOrbits();                                // Orbit が ON の物体を、中心の周りに回す（プレイ時間から角度を決める）
    void UpdateSpins(float dt);                         // Spin が ON の物体を、その場で回す
    static bool IsSolidRole(int role) { return role == Role_Wall || role == Role_Gate; }   // ぶつかる・乗れる役割か
    static constexpr float  GateOpenSeconds = 1.5f;     // ゲートが沈みきるまでの時間
    // ダメージと戻る場所（Respawn）
    struct SafePoint
    {
        DirectX::XMFLOAT3   pos;        // ボールの位置
        float               time;       // 記録したときのプレイ時間（秒）
    };
    std::vector<SafePoint>  m_SafePoints;               // 床に乗っている間、0.5 秒ごとに記録した安全な位置（新しいものが後ろ）
    float                   m_PlayTime = 0.0f;          // プレイを始めてからの時間（秒）
    float                   m_SafeRecordTimer = 0.0f;   // 次に安全な位置を記録するまでの時間を測る
    float                   m_RespawnMessageTime = 0.0f;// 画面の真ん中に "Respawn" をあと何秒表示するか
    void RecordSafePoint(float dt);                     // 床に乗っていれば、0.5 秒ごとに今の位置を記録する
    DirectX::XMFLOAT3 GetSafeRespawnPoint() const;      // 1 秒以上前に記録した、一番新しい安全な位置
    void RespawnBall(const DirectX::XMFLOAT3& pos);     // ボールを pos に戻し、速度を 0 にしてカメラも戻す
    bool CheckDamage();                                 // Damage に触れていれば、安全な位置へ戻して true

    // ゴール
    bool                    m_Goaled    = false;        // ゴールしたら true。ボールを止め、Enter でやり直せるようにする
    float                   m_ClearTime = 0.0f;         // ゴールしたときのプレイ時間（秒）
    bool                    m_PrevEnter = false;        // 前のフレームで Enter を押していたか。「押した瞬間」を見分けるため
    void CheckGoal();                                   // Goal に触れていれば、ゴールにする

    std::string             m_HudMessage;               // HUD に一時的に出す文字。"Gate opened!" など
    float                   m_HudMessageTime = 0.0f;    // m_HudMessage をあと何秒表示するか
    void DrawHud();                                     // プレイ中、ゲーム画面の上に取った数などを表示する

    // ゲームモード（--editor なしで起動したとき）とタイトル画面
    bool                    m_EditorMode  = false;      // true ならエディタ、false ならゲーム（Inspector なし、自動保存なし）
    bool                    m_OnTitle     = false;      // ゲームモードでタイトル画面を出している間 true
    float                   m_TitleTime   = 0.0f;       // タイトル画面を出してからの時間（カメラを回す・文字を点滅させる）
    DirectX::XMFLOAT3       m_TitleCenter = { 0, 0, 0 };// タイトル画面でカメラが回る中心（ステージの真ん中）
    float                   m_TitleRadius = 15.0f;      // タイトル画面でカメラが回る半径（ステージ全体が見える距離）
    ImFont*                 m_UiFont      = nullptr;    // タイトルと HUD の文字に使うフォント（Windows の Segoe UI）。無ければ nullptr
    bool PrepareGameMode();                             // ゲームモードの準備：ステージを読み込み、プレイヤーがいるか確かめる
    void SetupTitleCamera();                            // ステージ全体を囲む範囲から、タイトル画面のカメラの中心と半径を決める
    void UpdateTitle(float dt);                         // タイトル画面：カメラを回し、Enter でプレイ、Esc で終了
    void DrawTitle();                                   // タイトル画面の文字を描く
    void QuitGame();                                    // ゲームを終了する（ウィンドウを閉じる）
    int  m_InspectorCollapse = 0;                       // 次のフレームで Inspector を 1 = 折りたたむ、2 = 開く。0 なら何もしない

    // 追従カメラ（Step 7）。プレイ中、ボールから決まった分だけずらした位置から、決まった向きでボールを見る
    bool                    m_FollowCamera   = true;                // false ならプレイ中はカメラが止まったまま
    DirectX::XMFLOAT3       m_FollowOffset   = { 0.0f, 6.0f, 12.0f }; // ボールからカメラまでのずらし量。上に6、後ろ（+Z）に12
    float                   m_FollowHeight   = 1.0f;                // 注視点の高さ。ボールの中心からどれだけ上を見るか
    float                   m_FollowSmooth   = 8.0f;                // 追いつく速さ。大きいほどすぐ追いつく。0 なら遅れなし
    void UpdateFollowCamera(float dt, bool snap);       // カメラの位置をボールに合わせる。snap が true なら遅れなしで一気に合わせる

    bool StartPlay();                   // プレイを始める。プレイヤーがいなければ始めず false
    void StopPlay();                    // プレイを終えて、開始前の状態に戻す
    void UpdateEditor(float dt);        // エディタ中の毎フレームの処理（選択・移動・カメラ・追加削除）
    void UpdatePlay(float dt);          // プレイ中の毎フレームの処理（ゲームのルール）

    // 配置（シーン）の保存・読み込み。App_Scene.cpp
    std::wstring    m_ScenePath;                // 保存先：res\stage\stage1.txt のフルパス
    bool            m_SceneReady = false;       // OnInit が最後まで成功したら true。true のときだけ終了時に自動保存する
    std::string     m_SceneMessage;             // Inspector に表示する結果。"Saved" など（英語）

    bool FindScenePath();       // 保存先の場所を決めて m_ScenePath に入れる。stage フォルダが無ければ作る
    void CreateDefaultScene();  // ファイルが無いときの初期配置：球5個＋床
    bool SaveScene();           // m_Objects をファイルに書き出す。先に今のファイルを .bak にコピーする
    bool LoadScene();           // ファイルを読んで m_Objects を作り直す。失敗したら今の配置はそのまま

};

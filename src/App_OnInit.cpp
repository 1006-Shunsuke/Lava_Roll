//-----------------------------------------------------------------------------
// File : App_OnInit.cpp
// Desc : 描画に使うGPUリソースの初期化。OnInit と、そこから順番に呼ぶ関数
//-----------------------------------------------------------------------------
#include "App.h"
#include <cstdio>
#include <string>
#include <cfloat>   // FLT_MAX

using namespace DirectX;


namespace
{
    // Shaderパスの受け取り
    std::wstring GetExeDirectory()
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring s(path);
        return s.substr(0, s.find_last_of(L"\\/") + 1);
    }

    // ファイルを探す。作業フォルダ → exeのフォルダ → その親 … の順に調べる
    // VSから実行すると作業フォルダは project\ 、exe直接なら x64\Debug\ になるため
    bool SearchFilePath(const wchar_t* filename, std::wstring& result)
    {
        auto exists = [](const std::wstring& path)
        {
            DWORD attr = GetFileAttributesW(path.c_str());
            return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
        };
        if (exists(filename))
        {
            result = filename;
            return true;
        }

        std::wstring dir = GetExeDirectory();
        for (int i = 0; i < 5; ++i)   // 最大5階層上まで
        {
            if (exists(dir + filename))
            {
                result = dir + filename;
                return true;
            }
            dir += L"..\\";
        }
        return false;
    }
}

//-----------------------------------------------------------------------------
// 箱のメッシュをコードで作る。1×1×1 で中心が原点
// 面ごとに法線が違うので、角の頂点を共有せずに 1面4頂点 × 6面 = 24頂点 にする
// （8頂点で共有すると、法線が平均されて角が丸く見えてしまう）
//-----------------------------------------------------------------------------
namespace
{
    Mesh CreateBoxMesh()
    {
        // 各面の向き。外から面を見たときの「法線」「右向き」「上向き」
        struct Face { XMFLOAT3 n, right, up; };
        const Face faces[6] =
        {
            { { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },   // 手前 (+Z)
            { { 0, 0,-1 }, {-1, 0, 0 }, { 0, 1, 0 } },   // 奥   (-Z)
            { { 1, 0, 0 }, { 0, 0,-1 }, { 0, 1, 0 } },   // 右   (+X)
            { {-1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },   // 左   (-X)
            { { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0,-1 } },   // 上   (+Y)
            { { 0,-1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },   // 下   (-Y)
        };

        // 4つの角の「右・上」方向のずらし量と、そこに貼るUV
        // 左下 → 左上 → 右上 → 右下 の順
        const float     sx[4] = { -0.5f, -0.5f,  0.5f,  0.5f };
        const float     sy[4] = { -0.5f,  0.5f,  0.5f, -0.5f };
        const XMFLOAT2  uv[4] = { { 0, 1 }, { 0, 0 }, { 1, 0 }, { 1, 1 } };

        Mesh box;
        box.MaterialId = 0;
        for (const auto& f : faces)
        {
            const auto start = static_cast<uint32_t>(box.Vertices.size());   // この面の最初の頂点番号

            for (int k = 0; k < 4; ++k)
            {
                // 角の位置 = 面の中心（法線 × 0.5） + 右 × sx + 上 × sy
                XMFLOAT3 pos(
                    f.n.x * 0.5f + f.right.x * sx[k] + f.up.x * sy[k],
                    f.n.y * 0.5f + f.right.y * sx[k] + f.up.y * sy[k],
                    f.n.z * 0.5f + f.right.z * sx[k] + f.up.z * sy[k]);

                box.Vertices.emplace_back(pos, f.n, uv[k], f.right);   // 接線は「右向き」
            }

            // 三角形2枚で四角形1枚：左下・左上・右上 と 左下・右上・右下
            const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
            for (auto i : idx)
            {
                box.Indices.push_back(start + i);
            }
        }
        return box;
    }
}

namespace
{
    // どこで失敗したかと HRESULT を表示する
    void ShowError(const wchar_t* where, HRESULT hr)
    {
        wchar_t buf[256];
        swprintf_s(buf, L"%s で失敗\nhr = 0x%08X", where, static_cast<unsigned>(hr));
        MessageBoxW(nullptr, buf, L"エラー", MB_OK);
    }

    // assimp が返す UTF-8 のパスを wchar_t の文字列に変換する
    std::wstring ToWide(const std::string& value)
    {
        if (value.empty())
        {
            return std::wstring();
        }

        // 1回目: 変換後に何文字必要かを調べる。末尾の '\0' を含む
        const int length = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
        if (length <= 0)
        {
            return std::wstring();
        }

        // 2回目: 確保した領域に実際に変換する
        std::wstring result(length, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, &result[0], length);
        result.resize(length - 1);  // 末尾の '\0' の分を取り除く
        return result;
    }

    // パスからフォルダ部分を取り出す。"res\\model\\a.obj" なら "res\\model\\"
    std::wstring GetDirectoryPath(const std::wstring& path)
    {
        const auto pos = path.find_last_of(L"\\/");
        return (pos == std::wstring::npos) ? std::wstring() : path.substr(0, pos + 1);
    }

    // 絶対パスかどうか。"C:\\..." や "\\..." で始まるものが対象
    bool IsAbsolutePath(const std::wstring& path)
    {
        return (path.size() >= 2 && path[1] == L':') || (!path.empty() && (path[0] == L'\\' || path[0] == L'/'));
    }
}

//--------------------------------------------------------
//   「描画に必要なGPUリソースを一通り作る」初期化関数
//--------------------------------------------------------
// 
// 描画に必要なデータは「頂点情報」→「インデックス情報」→「定数バッファ情報」→「ルートシグニチャ情報」→「パイプラインステート情報」の順に作る必要がある
// データ自体は最初頂点データがCPUにあり、それをシェーダーが扱えるようにするために、頂点データ本体はGPU側が読み取れるような「リソース」,そのリソースの使い方として「ディスクリプタ」を作る必要がある
// そのリソース自体は「ヒープ」というGPUがアクセスできるメモリ上の「空の入れ物」に入れる必要がある.「ディスクリプタ」もディスクリプタヒープという空の配列に入れる必要がある


//-----------------------------------------------------------------------------
// 初期化時の処理
// 描画に必要な GPU リソースを、下の 1〜10 の順番に作る
//-----------------------------------------------------------------------------
bool App::OnInit()
{
    // どれかが失敗したら、そこで中断する
    std::vector<MeshVertex> vertices;       // 全メッシュの頂点。1 で読み込み、2 で使う
    std::vector<uint32_t>   indices;        // 全メッシュのインデックス。1 で読み込み、3 で使う
    std::wstring            texturePath;    // マテリアルに書かれたテクスチャ。1 で決め、6 で使う

    if (!LoadMeshData(vertices, indices, texturePath))   { return false; }   // 1. メッシュの読み込み
    if (!CreateVertexBuffer(vertices))                   { return false; }   // 2. 頂点バッファ
    if (!CreateIndexBuffer(indices))                     { return false; }   // 3. インデックスバッファ
    if (!CreateCbvSrvHeap())                             { return false; }   // 4. CBV/SRV 用のディスクリプタヒープ
    if (!CreateConstantBuffers())                        { return false; }   // 5. 定数バッファ：変換行列・ライト
    if (!LoadTexture(texturePath))                       { return false; }   // 6. テクスチャと SRV
    if (!CreateRootSignature())                          { return false; }   // 7. ルートシグニチャ
    if (!CreateDepthBuffer())                            { return false; }   // 8. 深度バッファ
    if (!CreatePipelineState())                          { return false; }   // 9. パイプラインステート
    if (!CreateSkyPipeline())                            { return false; }   // 9-11. 背景の空
    InitViewport();                                                          // 10. ビューポートとシザー矩形

    // 11. 物体の配置
    FindScenePath();
    if (!m_EditorMode)
    {
        // ゲームモード：ステージを読み込み、タイトル画面から始める。終了時の自動保存はしない（m_SceneReady は false のまま）
        return PrepareGameMode();
    }

    // エディタ：保存ファイルがあれば読み込み、無い・読めないときは初期配置を作る
    if (!LoadScene())
    {
        CreateDefaultScene();
    }

    // ここまで来たら初期化は成功。終了時の自動保存を許可する
    m_SceneReady = true;

    return true;
}

//-----------------------------------------------------------------------------
// 物体を1個追加する
// 定数バッファは MaxObjects 個分しか確保していないので、それを超えて追加しない
// 超えたときは Visual Studio の「出力」ウィンドウに警告を出して false を返す
//-----------------------------------------------------------------------------
bool App::AddObject(const GameObject& object)
{
    if (m_Objects.size() >= MaxObjects)
    {
        OutputDebugStringW(L"MaxObjectsを超えるので追加できません\n");
        return false;
    }

    m_Objects.push_back(object);

    // テクスチャが未指定（-1）や範囲外なら、既定のテクスチャにする
    GameObject& added = m_Objects.back();
    if (added.texture < 0 || added.texture >= static_cast<int>(m_Textures.size()))
    {
        added.texture = m_DefaultTexture;
    }
    return true;
}

//-----------------------------------------------------------------------------
// 1. メッシュファイルの読み込み
// assimp でモデルファイルを読み、全メッシュの頂点・インデックスを1つの配列にまとめる
// この vertices / indices を、2. 頂点バッファ・3. インデックスバッファにコピーする
//-----------------------------------------------------------------------------
bool App::LoadMeshData(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, std::wstring& texturePath)
{
    // 1-1 ファイル探索
    std::wstring meshPath;
    if (!SearchFilePath(L"res\\model\\sphere.obj", meshPath))
    {
        MessageBoxW(nullptr, L"res\\model\\sphere.obj が見つかりません", L"メッシュ読み込み失敗", MB_OK);
        return false;
    }

    // 1-2 読み込み
    std::vector<Mesh>     meshes;
    std::vector<Material> materials;    // テクスチャのパスを取り出すのに使う
    if (!LoadMesh(meshPath.c_str(), meshes, materials))
    {
        // 失敗の理由は Visual Studio の「出力」ウィンドウに [LoadMesh] として出る
        MessageBoxW(nullptr, meshPath.c_str(), L"メッシュ読み込み失敗", MB_OK);
        return false;
    }

    // 1-3 マテリアルからテクスチャのパスを決める
    // .mtl の map_Kd に書かれたファイル名を、モデルと同じフォルダからの相対パスとして扱う
    texturePath.clear();
    for (const auto& material : materials)
    {
        // テクスチャの指定が無いマテリアルと、'*' で始まる埋め込みテクスチャは飛ばす。埋め込みは glTF などで使うが今は非対応
        if (material.DiffuseMap.empty() || material.DiffuseMap[0] == '*')
        {
            continue;
        }

        const auto name = ToWide(material.DiffuseMap);
        texturePath = IsAbsolutePath(name) ? name : GetDirectoryPath(meshPath) + name;
        break;  // 今はテクスチャを1枚しか使えないので、最初に見つかったものを使う
    }

    // 1-4 モデルを順番に配列へ追加する。この順番が ModelId の番号になる
    m_DrawRanges.clear();
    m_Models.clear();
    AppendModel(meshes, vertices, indices, "Sphere", false);            // 0番 Model_Sphere：sphere.obj（当たり判定は球の計算）
    AppendModel({ CreateBoxMesh() }, vertices, indices, "Box", false);  // 1番 Model_Box   ：コードで作った箱（当たり判定は箱の計算）

    // 1-5 res\model フォルダにある残りのモデルを全部読み込む（2番から）。読めないファイルは飛ばす
    const std::wstring resDir = FindResDirectory();
    if (!resDir.empty())
    {
        const std::wstring modelDir = resDir + L"model\\";
        WIN32_FIND_DATAW data = {};
        HANDLE hFind = FindFirstFileW((modelDir + L"*").c_str(), &data);
        if (hFind != INVALID_HANDLE_VALUE)
        {
            do
            {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    continue;   // フォルダは飛ばす
                }

                // 読み込む形式か調べる。大文字・小文字は区別しない
                const std::wstring fileName = data.cFileName;
                const auto dot = fileName.find_last_of(L'.');
                const std::wstring ext = (dot == std::wstring::npos) ? L"" : fileName.substr(dot);
                const bool isModel = _wcsicmp(ext.c_str(), L".obj") == 0 || _wcsicmp(ext.c_str(), L".gltf") == 0 ||
                                     _wcsicmp(ext.c_str(), L".glb") == 0 || _wcsicmp(ext.c_str(), L".fbx") == 0;
                if (!isModel || _wcsicmp(fileName.c_str(), L"sphere.obj") == 0)
                {
                    continue;   // モデルではない、または 0番で読んだ sphere.obj
                }

                // 名前に空白があると、保存ファイルで区切りと区別できないので読まない
                if (fileName.find(L' ') != std::wstring::npos)
                {
                    OutputDebugStringW((L"[LoadMeshData] 名前に空白があるので読みません: " + fileName + L"\n").c_str());
                    continue;
                }

                std::vector<Mesh>     extraMeshes;
                std::vector<Material> extraMaterials;   // 今は使わない（テクスチャは Inspector で選ぶ）
                if (!LoadMesh((modelDir + fileName).c_str(), extraMeshes, extraMaterials) || extraMeshes.empty())
                {
                    OutputDebugStringW((L"[LoadMeshData] 読み込めないので飛ばします: " + fileName + L"\n").c_str());
                    continue;
                }

                // ファイル名を UTF-8 にして名前にする
                const int len = WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, nullptr, 0, nullptr, nullptr);
                std::string name(len > 0 ? len - 1 : 0, '\0');
                if (len > 1)
                {
                    WideCharToMultiByte(CP_UTF8, 0, fileName.c_str(), -1, &name[0], len, nullptr, nullptr);
                }

                AppendModel(extraMeshes, vertices, indices, name, true);    // 当たり判定は三角形で行う
                OutputDebugStringW((L"[LoadMeshData] モデルを読み込みました: " + fileName + L"\n").c_str());
            } while (FindNextFileW(hFind, &data));
            FindClose(hFind);
        }
    }

    if (vertices.empty() || indices.empty() || m_Models.size() < Model_BuiltinCount)
    {
        MessageBoxW(nullptr, L"メッシュに三角形がありません", L"メッシュ読み込み失敗", MB_OK);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// 1-4. モデル1個分（部品が複数あってもよい）を、全モデル共通の配列の後ろに追加する
// 部品ごとの描画範囲を m_DrawRanges に、モデル全体の範囲を m_Models に記録する
//-----------------------------------------------------------------------------
void App::AppendModel(const std::vector<Mesh>& meshes, std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices,
                      const std::string& name, bool keepTriangles)
{
    ModelRange model = {};
    model.first = static_cast<UINT>(m_DrawRanges.size());   // このモデルの最初の部品は、今の m_DrawRanges の末尾
    model.count = static_cast<UINT>(meshes.size());         // 部品の数
    model.name  = name;

    // モデルをぴったり囲む箱：全部の頂点の x・y・z の最小と最大
    model.boundsMin = XMFLOAT3( FLT_MAX,  FLT_MAX,  FLT_MAX);
    model.boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const auto& mesh : meshes)
    {
        for (const auto& v : mesh.Vertices)
        {
            model.boundsMin.x = (std::min)(model.boundsMin.x, v.Position.x);
            model.boundsMin.y = (std::min)(model.boundsMin.y, v.Position.y);
            model.boundsMin.z = (std::min)(model.boundsMin.z, v.Position.z);
            model.boundsMax.x = (std::max)(model.boundsMax.x, v.Position.x);
            model.boundsMax.y = (std::max)(model.boundsMax.y, v.Position.y);
            model.boundsMax.z = (std::max)(model.boundsMax.z, v.Position.z);
        }
    }
    // 三角形での当たり判定用に、頂点の位置とインデックスを CPU 側にも残しておく
    // 部品ごとに頂点の番号が 0 から始まるので、ここまでに入れた頂点の数を足してつなげる
    model.useTriangles = keepTriangles;
    if (keepTriangles)
    {
        for (const auto& mesh : meshes)
        {
            const auto base = static_cast<uint32_t>(model.triPositions.size());
            for (const auto& v : mesh.Vertices)
            {
                model.triPositions.push_back(v.Position);
            }
            for (const auto i : mesh.Indices)
            {
                model.triIndices.push_back(base + i);
            }
        }
    }

    if (model.boundsMin.x > model.boundsMax.x)      // 頂点が1つもないときは 1×1×1 の箱にしておく
    {
        model.boundsMin = XMFLOAT3(-0.5f, -0.5f, -0.5f);
        model.boundsMax = XMFLOAT3( 0.5f,  0.5f,  0.5f);
    }

    for (const auto& mesh : meshes)
    {
        DrawRange range = {};
        range.IndexCount = static_cast<UINT>(mesh.Indices.size());
        range.StartIndex = static_cast<UINT>(indices.size());   // ここまでに入れたインデックスの数 = このメッシュの開始位置
        range.BaseVertex = static_cast<INT>(vertices.size());   // ここまでに入れた頂点の数 = このメッシュの頂点の開始位置
        m_DrawRanges.push_back(range);

        // 配列の後ろに追加していく
        vertices.insert(vertices.end(), mesh.Vertices.begin(), mesh.Vertices.end());
        indices.insert(indices.end(), mesh.Indices.begin(), mesh.Indices.end());
    }

    m_Models.push_back(model);
}

//-----------------------------------------------------------------------------
// 2. 頂点バッファの生成：頂点情報を入れるリソース
// 本来頂点情報はCPUにあるが、GPUが扱えるようにするために、GPU側が読み取れるような「リソース」を作る必要がある
// 頂点シェーダの入力データに合うように頂点バッファを作成する。
// GPUが読むとき、必要な情報は「アドレス・サイズ・ストライド」のみであるため、ディスクリプタは不要で「ビュー」と呼ばれる構造体を作るだけでよい
// 頂点データをGPU側が扱える頂点バッファの中に入れてGPUへの渡し方まで用意
// ここではGPU側では何の処理も行わない
//-----------------------------------------------------------------------------
bool App::CreateVertexBuffer(const std::vector<MeshVertex>& vertices)
{
    // 頂点データの中身は 1. の LoadMeshData で読み込んだ vertices
    // バッファの大きさ = 頂点1個のサイズ × 頂点の数
    const auto vbSize = static_cast<UINT>(sizeof(MeshVertex) * vertices.size());

    // 2-1 リソースを作り、頂点データを書き込む。CreateUploadBuffer は App_Helper.cpp にある
    auto hr = CreateUploadBuffer(vbSize, vertices.data(), _vB);
    if (FAILED(hr))
    {
        ShowError(L"2-1 頂点バッファ生成", hr);
        return false;
    }

    // 2-2 View構造体を作成
    // 頂点バッファビューの設定。ただし、ビュー自体はGPU側から読めない
    m_VBV.BufferLocation = _vB->GetGPUVirtualAddress(); // GPU仮想アドレス
    m_VBV.SizeInBytes = vbSize; // 頂点バッファ全体のサイズ
    m_VBV.StrideInBytes = static_cast<UINT>(sizeof(MeshVertex)); // 1頂点あたりのサイズ

    return true;
}

//-----------------------------------------------------------------------------
// 3. インデックスバッファの生成
// 三角形を作る頂点の番号、つまりインデックスを GPU が読めるバッファに入れ、GPU への渡し方まで用意する
// 同じ頂点を何度も書かずに済むので、頂点バッファが小さくなる
//-----------------------------------------------------------------------------
bool App::CreateIndexBuffer(const std::vector<uint32_t>& indices)
{
    // インデックスデータは 1. の LoadMeshData で読み込んだ indices
    const auto ibSize = static_cast<UINT>(sizeof(uint32_t) * indices.size());

    // 3-1 リソースを作り、インデックスデータを書き込む
    auto hr = CreateUploadBuffer(ibSize, indices.data(), _iB);
    if (FAILED(hr))
    {
        ShowError(L"3-1 インデックスバッファ生成", hr);
        return false;
    }

    // 3-2 View構造体を作成
    // インデックスバッファビューの設定
    m_IBV.BufferLocation = _iB->GetGPUVirtualAddress(); // GPU仮想アドレス
    m_IBV.Format = DXGI_FORMAT_R32_UINT; // インデックス1個の型：uint32_t
    m_IBV.SizeInBytes = ibSize; // インデックスバッファ全体のサイズ

    return true;
}

//-----------------------------------------------------------------------------
// 4. 定数バッファとテクスチャ用のディスクリプタヒープの生成
// CBV_SRV_UAV のディスクリプタヒープを1つ作り、次のように並べる
//   [0]〜[MaxTextures-1] SRV : m_Textures[i]    → t0（描く物体ごとに、どれを t0 に見せるか切り替える）
// ※ b0（Transform）と b1（ライト）はルートCBVで GPU アドレスを直接渡すので、ヒープには置かない
//-----------------------------------------------------------------------------
bool App::CreateCbvSrvHeap()
{
    // 4-1 ディスクリプタヒープの生成
    // 数は SRV×MaxTextures。シェーダーから見えるヒープにする点が RTV・DSV との違い
    auto hr = CreateDescriptorHeap(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MaxTextures, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, _heapCBV_SRV_UAV);
    if (FAILED(hr))
    {
        ShowError(L"4-1 CBV_SRV_UAVヒープ生成", hr);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// 5. 定数バッファの生成：変換行列・ライト
// フレームの数だけ作り、Map したまま持ち続ける
//-----------------------------------------------------------------------------
bool App::CreateConstantBuffers()
{
    // 5-1 カメラを最初の位置・向きにする
    //     ビュー行列と射影行列は、カメラを動かせるように Render で毎フレーム作って書き込む
    ResetCamera();

    // 5-2 定数バッファをフレームの数だけ作る
    for (auto i = 0u; i < FrameCount; ++i)
    {
        // 5-2-1 リソースを作成してメモリを確保する。Transform は alignas(256) なので大きさは256の倍数
        // 中身は 5-2-3 で書くので、初期データは渡さない
        const UINT64 bufferSize = static_cast<UINT64>(sizeof(Transform)) * MaxObjects;
        auto hr = CreateUploadBuffer(bufferSize, nullptr, _cB[i]);
        if (FAILED(hr))
        {
            ShowError(L"5-2-1 定数バッファ生成", hr);
            return false;
        }

        // 5-2-2 マッピング。CPUから書き込めるポインタを持ち続ける
        hr = _cB[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_pTransform[i]));
        if (FAILED(hr))
        {
            ShowError(L"5-2-2 定数バッファMap", hr);
            return false;
        }

        // 5-2-3 変換行列を単位行列で初期化しておく。中身は Render で毎フレーム書き込む
        for (auto j = 0u; j < MaxObjects; ++j)
        {
            Transform& transform = m_pTransform[i][j];

            transform.World         = XMMatrixIdentity();
            transform.View          = XMMatrixIdentity();
            transform.Proj          = XMMatrixIdentity();
            transform.WorldInvTrans = XMMatrixIdentity();
        }
    }

    // 5-3 ライト用の定数バッファをフレームの数だけ作る
    // ルートシグニチャに GPU アドレスを直接渡すルートCBVで使うので、ヒープに CBV は作らない
    for (auto i = 0u; i < FrameCount; ++i)
    {
        // 5-3-1 リソースを作成してメモリを確保する
        // LightParam も alignas(256) なので大きさは256
        auto hr = CreateUploadBuffer(sizeof(LightParam), nullptr, _lightCB[i]);
        if (FAILED(hr))
        {
            ShowError(L"5-3-1 ライト用定数バッファ生成", hr);
            return false;
        }

        // 5-3-2 マッピング。Transform と同じく、書き込み用のポインタを持ち続ける
        hr = _lightCB[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_pLight[i]));
        if (FAILED(hr))
        {
            ShowError(L"5-3-2 ライト用定数バッファMap", hr);
            return false;
        }

        // 5-3-3 初期値は0で埋めておく。実際の値は毎フレーム Render で書く
        memset(m_pLight[i], 0, sizeof(LightParam));
    }

    return true;
}

//-----------------------------------------------------------------------------
// 6. テクスチャの読み込みと SRV の作成
// 読み込みと SRV の作成は App_Texture.cpp の LoadAllTextures で行う
//   0番 : 組み込みの白いテクスチャ "(default)"。画像ファイルが1枚もなくても必ずある
//   1番〜 : res\texture の画像と、モデルのマテリアルに指定された画像
//-----------------------------------------------------------------------------
bool App::LoadTexture(const std::wstring& modelTexturePath)
{
    // 組み込みのテクスチャが作れなかったときだけ失敗にする。画像ファイルは読めなければ飛ばす
    if (!LoadAllTextures(modelTexturePath))
    {
        MessageBoxW(nullptr, L"組み込みのテクスチャを作れませんでした", L"テクスチャ読み込み失敗", MB_OK);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// 7. ルートシグニチャの生成
// ルートシグニチャは定数バッファやテクスチャ,サンプラーといったシェーダ内で使用するリソースのレイアウトを決めるオブジェクト
//-----------------------------------------------------------------------------
bool App::CreateRootSignature()
{
    // 7-1 ルートシグニチャのフラグ
    // ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT : 頂点バッファと入力レイアウトを使う
    // DENY_xxx_SHADER_ROOT_ACCESS        : 使わないドメイン・ハル・ジオメトリシェーダーからは見えなくし、少し速くする
    auto rootSignatureFlags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    rootSignatureFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS;
    rootSignatureFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS;
    rootSignatureFlags |= D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;

    // 7-2 ルートパラメータの配列。シェーダーへの「入口」の一覧
    // [0] b0 : Transform → ディスクリプタテーブル。ヒープの CBV を指す
    // [1] t0 : テクスチャ → ディスクリプタテーブル。ヒープの SRV を指す
    // [2] b1 : ライト   → ルートCBV。定数バッファの GPU アドレスを直接渡す
    // ※ ルートパラメータの番号と register の番号は別物
    //    番号は描画時の SetGraphicsRoot○○(番号, …) で使う
    D3D12_ROOT_PARAMETER rootParameters[RootParam_Count] = {};   // 番号は App.h の RootParam

    // 7-3 [0] b0 : TransformをルートCBVで渡す
    rootParameters[RootParam_Transform].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[RootParam_Transform].Descriptor.ShaderRegister = 0; // b0
    rootParameters[RootParam_Transform].Descriptor.RegisterSpace = 0;
    rootParameters[RootParam_Transform].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    // 7-4 [1] t0 : テクスチャの SRV
    // 7-4-1 ディスクリプタレンジ : 「ヒープの中の SRV を1個、t0 として使う」
    D3D12_DESCRIPTOR_RANGE srvRange = {};
    srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;    // 範囲の中身は SRV
    srvRange.NumDescriptors = 1;                             // 1個
    srvRange.BaseShaderRegister = 0;                         // t0 から
    srvRange.RegisterSpace = 0;
    srvRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    // 7-4-2 ルートパラメータ : 上のレンジを持つディスクリプタテーブル
    // テクスチャはルートCBVのようにアドレスを直接渡せないので、テーブル経由で渡す
    rootParameters[RootParam_Texture].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParameters[RootParam_Texture].DescriptorTable.NumDescriptorRanges = 1;
    rootParameters[RootParam_Texture].DescriptorTable.pDescriptorRanges = &srvRange;
    rootParameters[RootParam_Texture].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;     // ピクセルシェーダーだけが使う

    // 7-5 [2] b1 : ライト用の定数バッファ。ルートCBVで渡す
    // ディスクリプタテーブルと違い、ヒープに CBV を作らずに「アドレスを直接」渡せる
    // 描画時は SetGraphicsRootConstantBufferView(RootParam_Light, アドレス) で渡す
    rootParameters[RootParam_Light].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParameters[RootParam_Light].Descriptor.ShaderRegister = 1;                         // b1
    rootParameters[RootParam_Light].Descriptor.RegisterSpace = 0;
    rootParameters[RootParam_Light].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;     // ピクセルシェーダーだけが使う

    // 7-6 s0 : 静的サンプラー。テクスチャの「読み方」を決める
    // ルートシグニチャに直接埋め込むので、サンプラー用ヒープは不要
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;        // 縮小・拡大・ミップ間すべて線形補間
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;      // UVが0～1の外なら繰り返す。モデルのUVははみ出すことがあるため
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    sampler.MipLODBias = D3D12_DEFAULT_MIP_LOD_BIAS;         // ミップレベルのずらし量は0
    sampler.MaxAnisotropy = 1;                               // 異方性フィルタ用。今は未使用
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;    // 比較サンプラー用。今は未使用
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK; // BORDER モード時の色。今は未使用
    sampler.MinLOD = 0.0f;                                   // 使うミップの範囲：最も細かい 0 から
    sampler.MaxLOD = D3D12_FLOAT32_MAX;                      // 最も粗いものまで
    sampler.ShaderRegister = 0;                              // s0
    sampler.RegisterSpace = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // 7-7 ルートシグニチャの設定。7-1～7-6 をまとめる
    D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
    rootSignatureDesc.NumParameters = _countof(rootParameters);                   // ルートパラメータの数は3
    rootSignatureDesc.NumStaticSamplers = 1;                              // 静的サンプラーの数
    rootSignatureDesc.pParameters = rootParameters;
    rootSignatureDesc.pStaticSamplers = &sampler;
    rootSignatureDesc.Flags = rootSignatureFlags;

    ComPtr<ID3DBlob> pSignatureBlob;         // シリアライズ結果のバイト列
    ComPtr<ID3DBlob> pErrorBlob;    // 失敗した時のエラーメッセージ

    // 7-8 シリアライズ：設定を GPU ドライバに渡せるバイト列に変換する
    auto hr = D3D12SerializeRootSignature(
        &rootSignatureDesc,
        D3D_ROOT_SIGNATURE_VERSION_1_0,
        pSignatureBlob.GetAddressOf(),
        pErrorBlob.GetAddressOf());
    if (FAILED(hr))
    {
        ShowError(L"7-8 ルートシグニチャ シリアライズ", hr);
        return false;
    }

    // 7-9 ルートシグニチャの生成：バイト列からオブジェクトを作る
    hr = _device->CreateRootSignature(
        0,                              // ノードマスク。GPU1枚なので0
        pSignatureBlob->GetBufferPointer(),
        pSignatureBlob->GetBufferSize(),
        IID_PPV_ARGS(_rootSignature.GetAddressOf()));
    if (FAILED(hr))
    {
        ShowError(L"7-9 ルートシグニチャ生成", hr);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// 8. 深度バッファの生成
//-----------------------------------------------------------------------------
bool App::CreateDepthBuffer()
{
    // 8-1 DSV用ディスクリプタヒープ
    // 深度バッファは1枚なので、数は1
    auto hr = CreateDescriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, _heapDSV);
    if (FAILED(hr))
    {
        ShowError(L"8-1 DSVヒープ生成", hr);
        return false;
    }

    m_HandleDSV = _heapDSV->GetCPUDescriptorHandleForHeapStart(); // ハンドルはアドレスの定数値

    // 8-2〜8-4 深度バッファと DSV を作る。リサイズのときも同じ処理を使うので関数にしてある
    return CreateDepthResource();
}

//-----------------------------------------------------------------------------
// 8-2〜8-4. 今の m_Width × m_Height で深度バッファを作り、DSV をヒープに書き込む
// 起動時（CreateDepthBuffer）と、ウィンドウの大きさが変わったとき（OnResize）に呼ぶ
//-----------------------------------------------------------------------------
bool App::CreateDepthResource()
{
    constexpr DXGI_FORMAT depthFormat = DXGI_FORMAT_D32_FLOAT;

    // 8-2 GPUが深度値を保存するテクスチャ
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProps.CreationNodeMask = 1;
    heapProps.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC depthDesc = {}; // デプス画像のサイズ,形式,サンプル数
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = m_Width;
    depthDesc.Height = m_Height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = depthFormat;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE depthClearValue = {};    // 最適化クリア値
    depthClearValue.Format = depthFormat;
    depthClearValue.DepthStencil.Depth = 1.0f;
    depthClearValue.DepthStencil.Stencil = 0;

    // 8-3 リソース生成
    auto hr = _device->CreateCommittedResource(
        &heapProps,
        D3D12_HEAP_FLAG_NONE,
        &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE,
        &depthClearValue,
        IID_PPV_ARGS(_depthBuffer.ReleaseAndGetAddressOf()));
    if (FAILED(hr))
    {
        ShowError(L"8-3 深度バッファ生成", hr);
        return false;
    }

    // 8-4  ヒープ内の1スロットにDSVを書き込む
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    dsvDesc.Texture2D.MipSlice = 0;

    _device->CreateDepthStencilView(
        _depthBuffer.Get(), &dsvDesc, m_HandleDSV);

    return true;
}

//-----------------------------------------------------------------------------
// 9. パイプラインステートの生成
//-----------------------------------------------------------------------------
bool App::CreatePipelineState()
{
    // 9-0～9-2 入力レイアウト
    // Mesh.cpp で定義した MeshVertex::InputLayout を使う。位置・法線・UV・接線の4要素

    // 9-3 ラスタライザーステートの設定：ラスタライズを行うときの状態
    D3D12_RASTERIZER_DESC rasterizerDesc;
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID; // 三角形を塗りつぶし
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE; // 前面も背面も描画する
    rasterizerDesc.FrontCounterClockwise = FALSE; // 頂点の並び順が反時計回りなら前面とする
    rasterizerDesc.DepthBias = D3D12_DEFAULT_DEPTH_BIAS; // 深度値に加算する値。0でよい
    rasterizerDesc.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP; // DepthBias の最大値。0でよい
    rasterizerDesc.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS; // 深度値に加算する値の、勾配に応じたスケーリング値。0でよい
    rasterizerDesc.DepthClipEnable = FALSE; // 距離に基づくクリッピングを有効にするか。無効でよい
    rasterizerDesc.MultisampleEnable = FALSE; // マルチサンプリング
    rasterizerDesc.AntialiasedLineEnable = TRUE; // アンチエイリアス
    rasterizerDesc.ForcedSampleCount = 0;
    rasterizerDesc.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF; // 厳密判定OFF

    // 9-4 レンダーターゲットのブレンド設定
    D3D12_RENDER_TARGET_BLEND_DESC renderTargetBlendDesc = {
        FALSE, FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL
    };

    // 9-5 ブレンドステートの設定
    D3D12_BLEND_DESC blendDesc;
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        blendDesc.RenderTarget[i] = renderTargetBlendDesc;
    }

    ComPtr<ID3DBlob> pVSBlob;
    ComPtr<ID3DBlob> pPSBlob;

    // 9-6 頂点シェーダ読み込み
    auto vsPath = GetExeDirectory() + L"LightVS.cso";   // ライティング用の頂点シェーダ
    auto hr = D3DReadFileToBlob(vsPath.c_str(), pVSBlob.GetAddressOf());
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, vsPath.c_str(), L"VS読み込み失敗", MB_OK);
        return false;
    }

    // 9-7 ピクセルシェーダ読み込み
    auto psPath = GetExeDirectory() + L"LightPS.cso";   // ライティング用のピクセルシェーダ
    hr = D3DReadFileToBlob(psPath.c_str(), pPSBlob.GetAddressOf());
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, psPath.c_str(), L"PS読み込み失敗", MB_OK);
        return false;
    }
    // 9-8 深度ステンシルステートの設定
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE; // 深度テストを有効にする
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL; // テストを通過した部分の深度を書き込む
    depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL; // 保存済みの深度より小さい＝手前なら通過
    depthStencilDesc.StencilEnable = FALSE; // 今回はステンシルを使わない

    // 9-9 パイプラインステートの設定
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = MeshVertex::InputLayout;   // Mesh.cpp で定義した入力レイアウト
    psoDesc.pRootSignature = _rootSignature.Get();
    psoDesc.VS = { pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize() };
    psoDesc.PS = { pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    // 9-10 パイプラインステートを生成
    hr = _device->CreateGraphicsPipelineState(
        &psoDesc,
        IID_PPV_ARGS(_pSO.GetAddressOf()));
    if (FAILED(hr))
    {
        ShowError(L"9-10 PSO生成", hr);
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// 9-11. 背景の空のパイプラインステートと定数バッファ
// 物体を描く前に、画面全体をおおう三角形を1枚描いて、各ピクセルで空の色を計算する
// ルートシグニチャは物体と同じものを使う（b0 に空の設定、t0 にパノラマ画像）
//-----------------------------------------------------------------------------
bool App::CreateSkyPipeline()
{
    // 1. シェーダの読み込み
    ComPtr<ID3DBlob> pVSBlob;
    ComPtr<ID3DBlob> pPSBlob;
    auto vsPath = GetExeDirectory() + L"SkyVS.cso";
    auto hr = D3DReadFileToBlob(vsPath.c_str(), pVSBlob.GetAddressOf());
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, vsPath.c_str(), L"空のVS読み込み失敗", MB_OK);
        return false;
    }
    auto psPath = GetExeDirectory() + L"SkyPS.cso";
    hr = D3DReadFileToBlob(psPath.c_str(), pPSBlob.GetAddressOf());
    if (FAILED(hr))
    {
        MessageBoxW(nullptr, psPath.c_str(), L"空のPS読み込み失敗", MB_OK);
        return false;
    }

    // 2. ラスタライザー：裏表どちらも描く
    D3D12_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
    rasterizerDesc.DepthClipEnable = FALSE;

    // 3. ブレンド：なし（上書き）
    D3D12_BLEND_DESC blendDesc = {};
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
    {
        blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        blendDesc.RenderTarget[i].SrcBlend  = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlend = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOp   = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].SrcBlendAlpha  = D3D12_BLEND_ONE;
        blendDesc.RenderTarget[i].DestBlendAlpha = D3D12_BLEND_ZERO;
        blendDesc.RenderTarget[i].BlendOpAlpha   = D3D12_BLEND_OP_ADD;
        blendDesc.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
    }

    // 4. 深度：テストも書き込みもしない。物体より先に描くので、物体は必ず空の手前になる
    D3D12_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable    = FALSE;
    depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    depthStencilDesc.DepthFunc      = D3D12_COMPARISON_FUNC_ALWAYS;
    depthStencilDesc.StencilEnable  = FALSE;

    // 5. パイプラインステート。頂点バッファは使わないので、入力レイアウトは空
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = { nullptr, 0 };
    psoDesc.pRootSignature = _rootSignature.Get();
    psoDesc.VS = { pVSBlob->GetBufferPointer(), pVSBlob->GetBufferSize() };
    psoDesc.PS = { pPSBlob->GetBufferPointer(), pPSBlob->GetBufferSize() };
    psoDesc.RasterizerState = rasterizerDesc;
    psoDesc.BlendState = blendDesc;
    psoDesc.DepthStencilState = depthStencilDesc;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;     // 深度は使わないが、描くときに深度バッファが設定されているので形式はそろえる
    psoDesc.SampleDesc.Count = 1;
    psoDesc.SampleDesc.Quality = 0;

    hr = _device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(_skyPSO.GetAddressOf()));
    if (FAILED(hr))
    {
        ShowError(L"9-11 空のPSO生成", hr);
        return false;
    }

    // 6. 空の設定の定数バッファ。フレームの数だけ作り、Map したまま持ち続ける
    for (auto i = 0u; i < FrameCount; ++i)
    {
        hr = CreateUploadBuffer(sizeof(SkyParam), nullptr, _skyCB[i]);
        if (FAILED(hr))
        {
            ShowError(L"9-11 空の定数バッファ生成", hr);
            return false;
        }
        hr = _skyCB[i]->Map(0, nullptr, reinterpret_cast<void**>(&m_pSky[i]));
        if (FAILED(hr))
        {
            ShowError(L"9-11 空の定数バッファMap", hr);
            return false;
        }
        memset(m_pSky[i], 0, sizeof(SkyParam));
    }

    return true;
}

//-----------------------------------------------------------------------------
// 10. ビューポートとシザー矩形の設定
//-----------------------------------------------------------------------------
void App::InitViewport()
{
    m_Viewport.TopLeftX = 0;
    m_Viewport.TopLeftY = 0;
    m_Viewport.Width = static_cast<float>(m_Width);
    m_Viewport.Height = static_cast<float>(m_Height);
    m_Viewport.MinDepth = 0.0f;
    m_Viewport.MaxDepth = 1.0f;

    m_Scissor.left = 0;
    m_Scissor.right = m_Width;
    m_Scissor.top = 0;
    m_Scissor.bottom = m_Height;
}

//-----------------------------------------------------------------------------
// 名前からモデルの番号を探す。大文字・小文字は区別しない
//   name   : "Sphere"・"Box"・"slope.obj" など
//   戻り値 : 番号。見つからなければ -1
//-----------------------------------------------------------------------------
int App::FindModelByName(const std::string& name) const
{
    for (int i = 0; i < static_cast<int>(m_Models.size()); ++i)
    {
        if (_stricmp(m_Models[i].name.c_str(), name.c_str()) == 0)
        {
            return i;
        }
    }
    return -1;
}

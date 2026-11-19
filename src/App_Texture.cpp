//-----------------------------------------------------------------------------
// File : App_Texture.cpp
// Desc : テクスチャの読み込み。起動時に全部読み込み、物体ごとに番号で選んで使う
//-----------------------------------------------------------------------------
#include "App.h"
#include <ResourceUploadBatch.h>    // DirectXTK12：テクスチャをGPUへ転送する
#include <DDSTextureLoader.h>       // DirectXTK12：DDS画像を読み込む
#include <WICTextureLoader.h>       // DirectXTK12：PNG・JPG などを読み込む
#include <BufferHelpers.h>          // DirectXTK12：メモリ上の画像からテクスチャを作る（組み込みテクスチャ用）

namespace
{
    // 拡張子を返す。"a.png" なら ".png"、無ければ空
    std::wstring GetExtension(const std::wstring& path)
    {
        const auto pos = path.find_last_of(L'.');
        return (pos == std::wstring::npos) ? std::wstring() : path.substr(pos);
    }

    // 拡張子が .dds かどうか。大文字・小文字は区別しないので ".DDS" も OK
    bool IsDdsFile(const std::wstring& path)
    {
        return _wcsicmp(GetExtension(path).c_str(), L".dds") == 0;
    }

    // このエンジンで読み込める画像ファイルかどうか
    bool IsTextureFile(const std::wstring& path)
    {
        const wchar_t* extensions[] = { L".dds", L".png", L".jpg", L".jpeg", L".bmp" };
        const auto ext = GetExtension(path);
        for (const auto* e : extensions)
        {
            if (_wcsicmp(ext.c_str(), e) == 0)
            {
                return true;
            }
        }
        return false;
    }

    // wchar_t の文字列を UTF-8 に変換する。ImGui は UTF-8 の文字列を表示するため
    std::string ToUtf8(const std::wstring& value)
    {
        if (value.empty())
        {
            return std::string();
        }

        // 1回目: 変換後に何バイト必要かを調べる。末尾の '\0' を含む
        const int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (length <= 0)
        {
            return std::string();
        }

        // 2回目: 確保した領域に実際に変換する
        std::string result(length, '\0');
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], length, nullptr, nullptr);
        result.resize(length - 1);  // 末尾の '\0' の分を取り除く
        return result;
    }
}

//-----------------------------------------------------------------------------
// テクスチャを1枚読み込み、ヒープの slot 番目に SRV を作る
//   path    : 読み込む画像ファイル。DDS・PNG・JPG・BMP
//   slot    : SRV を置くヒープの番号。m_Textures の番号と同じにする
//   texture : 読み込んだテクスチャが入る
//   戻り値  : 成功なら S_OK
//-----------------------------------------------------------------------------
HRESULT App::LoadTextureResource(const std::wstring& path, UINT slot, ComPtr<ID3D12Resource>& texture)
{
    if (slot >= MaxTextures)
    {
        return E_INVALIDARG;    // ヒープに入りきらない
    }

    // 1. テクスチャを読み込む
    //    拡張子で読み込み方を変え、DirectXTK12 で Upload → Default のコピーとバリアまでまとめて行う
    //    DDS はそのまま読み、PNG・JPG・BMP は Windows の画像読み込み機能である WIC で読む
    ComPtr<ID3D12Resource> pNewTexture;
    DirectX::ResourceUploadBatch resourceUpload(_device.Get());
    resourceUpload.Begin();

    HRESULT hr = S_OK;
    if (IsDdsFile(path))
    {
        hr = DirectX::CreateDDSTextureFromFileEx(
            _device.Get(),
            resourceUpload,
            path.c_str(),
            0,                                  // 最大サイズ。0なら制限なし
            D3D12_RESOURCE_FLAG_NONE,
            DirectX::DDS_LOADER_FORCE_SRGB,     // RTVが sRGB なので画像も sRGB として読む
            pNewTexture.GetAddressOf());
    }
    else
    {
        hr = DirectX::CreateWICTextureFromFileEx(
            _device.Get(),
            resourceUpload,
            path.c_str(),
            0,                                                                  // 最大サイズ。0なら制限なし
            D3D12_RESOURCE_FLAG_NONE,
            DirectX::WIC_LOADER_FORCE_SRGB | DirectX::WIC_LOADER_MIP_AUTOGEN,   // sRGB として読み、ミップマップも作る
            pNewTexture.GetAddressOf());
    }

    // 2. GPU への転送を実行し、終わるまで待つ。失敗したときも、Begin したので End しておく
    auto finished = resourceUpload.End(_queue.Get());
    finished.wait();
    if (FAILED(hr))
    {
        return hr;
    }

    pNewTexture->SetName(path.c_str());  // ファイルのパスを名前にして、PIXで見分けやすくする

    // 3. SRVをヒープの slot 番目に書き込む
    CreateTextureSrv(pNewTexture.Get(), slot);

    texture = pNewTexture;
    return S_OK;
}

//-----------------------------------------------------------------------------
// 組み込みの白いテクスチャを、画像ファイルを使わずにコードで作る
// 白なら「テクスチャ色 × 物体の色」で物体の色がそのまま出るので、既定のテクスチャに向いている
//   slot    : SRV を置くヒープの番号
//   texture : 作ったテクスチャが入る
//-----------------------------------------------------------------------------
HRESULT App::CreateBuiltinTexture(UINT slot, ComPtr<ID3D12Resource>& texture)
{
    if (slot >= MaxTextures)
    {
        return E_INVALIDARG;
    }

    // 1. 4×4 の白い画像をメモリ上に作る。1ピクセル = RGBA の4バイト（0xFFFFFFFF で全部 255 = 白）
    const UINT size = 4;
    std::vector<uint32_t> pixels(size * size, 0xFFFFFFFFu);

    D3D12_SUBRESOURCE_DATA initData = {};
    initData.pData      = pixels.data();
    initData.RowPitch   = static_cast<LONG_PTR>(size * sizeof(uint32_t));   // 1行のバイト数
    initData.SlicePitch = initData.RowPitch * size;                         // 画像全体のバイト数

    // 2. GPU に送ってテクスチャにする。転送は DirectXTK12 の ResourceUploadBatch にまかせる
    //    ファイルから読むテクスチャと同じく sRGB 形式にする（白は sRGB でも 1.0 のまま）
    ComPtr<ID3D12Resource> pNewTexture;
    DirectX::ResourceUploadBatch resourceUpload(_device.Get());
    resourceUpload.Begin();

    const HRESULT hr = DirectX::CreateTextureFromMemory(
        _device.Get(),
        resourceUpload,
        size, size,
        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        initData,
        pNewTexture.GetAddressOf());

    // 転送を実行し、終わるまで待つ。pixels はここまで生きている必要がある
    auto finished = resourceUpload.End(_queue.Get());
    finished.wait();
    if (FAILED(hr))
    {
        return hr;
    }

    pNewTexture->SetName(L"(default)");

    // 3. SRV をヒープの slot 番目に書き込む
    CreateTextureSrv(pNewTexture.Get(), slot);

    texture = pNewTexture;
    return S_OK;
}

//-----------------------------------------------------------------------------
// テクスチャの SRV をヒープの slot 番目に作る。SRVはテクスチャの読み方の説明書きにあたる
//-----------------------------------------------------------------------------
void App::CreateTextureSrv(ID3D12Resource* texture, UINT slot)
{
    const auto textureDesc = texture->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = textureDesc.Format;                                    // リソースと同じ形式
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = textureDesc.MipLevels;
    srvDesc.Texture2D.PlaneSlice = 0;
    srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

    auto srvCpuHandle = _heapCBV_SRV_UAV->GetCPUDescriptorHandleForHeapStart();  // 書き込む時に使う
    srvCpuHandle.ptr += static_cast<SIZE_T>(m_SrvDescSize) * slot;              // slot 番目にずらす

    _device->CreateShaderResourceView(texture, &srvDesc, srvCpuHandle);
}

//-----------------------------------------------------------------------------
// フォルダの中の画像ファイルを一覧にする
//   folder : 調べるフォルダ。最後に \ が付く（例："C:\\...\\res\\texture\\"）
//   戻り値 : 画像ファイルのフルパスの一覧
//-----------------------------------------------------------------------------
std::vector<std::wstring> App::FindTextureFiles(const std::wstring& folder) const
{
    std::vector<std::wstring> files;

    // Windows の FindFirstFile / FindNextFile で、フォルダの中のファイルを1つずつ調べる
    WIN32_FIND_DATAW data = {};
    HANDLE hFind = FindFirstFileW((folder + L"*").c_str(), &data);
    if (hFind == INVALID_HANDLE_VALUE)
    {
        return files;
    }

    do
    {
        // フォルダと、読み込めない形式のファイルは飛ばす
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || !IsTextureFile(data.cFileName))
        {
            continue;
        }
        files.push_back(folder + data.cFileName);
    } while (FindNextFileW(hFind, &data));

    FindClose(hFind);
    return files;
}

//-----------------------------------------------------------------------------
// テクスチャを全部読み込む
// 1. 0番：組み込みの白いテクスチャ "(default)"。これが作れなければ失敗
// 2. res\texture フォルダの画像を 1番から順に読み込む。読めない画像は飛ばす
// 3. モデルのマテリアルに指定された画像があれば、それも読み込む（まだ読んでいなければ）
// i 番目のテクスチャ = m_Textures[i] = m_TextureFiles[i] = m_TextureNames[i] = ヒープの i 番目の SRV
//-----------------------------------------------------------------------------
bool App::LoadAllTextures(const std::wstring& modelTexturePath)
{
    m_Textures.clear();
    m_TextureFiles.clear();
    m_TextureNames.clear();

    // SRV 1個分の大きさと、ヒープの先頭の GPU ハンドル。描画時に使う
    m_SrvDescSize      = _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_TextureHeapStart = _heapCBV_SRV_UAV->GetGPUDescriptorHandleForHeapStart();

    // 1. 0番：組み込みの白いテクスチャ
    {
        ComPtr<ID3D12Resource> texture;
        if (FAILED(CreateBuiltinTexture(0, texture)))
        {
            return false;
        }
        m_Textures.push_back(texture);
        m_TextureFiles.push_back(L"");          // ファイルではないので空
        m_TextureNames.push_back("(default)");  // 保存ファイルにはこの名前で書く
        m_DefaultTexture = 0;
    }

    // 画像ファイルを1枚読み込んで一覧に足す。同じ名前のものがすでにあれば読まない
    auto addTexture = [this](const std::wstring& path)
    {
        // ファイル名の部分だけ取り出す。"res\\texture\\a.png" なら "a.png"
        const auto pos  = path.find_last_of(L"\\/");
        const auto name = ToUtf8((pos == std::wstring::npos) ? path : path.substr(pos + 1));
        if (FindTextureByName(name) >= 0)
        {
            return;     // もう読み込んである
        }

        if (m_Textures.size() >= MaxTextures)
        {
            OutputDebugStringW((L"[LoadAllTextures] MaxTextures を超えたので読み込まない: " + path + L"\n").c_str());
            return;
        }

        ComPtr<ID3D12Resource> texture;
        const auto hr = LoadTextureResource(path, static_cast<UINT>(m_Textures.size()), texture);
        if (FAILED(hr))
        {
            OutputDebugStringW((L"[LoadAllTextures] 読み込めないので飛ばす: " + path + L"\n").c_str());
            return;
        }

        m_Textures.push_back(texture);
        m_TextureFiles.push_back(path);
        m_TextureNames.push_back(name);
    };

    // 2. res\texture フォルダの画像を全部読み込む。フォルダが無くても起動は続ける
    const std::wstring resDir = FindResDirectory();
    if (resDir.empty())
    {
        OutputDebugStringW(L"[LoadAllTextures] res フォルダが見つからないので、(default) だけで起動します\n");
    }
    else
    {
        for (const auto& path : FindTextureFiles(resDir + L"texture\\"))
        {
            addTexture(path);
        }
    }

    // 3. モデルのマテリアルに指定された画像。見つからなければ飛ばす
    if (!modelTexturePath.empty())
    {
        if (GetFileAttributesW(modelTexturePath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            addTexture(modelTexturePath);
        }
        else
        {
            OutputDebugStringW((L"[LoadAllTextures] モデルが指定した画像が見つからない: " + modelTexturePath + L"\n").c_str());
        }
    }

    return true;
}

//-----------------------------------------------------------------------------
// ファイル名からテクスチャの番号を探す。大文字・小文字は区別しない
//   name   : "fabrics_0037_color_1k.jpg" のようなファイル名。UTF-8
//   戻り値 : 番号。見つからなければ -1
//-----------------------------------------------------------------------------
int App::FindTextureByName(const std::string& name) const
{
    for (int i = 0; i < static_cast<int>(m_TextureNames.size()); ++i)
    {
        if (_stricmp(m_TextureNames[i].c_str(), name.c_str()) == 0)
        {
            return i;
        }
    }
    return -1;
}

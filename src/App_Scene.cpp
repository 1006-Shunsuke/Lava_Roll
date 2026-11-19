//-----------------------------------------------------------------------------
// File : App_Scene.cpp
// Desc : 物体の配置（シーン）の保存と読み込み
//        保存先は res\stage\stage1.txt。1物体1行のテキスト形式で、メモ帳でも読み書きできる
//
//        # model  pos.x pos.y pos.z  rot.x rot.y rot.z  scale.x scale.y scale.z  color.r color.g color.b color.a  role  group  texture
//        Sphere  -4 0 0  0 0 0  1 1 1  1 1 1 1  1  0  (default)
//
//        version 1 : texture の列なし（読み込むと既定のテクスチャになる）
//        version 2 : 行の最後に texture のファイル名。番号ではなく名前で保存する
//                    （番号はフォルダ内の並び順で決まるので、画像を足すとずれるため）
//        version 3 : 色の後ろ・テクスチャの前に role（役割の番号）。テクスチャは「行の残り全部」なので一番後ろのまま
//        version 4 : 列は version 3 と同じ。役割の意味が変わった（None = 通り抜ける、Wall = 乗れる）
//                    version 3 以前は None の箱にも乗れたので、読み込むときに None の箱を Wall に直す
//        version 5 : model を番号ではなく名前で書く（"Sphere"・"Box"・"slope.obj" など）
//                    res\model のモデルは番号がフォルダの中身で変わるので、テクスチャと同じく名前で保存する
//        version 6 : role の後ろ・テクスチャの前に group（グループ番号）。アイテムとゲートを結び付ける
//        version 7 : group の後ろ・テクスチャの前に orbit（0/1）・中心 x y z・speed・axis（円周に沿って回る設定）
//        version 8 : orbit の後ろ・テクスチャの前に spin（0/1）・speed・axis（その場で回る設定）
//                    version 7 以前は Item と Damage が自動で回っていたので、読み込むときに Spin を ON にする
//        version 9 : spin の後ろ・テクスチャの前に uv scale x y・uv scroll x y・emissive（見た目の設定）
//                    version の行の次に、"env" で始まる行で環境の設定（ライト・背景の空）を書く
//-----------------------------------------------------------------------------
#include "App.h"
#include <fstream>
#include <sstream>
#include <string>
#include <cstdlib>   // std::atoi

using namespace DirectX;

namespace
{
    const int SceneVersion = 9;     // 書き出すときのファイル形式の版。項目や意味を変えたら上げる。読み込みは 1〜9 に対応

    // ファイルが存在するか（フォルダは除く）
    bool FileExists(const std::wstring& path)
    {
        DWORD attr = GetFileAttributesW(path.c_str());
        return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
    }

    // 出力ウィンドウに1行出す
    void Log(const std::wstring& text)
    {
        OutputDebugStringW((L"[Scene] " + text + L"\n").c_str());
    }
}

//-----------------------------------------------------------------------------
// 保存先の場所を決める
// res フォルダを探し（App_Helper.cpp の FindResDirectory）、その中の stage\stage1.txt を保存先にする
//-----------------------------------------------------------------------------
bool App::FindScenePath()
{
    const std::wstring resDir = FindResDirectory();   // 見つかった res フォルダ。最後に \ が付く

    if (resDir.empty())
    {
        Log(L"res フォルダが見つからないので、保存・読み込みはできません");
        m_ScenePath.clear();
        return false;
    }

    // stage フォルダが無ければ作る。すでにあるときの失敗は問題ない
    const std::wstring stageDir = resDir + L"stage\\";
    CreateDirectoryW(stageDir.c_str(), nullptr);

    m_ScenePath = stageDir + L"stage1.txt";
    Log(L"保存先: " + m_ScenePath);
    return true;
}

//-----------------------------------------------------------------------------
// 保存ファイルが無いときの初期配置：球5個＋床
//-----------------------------------------------------------------------------
void App::CreateDefaultScene()
{
    m_Objects.clear();
    m_SelectedObject = -1;      // 何も選んでいない状態で始める

    // 球を5個、x方向に2ずつ間をあけて並べる
    for (int j = 0; j < 5; ++j)
    {
        GameObject object;
        object.position = { (static_cast<float>(j) - 2.0f) * 2.0f, 0.0f, 0.0f };   // -4, -2, 0, 2, 4
        AddObject(object);
    }

    // 床：箱を平たく伸ばして、球の下に置く
    GameObject floor;
    floor.model    = Model_Box;
    floor.role     = Role_Wall;                 // 乗れる
    floor.position = { 0.0f, -1.1f, 0.0f };     // 球（半径1）の少し下
    floor.scale    = { 12.0f, 0.2f, 12.0f };    // 横12・厚さ0.2・奥行12
    AddObject(floor);
}

//-----------------------------------------------------------------------------
// 今の配置をファイルに書き出す
// 1. 今のファイルを .bak にコピーする（1つ前の保存状態に戻せるように）
// 2. 1物体1行で書き出す
//-----------------------------------------------------------------------------
bool App::SaveScene()
{
    if (m_ScenePath.empty())
    {
        m_SceneMessage = "Save failed (no path)";
        return false;
    }

    // 1. 上書きする前に、今のファイルを .bak にコピーする。ファイルが無い（初回）ときは何もしない
    if (FileExists(m_ScenePath))
    {
        const std::wstring bakPath = m_ScenePath.substr(0, m_ScenePath.size() - 4) + L".bak";   // .txt → .bak
        CopyFileW(m_ScenePath.c_str(), bakPath.c_str(), FALSE);    // FALSE：すでにあれば上書き
    }

    // 2. 書き出す
    std::ofstream file(m_ScenePath);    // テキストとして開く。中身は空になる
    if (!file)
    {
        Log(L"ファイルを開けませんでした: " + m_ScenePath);
        m_SceneMessage = "Save failed";
        return false;
    }

    file << "# Last stage file\n";
    file << "version " << SceneVersion << "\n";

    // 環境の設定（ライト・背景の空）。"env 名前 値..." の形で1行ずつ書く
    {
        const bool validSky = m_SkyTexture >= 0 && m_SkyTexture < static_cast<int>(m_TextureNames.size());
        const std::string skyTextureName = validSky ? m_TextureNames[m_SkyTexture] : std::string("(default)");
        file << "# environment\n";
        file << "env light_dir "       << m_LightDir.x << ' ' << m_LightDir.y << ' ' << m_LightDir.z << "\n";
        file << "env light_color "     << m_LightColor[0] << ' ' << m_LightColor[1] << ' ' << m_LightColor[2] << "\n";
        file << "env light_intensity " << m_LightIntensity << "\n";
        file << "env ambient "         << m_AmbientColor[0] << ' ' << m_AmbientColor[1] << ' ' << m_AmbientColor[2] << "\n";
        file << "env specular "        << m_SpecularIntensity << "\n";
        file << "env shininess "       << m_Shininess << "\n";
        file << "env clear_color "     << m_ClearColor[0] << ' ' << m_ClearColor[1] << ' ' << m_ClearColor[2] << ' ' << m_ClearColor[3] << "\n";
        file << "env sky_mode "        << m_SkyMode << "\n";
        file << "env sky_top "         << m_SkyTop[0] << ' ' << m_SkyTop[1] << ' ' << m_SkyTop[2] << "\n";
        file << "env sky_horizon "     << m_SkyHorizon[0] << ' ' << m_SkyHorizon[1] << ' ' << m_SkyHorizon[2] << "\n";
        file << "env sky_bottom "      << m_SkyBottom[0] << ' ' << m_SkyBottom[1] << ' ' << m_SkyBottom[2] << "\n";
        file << "env sky_curve "       << m_SkyExponent << "\n";
        file << "env sky_rotation "    << m_SkyRotation << "\n";
        file << "env sky_texture "     << skyTextureName << "\n";
    }
    file << "# model  pos.x pos.y pos.z  rot.x rot.y rot.z  scale.x scale.y scale.z  color.r color.g color.b color.a  role  group  orbit cx cy cz speed axis  spin speed axis  uv_scale x y  uv_scroll x y  emissive  texture\n";
    file << "# model : Sphere, Box, or a file name in res/model (e.g. slope.obj)\n";
    file << "# role  : 0 = None (pass through), 1 = Player, 2 = Wall (solid), 3 = Item, 4 = Gate, 5 = Damage, 6 = Goal\n";
    file << "# group : items and gates with the same group number are linked (0 = none)\n";

    for (const auto& o : m_Objects)
    {
        // テクスチャはファイル名で書く。番号がおかしいときは既定のテクスチャの名前にする
        const int textureId = (o.texture >= 0 && o.texture < static_cast<int>(m_TextureNames.size())) ? o.texture : m_DefaultTexture;
        const std::string textureName = m_TextureNames.empty() ? std::string() : m_TextureNames[textureId];

        // モデルは名前で書く
        const std::string modelName = (o.model >= 0 && o.model < static_cast<int>(m_Models.size())) ? m_Models[o.model].name : std::string("Box");

        file << modelName << "  "
             << o.position.x << ' ' << o.position.y << ' ' << o.position.z << "  "
             << o.rotation.x << ' ' << o.rotation.y << ' ' << o.rotation.z << "  "
             << o.scale.x    << ' ' << o.scale.y    << ' ' << o.scale.z    << "  "
             << o.color.x    << ' ' << o.color.y    << ' ' << o.color.z    << ' ' << o.color.w << "  "
             << o.role       << "  "
             << o.group      << "  "
             << (o.orbit ? 1 : 0) << ' '
             << o.orbitCenter.x << ' ' << o.orbitCenter.y << ' ' << o.orbitCenter.z << ' '
             << o.orbitSpeed << ' ' << o.orbitAxis << "  "
             << (o.spin ? 1 : 0) << ' ' << o.spinSpeed << ' ' << o.spinAxis << "  "
             << o.uvScale.x << ' ' << o.uvScale.y << ' ' << o.uvScroll.x << ' ' << o.uvScroll.y << ' ' << o.emissive << "  "
             << textureName << "\n";
    }

    if (!file)
    {
        m_SceneMessage = "Save failed";
        return false;
    }

    m_SceneMessage = "Saved " + std::to_string(m_Objects.size()) + " objects";
    return true;
}

//-----------------------------------------------------------------------------
// ファイルを読んで配置を作り直す
// まず一時的な配列に読み込み、最後まで読めたときだけ m_Objects と入れ替える
// 失敗したら false を返し、今の配置はそのまま残す
//-----------------------------------------------------------------------------
bool App::LoadScene()
{
    if (m_ScenePath.empty() || !FileExists(m_ScenePath))
    {
        m_SceneMessage = "No save file";
        return false;
    }

    std::ifstream file(m_ScenePath);
    if (!file)
    {
        Log(L"ファイルを開けませんでした: " + m_ScenePath);
        m_SceneMessage = "Load failed";
        return false;
    }

    std::vector<GameObject> loaded;     // 一時的な配列

    // 環境の設定も、一時的な変数に読み込む（失敗したら今の設定のまま）。ファイルに無い項目は今の値のまま
    XMFLOAT3 envLightDir = m_LightDir;
    float envLightColor[3]   = { m_LightColor[0], m_LightColor[1], m_LightColor[2] };
    float envLightIntensity  = m_LightIntensity;
    float envAmbient[3]      = { m_AmbientColor[0], m_AmbientColor[1], m_AmbientColor[2] };
    float envSpecular        = m_SpecularIntensity;
    float envShininess       = m_Shininess;
    float envClear[4]        = { m_ClearColor[0], m_ClearColor[1], m_ClearColor[2], m_ClearColor[3] };
    int   envSkyMode         = m_SkyMode;
    float envSkyTop[3]       = { m_SkyTop[0], m_SkyTop[1], m_SkyTop[2] };
    float envSkyHorizon[3]   = { m_SkyHorizon[0], m_SkyHorizon[1], m_SkyHorizon[2] };
    float envSkyBottom[3]    = { m_SkyBottom[0], m_SkyBottom[1], m_SkyBottom[2] };
    float envSkyCurve        = m_SkyExponent;
    float envSkyRotation     = m_SkyRotation;
    int   envSkyTexture      = m_SkyTexture;

    bool        versionFound = false;
    int         version = 0;            // ファイルに書かれた版
    std::string line;
    int         lineNo = 0;

    while (std::getline(file, line))
    {
        ++lineNo;

        // 空行と # で始まるコメント行は飛ばす
        const auto first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#')
        {
            continue;
        }

        std::istringstream ss(line);

        // 最初の有効な行は "version 1" または "version 2"
        if (!versionFound)
        {
            std::string word;
            if (!(ss >> word >> version) || word != "version" || version < 1 || version > SceneVersion)
            {
                Log(L"version の行が正しくありません（" + std::to_wstring(lineNo) + L"行目）");
                m_SceneMessage = "Load failed (version)";
                return false;
            }
            versionFound = true;
            continue;
        }

        // 環境の設定の行："env 名前 値..."（version 9 から）
        if (line.compare(first, 4, "env ") == 0)
        {
            std::string word, key;
            ss >> word >> key;
            bool ok = true;
            if      (key == "light_dir")       { ok = static_cast<bool>(ss >> envLightDir.x >> envLightDir.y >> envLightDir.z); }
            else if (key == "light_color")     { ok = static_cast<bool>(ss >> envLightColor[0] >> envLightColor[1] >> envLightColor[2]); }
            else if (key == "light_intensity") { ok = static_cast<bool>(ss >> envLightIntensity); }
            else if (key == "ambient")         { ok = static_cast<bool>(ss >> envAmbient[0] >> envAmbient[1] >> envAmbient[2]); }
            else if (key == "specular")        { ok = static_cast<bool>(ss >> envSpecular); }
            else if (key == "shininess")       { ok = static_cast<bool>(ss >> envShininess); }
            else if (key == "clear_color")     { ok = static_cast<bool>(ss >> envClear[0] >> envClear[1] >> envClear[2] >> envClear[3]); }
            else if (key == "sky_mode")        { ok = static_cast<bool>(ss >> envSkyMode); }
            else if (key == "sky_top")         { ok = static_cast<bool>(ss >> envSkyTop[0] >> envSkyTop[1] >> envSkyTop[2]); }
            else if (key == "sky_horizon")     { ok = static_cast<bool>(ss >> envSkyHorizon[0] >> envSkyHorizon[1] >> envSkyHorizon[2]); }
            else if (key == "sky_bottom")      { ok = static_cast<bool>(ss >> envSkyBottom[0] >> envSkyBottom[1] >> envSkyBottom[2]); }
            else if (key == "sky_curve")       { ok = static_cast<bool>(ss >> envSkyCurve); }
            else if (key == "sky_rotation")    { ok = static_cast<bool>(ss >> envSkyRotation); }
            else if (key == "sky_texture")
            {
                // テクスチャ名は行の残り全部
                std::string name;
                std::getline(ss >> std::ws, name);
                while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r'))
                {
                    name.pop_back();
                }
                const int id = FindTextureByName(name);
                envSkyTexture = (id >= 0) ? id : m_DefaultTexture;
            }
            else
            {
                Log(std::to_wstring(lineNo) + L"行目：知らない env の項目なので飛ばします");
            }
            if (!ok)
            {
                Log(std::to_wstring(lineNo) + L"行目：env の値が読めないので飛ばします");
            }
            continue;
        }

        // 物体1個分を読む。数が足りない行は飛ばす
        // モデルは version 5 から名前、それより前は番号（0 = Sphere、1 = Box）
        GameObject o;
        std::string modelToken;
        if (!(ss >> modelToken
                 >> o.position.x >> o.position.y >> o.position.z
                 >> o.rotation.x >> o.rotation.y >> o.rotation.z
                 >> o.scale.x    >> o.scale.y    >> o.scale.z
                 >> o.color.x    >> o.color.y    >> o.color.z    >> o.color.w))
        {
            Log(std::to_wstring(lineNo) + L"行目：数が足りないので飛ばします");
            continue;
        }

        if (version >= 5)
        {
            o.model = FindModelByName(modelToken);
            if (o.model < 0)
            {
                Log(std::to_wstring(lineNo) + L"行目：モデルが見つからないので Box にします");
                o.model = Model_Box;
            }
        }
        else
        {
            // 番号。0 = Sphere、1 = Box 以外はおかしいので飛ばす
            o.model = std::atoi(modelToken.c_str());
            if (modelToken.empty() || o.model < 0 || o.model >= Model_BuiltinCount)
            {
                Log(std::to_wstring(lineNo) + L"行目：モデル番号がおかしいので飛ばします");
                continue;
            }
        }

        // 役割（version 3 から）。読めない・範囲外なら飾りにする
        o.role = Role_None;
        if (version >= 3)
        {
            if (!(ss >> o.role) || o.role < 0 || o.role >= Role_Count)
            {
                Log(std::to_wstring(lineNo) + L"行目：役割の番号がおかしいので飾り（None）にします");
                o.role = Role_None;
                ss.clear();     // 読み取りに失敗した状態を戻して、続きのテクスチャ名を読めるようにする
            }
        }

        // グループ番号（version 6 から）。読めない・マイナスなら 0 にする
        o.group = 0;
        if (version >= 6)
        {
            if (!(ss >> o.group) || o.group < 0)
            {
                Log(std::to_wstring(lineNo) + L"行目：グループ番号がおかしいので 0 にします");
                o.group = 0;
                ss.clear();
            }
        }

        // 円周に沿って回る設定（version 7 から）。読めなければ回らない
        o.orbit = false;
        if (version >= 7)
        {
            int orbitFlag = 0;
            if (!(ss >> orbitFlag >> o.orbitCenter.x >> o.orbitCenter.y >> o.orbitCenter.z >> o.orbitSpeed >> o.orbitAxis))
            {
                Log(std::to_wstring(lineNo) + L"行目：Orbit の設定が読めないので回さない設定にします");
                orbitFlag = 0;
                o.orbitCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);
                o.orbitSpeed = 90.0f;
                o.orbitAxis = 0;
                ss.clear();
            }
            o.orbit = (orbitFlag != 0);
            if (o.orbitAxis < 0 || o.orbitAxis > 2)
            {
                o.orbitAxis = 0;
            }
        }

        // その場で回る設定（version 8 から）
        o.spin = false;
        if (version >= 8)
        {
            int spinFlag = 0;
            if (!(ss >> spinFlag >> o.spinSpeed >> o.spinAxis))
            {
                Log(std::to_wstring(lineNo) + L"行目：Spin の設定が読めないので回さない設定にします");
                spinFlag = 0;
                o.spinSpeed = 90.0f;
                o.spinAxis = 0;
                ss.clear();
            }
            o.spin = (spinFlag != 0);
            if (o.spinAxis < 0 || o.spinAxis > 2)
            {
                o.spinAxis = 0;
            }
        }
        else if (o.role == Role_Item || o.role == Role_Damage)
        {
            // version 7 以前は Item と Damage が自動で回っていたので、同じ見た目になるように Spin を ON にする
            o.spin      = true;
            o.spinSpeed = 90.0f;
            o.spinAxis  = 0;
        }

        // version 3 以前は「None の箱にも乗れる」ルールだったので、同じ動きになるように Wall に直す
        if (version <= 3 && o.model == Model_Box && o.role == Role_None)
        {
            o.role = Role_Wall;
        }

        // 見た目の設定（version 9 から）。読めなければ初期値
        if (version >= 9)
        {
            if (!(ss >> o.uvScale.x >> o.uvScale.y >> o.uvScroll.x >> o.uvScroll.y >> o.emissive))
            {
                Log(std::to_wstring(lineNo) + L"行目：見た目の設定が読めないので初期値にします");
                o.uvScale  = XMFLOAT2(1.0f, 1.0f);
                o.uvScroll = XMFLOAT2(0.0f, 0.0f);
                o.emissive = 0.0f;
                ss.clear();
            }
        }

        // テクスチャ（version 2 から）。行の残り全部をファイル名として読む（名前に空白があってもよいように）
        o.texture = m_DefaultTexture;
        if (version >= 2)
        {
            std::string textureName;
            std::getline(ss >> std::ws, textureName);
            while (!textureName.empty() && (textureName.back() == ' ' || textureName.back() == '\t' || textureName.back() == '\r'))
            {
                textureName.pop_back();     // 末尾の空白と改行を取り除く
            }

            if (!textureName.empty())
            {
                const int id = FindTextureByName(textureName);
                if (id >= 0)
                {
                    o.texture = id;
                }
                else
                {
                    Log(std::to_wstring(lineNo) + L"行目：テクスチャが見つからないので既定のテクスチャにします");
                }
            }
        }

        // 上限を超えた分は読まない
        if (loaded.size() >= MaxObjects)
        {
            Log(L"MaxObjects を超えたので、" + std::to_wstring(lineNo) + L"行目以降は読みません");
            break;
        }

        loaded.push_back(o);
    }

    if (!versionFound)
    {
        m_SceneMessage = "Load failed (empty)";
        return false;
    }

    // 最後まで読めたので入れ替える
    m_Objects.swap(loaded);

    // 環境の設定も反映する
    m_LightDir          = envLightDir;
    m_LightIntensity    = envLightIntensity;
    m_SpecularIntensity = envSpecular;
    m_Shininess         = envShininess;
    for (int i = 0; i < 3; ++i)
    {
        m_LightColor[i]   = envLightColor[i];
        m_AmbientColor[i] = envAmbient[i];
        m_SkyTop[i]       = envSkyTop[i];
        m_SkyHorizon[i]   = envSkyHorizon[i];
        m_SkyBottom[i]    = envSkyBottom[i];
    }
    for (int i = 0; i < 4; ++i)
    {
        m_ClearColor[i] = envClear[i];
    }
    m_SkyMode     = (envSkyMode == 1) ? 1 : 0;
    m_SkyExponent = envSkyCurve;
    m_SkyRotation = envSkyRotation;
    m_SkyTexture  = envSkyTexture;
    m_SelectedObject = -1;      // 何も選んでいない状態で始める
    m_SceneMessage = "Loaded " + std::to_string(m_Objects.size()) + " objects";
    return true;
}

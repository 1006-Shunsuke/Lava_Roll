//-----------------------------------------------------------------------------
// File : App_Loop.cpp
// Desc : メインループと毎フレームの処理。Run・MainLoop・WndProc・Render・Present
//-----------------------------------------------------------------------------
#include "App.h"
#include <cmath>
#include <cfloat>   // FLT_MAX
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

using namespace DirectX;

namespace
{
    //--------------------------------------------------------------------------------
    // ワールド行列を作る。描画・ピッキング・地面の判定で同じ作り方にする
    //--------------------------------------------------------------------------------
    XMMATRIX MakeWorldMatrix(const XMFLOAT3& position, const XMFLOAT3& rotationDeg, const XMFLOAT3& scale)
    {
        return XMMatrixScaling(scale.x, scale.y, scale.z) *
               XMMatrixRotationRollPitchYaw(
                   XMConvertToRadians(rotationDeg.x),
                   XMConvertToRadians(rotationDeg.y),
                   XMConvertToRadians(rotationDeg.z)) *
               XMMatrixTranslation(position.x, position.y, position.z);
    }

    //--------------------------------------------------------------------------------
    // レイと「軸にそろった箱（mn〜mx）」が重なる t の区間を求める（スラブ法）
    //   x・y・z それぞれで「箱の範囲に入っている t の区間」を求め、3つの重なりを返す
    //   tMin : 箱に入る t（レイの始点が箱の中なら負）  tMax : 箱から出る t
    //--------------------------------------------------------------------------------
    bool RayAabbRange(const XMFLOAT3& o, const XMFLOAT3& d, const XMFLOAT3& mn, const XMFLOAT3& mx, float& tMin, float& tMax)
    {
        const float origin[3] = { o.x, o.y, o.z };
        const float dir[3]    = { d.x, d.y, d.z };
        const float lo[3]     = { mn.x, mn.y, mn.z };
        const float hi[3]     = { mx.x, mx.y, mx.z };
        tMin = -FLT_MAX;
        tMax =  FLT_MAX;

        for (int i = 0; i < 3; ++i)
        {
            if (fabsf(dir[i]) < 1e-8f)
            {
                // この軸に平行なレイ：始点が範囲の外なら当たらない
                if (origin[i] < lo[i] || origin[i] > hi[i])
                {
                    return false;
                }
                continue;
            }
            float t0 = (lo[i] - origin[i]) / dir[i];
            float t1 = (hi[i] - origin[i]) / dir[i];
            if (t0 > t1) { const float tmp = t0; t0 = t1; t1 = tmp; }   // 小さい方を t0 にする
            tMin = (std::max)(tMin, t0);
            tMax = (std::min)(tMax, t1);
            if (tMin > tMax)
            {
                return false;   // 重なりがない
            }
        }
        return tMax >= 0.0f;    // 箱がレイの始点より後ろなら当たらない
    }

    // 中心が原点・1×1×1 の箱（-0.5〜+0.5）の場合
    bool RayUnitBoxRange(const XMFLOAT3& o, const XMFLOAT3& d, float& tMin, float& tMax)
    {
        return RayAabbRange(o, d, XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.5f, 0.5f, 0.5f), tMin, tMax);
    }

    //--------------------------------------------------------------------------------
    // レイと三角形の交差判定（Möller–Trumbore 法）
    //   レイ上の点 = o + t * d が、三角形 v0・v1・v2 の中に入る t を求める
    //   三角形の表・裏は区別しない（Blender から書き出したときの頂点の順番に左右されないように）
    //--------------------------------------------------------------------------------
    bool RayTriangle(const XMVECTOR& o, const XMVECTOR& d,
                     const XMVECTOR& v0, const XMVECTOR& v1, const XMVECTOR& v2, float& t)
    {
        const XMVECTOR e1 = v1 - v0;                                // 三角形の辺1
        const XMVECTOR e2 = v2 - v0;                                // 三角形の辺2
        const XMVECTOR pv = XMVector3Cross(d, e2);
        const float det = XMVectorGetX(XMVector3Dot(e1, pv));
        if (fabsf(det) < 1e-12f)
        {
            return false;   // レイが三角形の面と平行
        }
        const float invDet = 1.0f / det;

        const XMVECTOR tv = o - v0;
        const float u = XMVectorGetX(XMVector3Dot(tv, pv)) * invDet;   // 三角形の中の位置（その1）
        if (u < 0.0f || u > 1.0f)
        {
            return false;
        }
        const XMVECTOR qv = XMVector3Cross(tv, e1);
        const float v = XMVectorGetX(XMVector3Dot(d, qv)) * invDet;    // 三角形の中の位置（その2）
        if (v < 0.0f || u + v > 1.0f)
        {
            return false;
        }
        t = XMVectorGetX(XMVector3Dot(e2, qv)) * invDet;
        return true;
    }

    //--------------------------------------------------------------------------------
    // レイとモデルの全部の三角形を調べる（ローカル空間で使う）
    //   tFirst   : 一番手前で当たった t（t >= 0 のものだけ）
    //   firstTri : その三角形の番号（triIndices の 3 × 番号 から3個）
    //   hitCount : t >= 0 で当たった三角形の数。奇数ならレイの始点がモデルの中（閉じた形のとき）
    //--------------------------------------------------------------------------------
    bool RayTriangles(const std::vector<XMFLOAT3>& pos, const std::vector<uint32_t>& idx,
                      const XMFLOAT3& o, const XMFLOAT3& d, float& tFirst, size_t& firstTri, int& hitCount)
    {
        const XMVECTOR ov = XMLoadFloat3(&o);
        const XMVECTOR dv = XMLoadFloat3(&d);
        tFirst   = FLT_MAX;
        firstTri = 0;
        hitCount = 0;

        for (size_t i = 0; i + 2 < idx.size(); i += 3)
        {
            float t = 0.0f;
            if (RayTriangle(ov, dv, XMLoadFloat3(&pos[idx[i]]), XMLoadFloat3(&pos[idx[i + 1]]), XMLoadFloat3(&pos[idx[i + 2]]), t)
                && t >= 0.0f)
            {
                ++hitCount;
                if (t < tFirst)
                {
                    tFirst   = t;
                    firstTri = i / 3;
                }
            }
        }
        return hitCount > 0;
    }

    //--------------------------------------------------------------------------------
    // 三角形 a・b・c の上で、点 p に一番近い点を求める
    // （Christer Ericson『Real-Time Collision Detection』の方法。頂点・辺・面のどこが一番近いかを順に調べる）
    //--------------------------------------------------------------------------------
    XMVECTOR ClosestPointOnTriangle(const XMVECTOR& p, const XMVECTOR& a, const XMVECTOR& b, const XMVECTOR& c)
    {
        auto dot = [](const XMVECTOR& x, const XMVECTOR& y) { return XMVectorGetX(XMVector3Dot(x, y)); };

        const XMVECTOR ab = b - a, ac = c - a, ap = p - a;
        const float d1 = dot(ab, ap), d2 = dot(ac, ap);
        if (d1 <= 0.0f && d2 <= 0.0f) return a;                                 // 頂点 a が一番近い

        const XMVECTOR bp = p - b;
        const float d3 = dot(ab, bp), d4 = dot(ac, bp);
        if (d3 >= 0.0f && d4 <= d3) return b;                                   // 頂点 b が一番近い

        const float vc = d1 * d4 - d3 * d2;
        if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) return a + ab * (d1 / (d1 - d3));   // 辺 ab の上

        const XMVECTOR cp = p - c;
        const float d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0.0f && d5 <= d6) return c;                                   // 頂点 c が一番近い

        const float vb = d5 * d2 - d1 * d6;
        if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) return a + ac * (d2 / (d2 - d6));   // 辺 ac の上

        const float va = d3 * d6 - d5 * d4;
        if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f)                       // 辺 bc の上
        {
            return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
        }

        const float denom = 1.0f / (va + vb + vc);                              // 面の内側
        return a + ab * (vb * denom) + ac * (vc * denom);
    }

    //--------------------------------------------------------------------------------
    // レイと「中心が原点・半径1の球」が最初に当たる t を求める
    //   |o + t d|^2 = 1 を t について解く（2次方程式 a t^2 + 2 b t + c = 0）
    //   tEnter : 球に入る t（レイの始点が球の中なら負）
    //--------------------------------------------------------------------------------
    bool RayUnitSphereEnter(const XMFLOAT3& o, const XMFLOAT3& d, float& tEnter)
    {
        const float a = d.x * d.x + d.y * d.y + d.z * d.z;
        const float b = o.x * d.x + o.y * d.y + o.z * d.z;
        const float c = o.x * o.x + o.y * o.y + o.z * o.z - 1.0f;
        const float disc = b * b - a * c;       // 判別式。負なら当たらない
        if (a <= 0.0f || disc < 0.0f)
        {
            return false;
        }
        tEnter = (-b - sqrtf(disc)) / a;        // 手前の交点
        return true;
    }
}

void App::Run()
{
    if (InitApp())
    {
        MainLoop();
    }
    TermApp();
}

void App::MainLoop()
{
    MSG msg = {};

    while (WM_QUIT != msg.message)
    {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE) == TRUE)
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        else
        {
            m_Timer.Tick();     // 最小化中も時間は測る（戻ったときに経過時間が一気に大きくならないように）

            // ウィンドウの大きさが変わっていたら、描画の前に作り直す
            if (m_ResizePending)
            {
                m_ResizePending = false;
                OnResize(m_PendingWidth, m_PendingHeight);
            }

            // 最小化中は描画しない。少し休んで CPU を使いすぎないようにする
            if (m_Minimized)
            {
                Sleep(10);
                continue;
            }

            Update(m_Timer.GetDeltaTime());
            Render();
        }
    }
}

// ウィンドウプロシージャ
LRESULT App::WndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wp, lp))
    {
        return true;
    }

    // この App のポインタ。CreateWindowEx の最後の引数で渡したものを、最初のメッセージ（WM_NCCREATE）で覚えておく
    // WndProc は static 関数なのでメンバ変数に直接触れないため、この方法で App を取り出す
    if (msg == WM_NCCREATE)
    {
        const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    App* app = reinterpret_cast<App*>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_DESTROY:
        PostQuitMessage(0);
        break;

    case WM_SIZE:
        // ウィンドウの大きさが変わった。ここでは予約だけして、次のフレームの最初に作り直す
        // （端をドラッグしている間は何十回も来るので、そのたびに作り直さないため）
        if (app != nullptr)
        {
            const uint32_t width  = LOWORD(lp);     // 描画される部分の幅
            const uint32_t height = HIWORD(lp);     // 描画される部分の高さ
            app->m_Minimized = (wp == SIZE_MINIMIZED) || width == 0 || height == 0;
            if (!app->m_Minimized)
            {
                app->m_PendingWidth  = width;
                app->m_PendingHeight = height;
                app->m_ResizePending = true;
            }
        }
        break;
    default:
        break;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

// 状態の変更処理、毎フレーム呼ばれる
// エディタ中とプレイ中で、やることを分ける
void App::Update(float dt)
{
    // 起動してからの時間。UV Scroll に使う（エディタ中も進むので、配置しながら流れ方を確認できる）
    m_TotalTime += dt;

    // ゲームモードのタイトル画面
    if (!m_EditorMode && m_OnTitle)
    {
        UpdateTitle(dt);
        return;
    }

    if (m_Playing)
    {
        UpdatePlay(dt);     // プレイ中：ゲームのルール
    }
    else
    {
        UpdateEditor(dt);   // エディタ中：選択・移動・カメラ・追加削除
    }
}

//------------------------------------------------------------------------------------
// エディタ中の毎フレームの処理
//------------------------------------------------------------------------------------
void App::UpdateEditor(float dt)
{
    // Inspector で予約された追加・複製・削除を、ここで実行する
    ApplyPendingEdit();

    // 右ドラッグでカメラの向きを変える。Move Camera の ON/OFF に関係なく使える
    RotateCameraByMouse();

    // 左クリックで物体を選ぶ
    PickObjectByMouse();

    HandleKeyInput(dt);

    if (m_AutoRotate)
    {
        m_RotationDeg.y += 60.0f * dt;          // 1秒に60度
        m_RotationDeg.y = fmodf(m_RotationDeg.y, 360.0f);          // 値が増え続けないように
    }
}

//------------------------------------------------------------------------------------
// プレイ中の毎フレームの処理
// ボールの物理（Step 6）・当たり判定（Step 8）などは、ここに足していく
//------------------------------------------------------------------------------------
void App::UpdatePlay(float dt)
{
    // 処理が一瞬止まったとき（ウィンドウのドラッグ中など）に dt が大きくなり、ボールがワープしないように上限をつける
    dt = (std::min)(dt, 0.05f);     // 最大 0.05 秒（20fps 相当）

    // Esc でプレイを終える。入力欄に入力中のときは反応しない
    // ゲームモードでは、Esc でゲームを終了する
    if (!ImGui::GetIO().WantCaptureKeyboard && (GetAsyncKeyState(VK_ESCAPE) & 0x8000))
    {
        if (!m_EditorMode)
        {
            QuitGame();
            return;
        }
        StopPlay();
        return;
    }

    // プレイ中は右ドラッグで視点を回さない（追従カメラの向きは固定）

    // プレイ時間を進める（Orbit の角度と、安全な位置の記録に使う）
    m_PlayTime += dt;

    // 回る物体を先に動かす。ボールの当たり判定が、このフレームの物体の位置で行われるように
    UpdateOrbits();
    UpdateSpins(dt);

    // ゴールしたあと：ボールは動かさない。Enter を押した瞬間に、最初からやり直す
    {
        const bool enterNow = !ImGui::GetIO().WantCaptureKeyboard && (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
        const bool enterPressed = enterNow && !m_PrevEnter;
        m_PrevEnter = enterNow;

        if (m_Goaled)
        {
            if (enterPressed)
            {
                StopPlay();     // 配置を元に戻して
                StartPlay();    // もう一度始める
            }
            return;
        }
    }

    // ボールを転がす
    UpdateBall(dt);

    // ボールが触れたアイテムを取る
    CollectItems();

    // Damage に触れていたら、少し前にいた安全な場所へ戻す。戻したフレームは安全な位置を記録しない
    const bool damaged = CheckDamage();

    // Goal に触れていたら、ゴールにする
    if (!damaged)
    {
        CheckGoal();
    }

    // 同じグループのアイテムを全部取ったゲートを開く
    UpdateGates(dt);

    // 床に乗っていれば、0.5 秒ごとに安全な位置を記録する
    if (!damaged)
    {
        RecordSafePoint(dt);
    }

    // HUD の一時的な文字の残り時間を減らす
    if (m_HudMessageTime > 0.0f)
    {
        m_HudMessageTime -= dt;
    }
    if (m_RespawnMessageTime > 0.0f)
    {
        m_RespawnMessageTime -= dt;
    }

    // ボールが動いたあとの位置にカメラを合わせる（順番を逆にすると、カメラが1フレーム遅れてガタつく）
    UpdateFollowCamera(dt, false);
}

//------------------------------------------------------------------------------------
// ボールを転がす（Step 6）
// 1. WASD（矢印キー）の入力を、カメラの向きを基準にした「地面と平行な向き」にする
// 2. 力 → 速度 → 位置 の順に更新する。摩擦でだんだん止まり、最高速度で頭打ちになる
// 3. 進んだ距離の分だけ、ボールを転がして見えるように回す
// 高さ（y）は変えない。壁との当たり判定は Step 8
//------------------------------------------------------------------------------------
void App::UpdateBall(float dt)
{
    if (m_PlayerIndex < 0 || m_PlayerIndex >= static_cast<int>(m_Objects.size()))
    {
        return;
    }
    GameObject& ball = m_Objects[m_PlayerIndex];

    // 1. 入力。入力欄に入力中のときは、キーを押していないことにする
    float f = 0.0f, r = 0.0f;   // 前・右への入力
    if (!ImGui::GetIO().WantCaptureKeyboard)
    {
        if ((GetAsyncKeyState('W') & 0x8000) || (GetAsyncKeyState(VK_UP)    & 0x8000)) f += 1.0f;
        if ((GetAsyncKeyState('S') & 0x8000) || (GetAsyncKeyState(VK_DOWN)  & 0x8000)) f -= 1.0f;
        if ((GetAsyncKeyState('D') & 0x8000) || (GetAsyncKeyState(VK_RIGHT) & 0x8000)) r += 1.0f;
        if ((GetAsyncKeyState('A') & 0x8000) || (GetAsyncKeyState(VK_LEFT)  & 0x8000)) r -= 1.0f;
    }

    // カメラの向きから、地面と平行な「前」と「右」を求める（MoveCamera と同じ式）
    const float yaw = XMConvertToRadians(m_CameraYaw);
    float ix = sinf(yaw) * f + cosf(yaw) * r;     // 入力の向き x
    float iz = -cosf(yaw) * f + sinf(yaw) * r;    // 入力の向き z

    // 斜めで速くならないように、長さを1にそろえる
    const float len = sqrtf(ix * ix + iz * iz);
    if (len > 1e-6f)
    {
        ix /= len;
        iz /= len;
    }

    // ジャンプ：Space を「押した瞬間」かつ地面にいるときだけ。押しっぱなしで跳ね続けないように
    const bool spaceNow = !ImGui::GetIO().WantCaptureKeyboard && (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
    if (spaceNow && !m_PrevSpace && m_OnGround)
    {
        m_BallVelY = m_PlayJumpSpeed;   // 上向きに飛び出す
        m_OnGround = false;
    }
    m_PrevSpace = spaceNow;

    // 2. 力 → 速度 → 位置（横方向）。空中でも WASD は効く
    m_BallVel.x += ix * m_PlayAccel * dt;           // 入力の向きに加速
    m_BallVel.z += iz * m_PlayAccel * dt;

    // 坂の上では、重力の「坂に沿った成分」で下り方向に加速する（坂が急なほど強い）
    //   面の向きを n とすると、横方向の加速 ＝ 重力 × n.y × (n.x, n.z)
    if (m_OnGround && m_GroundNormal.y < 0.999f)
    {
        m_BallVel.x += m_PlayGravity * m_GroundNormal.y * m_GroundNormal.x * dt;
        m_BallVel.z += m_PlayGravity * m_GroundNormal.y * m_GroundNormal.z * dt;
    }

    // 摩擦は地面に乗っているときだけ。空中で急に止まらないように
    if (m_OnGround)
    {
        const float decay = expf(-m_PlayFriction * dt); // どんな dt でも 0〜1 に収まる
        m_BallVel.x *= decay;
        m_BallVel.z *= decay;
    }

    const float speed = sqrtf(m_BallVel.x * m_BallVel.x + m_BallVel.z * m_BallVel.z);
    if (speed > m_PlayMaxSpeed && speed > 0.0f)     // 速すぎたら最高速度にそろえる
    {
        m_BallVel.x *= m_PlayMaxSpeed / speed;
        m_BallVel.z *= m_PlayMaxSpeed / speed;
    }

    // 横に動かして、Wall にぶつかったら押し戻す
    //   1フレームで大きく動くと薄い壁をすり抜けるので、半径の半分ずつに分けて「動かす → 判定」をくり返す（最大8回）
    const float collideRadius  = ball.scale.y;      // 当たり判定の半径。球のモデルは半径1なので、大きさ ＝ 半径
    // 乗り上げられる段差の高さ。半径の半分（ただし最大 0.3）。小さなボールが自分と同じ高さの段差を越えないように
    const float stepHeight     = (std::min)(0.3f, collideRadius * 0.5f);
    const float startX = ball.position.x;           // 動かす前の位置（転がる回転の計算に使う）
    const float startZ = ball.position.z;
    {
        const float moveLen = sqrtf(m_BallVel.x * m_BallVel.x + m_BallVel.z * m_BallVel.z) * dt;
        int steps = 1;
        if (collideRadius > 1e-6f)
        {
            steps = static_cast<int>(ceilf(moveLen / (collideRadius * 0.5f)));
            steps = (std::max)(1, (std::min)(8, steps));
        }
        const float subDt = dt / static_cast<float>(steps);
        for (int s = 0; s < steps; ++s)
        {
            ball.position.x += m_BallVel.x * subDt;     // 速度は押し戻しで変わるので、毎回読み直す
            ball.position.z += m_BallVel.z * subDt;
            ResolveWallCollisions(ball.position, collideRadius, stepHeight);
        }
    }
    const float dx = ball.position.x - startX;      // 押し戻されたあとに、実際に動いた距離
    const float dz = ball.position.z - startZ;

    // 3. 上下：重力で速度を減らし、高さを動かす
    const float bottomOffset = ball.scale.y;        // 球のモデルは半径1なので、中心から底までの距離 ＝ 大きさ y
    const float prevBottom   = ball.position.y - bottomOffset;    // 動かす前の底の高さ
    m_BallVelY -= m_PlayGravity * dt;
    ball.position.y += m_BallVelY * dt;
    const float newBottom    = ball.position.y - bottomOffset;    // 動かしたあとの底の高さ

    // 4. 着地の判定：真下にある面を探す
    //    レイの始点は「動かす前と後の、高い方の底 + 段差の許容量 + 横に動いた距離」
    //    ・速く落ちて薄い床をすり抜けないように、高い方の底から
    //    ・坂を上るとき、1フレームで坂の面が上がった分（45° なら横に動いた距離と同じ）だけ高くする。坂の中から始めないように
    const float horizontalMoved = sqrtf(dx * dx + dz * dz);
    float    groundY = 0.0f;
    XMFLOAT3 groundNormal(0.0f, 1.0f, 0.0f);
    const bool hasGround = FindGroundHeight(ball.position.x, ball.position.z,
                                            (std::max)(prevBottom, newBottom) + stepHeight + horizontalMoved,
                                            groundY, &groundNormal);

    // 面の上に球が接するときの「中心の高さ − 面の高さ」。平らなら半径、傾いていると半径 ÷ cos（面の向きの y）
    const float restOffset = bottomOffset / (std::max)(groundNormal.y, 0.2f);

    if (hasGround && m_BallVelY <= 0.0f && ball.position.y - restOffset <= groundY)
    {
        ball.position.y = groundY + restOffset;     // 面の上にぴったり乗せる
        m_BallVelY = 0.0f;
        m_OnGround = true;
        m_GroundNormal = groundNormal;              // 坂を転がり落ちる加速に使う
        m_CameraRefY = ball.position.y;             // カメラは着地した高さを基準にする
    }
    else
    {
        m_OnGround = false;     // 空中（ジャンプ中・落下中）
        m_GroundNormal = XMFLOAT3(0.0f, 1.0f, 0.0f);
    }

    // 5. ステージの外へ落ちたら、スタート地点に戻す
    if (ball.position.y < m_KillY)
    {
        RespawnBall(m_RespawnPos);
        return;
    }

    // 6. 転がって見える回転。地面に乗っているときだけ（空中では転がる理由がない）
    //    回す角度 = 進んだ距離 ÷ 半径。回す軸 = 上向き × 進む向き = (dz, 0, -dx) の向き
    //    回転を積み重ねるのでクォータニオンを使う（オイラー角だと、いろいろな向きに転がすとおかしくなる）
    const float dist   = sqrtf(dx * dx + dz * dz);
    const float radius = (ball.scale.x + ball.scale.y + ball.scale.z) / 3.0f;   // 球のモデルは半径1なので、大きさ ＝ 半径
    if (m_OnGround && dist > 1e-6f && radius > 1e-6f)
    {
        const XMVECTOR axis  = XMVectorSet(dz / dist, 0.0f, -dx / dist, 0.0f);
        const XMVECTOR delta = XMQuaternionRotationAxis(axis, dist / radius);
        XMVECTOR q = XMLoadFloat4(&m_BallOrientation);
        q = XMQuaternionNormalize(XMQuaternionMultiply(q, delta));   // 今の向きに、さらに delta だけ回す
        XMStoreFloat4(&m_BallOrientation, q);
    }
}

//------------------------------------------------------------------------------------
// (x, fromY, z) から真下へレイを飛ばし、当たった面のうち一番高いものを地面にする
//   対象：役割が Wall の物体（箱でも球でも）。None は通り抜ける
//   レイを物体のローカル空間（箱は1×1×1、球は半径1）に変換して判定するので、回転・伸ばした形にも乗れる
//   レイの始点が物体の中にあるとき（横から突っ込んでいるとき）は、その物体は地面にしない
//   戻り値 : 見つかれば true。groundY に上面の高さが入る
//------------------------------------------------------------------------------------
bool App::FindGroundHeight(float x, float z, float fromY, float& groundY, XMFLOAT3* groundNormal) const
{
    bool     found = false;
    float    best  = -FLT_MAX;
    XMFLOAT3 bestNormal(0.0f, 1.0f, 0.0f);     // 一番高い面の向き

    const XMVECTOR rayOrigin = XMVectorSet(x, fromY, z, 1.0f);
    const XMVECTOR rayDir    = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);   // 真下。長さ1なので t ＝ 落ちる距離

    for (size_t j = 0; j < m_Objects.size(); ++j)
    {
        const GameObject& object = m_Objects[j];
        if (static_cast<int>(j) == m_PlayerIndex || !IsSolidRole(object.role) || !object.active)
        {
            continue;   // 乗れるのは Wall だけ
        }

        if (object.model < 0 || object.model >= static_cast<int>(m_Models.size()))
        {
            continue;
        }
        const ModelRange& m = m_Models[object.model];

        const XMMATRIX world = MakeWorldMatrix(object.position, object.rotation, object.scale);
        XMVECTOR det;
        const XMMATRIX invWorld = XMMatrixInverse(&det, world);
        if (fabsf(XMVectorGetX(det)) < 1e-12f)
        {
            continue;   // 大きさ0などで逆行列が作れない
        }

        XMFLOAT3 o, d;
        XMStoreFloat3(&o, XMVector3TransformCoord(rayOrigin, invWorld));
        XMStoreFloat3(&d, XMVector3TransformNormal(rayDir, invWorld));

        // 最初に当たる t と、当たった面の向き（ワールド空間）。モデルによって判定を変える
        float    tHit = 0.0f;
        bool     hit  = false;
        XMVECTOR normal = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        if (m.useTriangles)
        {
            // 三角形：当たった三角形の数が奇数なら、レイの始点がモデルの中なので地面にしない（箱と同じ扱い）
            size_t tri = 0;
            int    hitCount = 0;
            if (RayTriangles(m.triPositions, m.triIndices, o, d, tHit, tri, hitCount) && (hitCount % 2) == 0)
            {
                hit = true;
                // 当たった三角形をワールド空間に変換して、面の向きを求める。上を向くように向きをそろえる
                const XMVECTOR a  = XMVector3TransformCoord(XMLoadFloat3(&m.triPositions[m.triIndices[tri * 3 + 0]]), world);
                const XMVECTOR bv = XMVector3TransformCoord(XMLoadFloat3(&m.triPositions[m.triIndices[tri * 3 + 1]]), world);
                const XMVECTOR cv = XMVector3TransformCoord(XMLoadFloat3(&m.triPositions[m.triIndices[tri * 3 + 2]]), world);
                normal = XMVector3Normalize(XMVector3Cross(bv - a, cv - a));
                if (XMVectorGetY(normal) < 0.0f)
                {
                    normal = -normal;
                }
            }
        }
        else if (object.model == Model_Sphere)
        {
            hit = RayUnitSphereEnter(o, d, tHit);
            if (hit)
            {
                // 球の表面の向き ＝ 中心から当たった点への向き
                const XMVECTOR hitPoint = rayOrigin + rayDir * tHit;
                normal = XMVector3Normalize(hitPoint - XMLoadFloat3(&object.position));
            }
        }
        else
        {
            float tMax = 0.0f;
            hit = RayAabbRange(o, d, m.boundsMin, m.boundsMax, tHit, tMax);
            if (hit)
            {
                // 箱の上面の向き ＝ 箱の3本の向きのうち、一番上を向いているもの（傾けた板なら、板の面の向き）
                const XMMATRIX rot = XMMatrixRotationRollPitchYaw(
                    XMConvertToRadians(object.rotation.x),
                    XMConvertToRadians(object.rotation.y),
                    XMConvertToRadians(object.rotation.z));
                int   bestAxis = 1;
                float bestUp   = 0.0f;
                for (int i = 0; i < 3; ++i)
                {
                    const float up = fabsf(XMVectorGetY(rot.r[i]));
                    if (up > bestUp)
                    {
                        bestUp   = up;
                        bestAxis = i;
                    }
                }
                normal = rot.r[bestAxis];
                if (XMVectorGetY(normal) < 0.0f)
                {
                    normal = -normal;
                }
            }
        }

        if (!hit || tHit < 0.0f)
        {
            continue;   // 当たらない、または始点が物体の中
        }

        const float top = fromY - tHit;     // 当たった面の高さ
        if (top > best)
        {
            best  = top;
            found = true;
            XMStoreFloat3(&bestNormal, normal);
        }
    }

    groundY = best;
    if (groundNormal != nullptr)
    {
        *groundNormal = bestNormal;
    }
    return found;
}

//------------------------------------------------------------------------------------
// ボール（中心 p・半径 radius）が物体にめり込んでいれば true を返す
//   n    : 押し戻す向き（長さ1）
//   push : n の向きにどれだけ押し戻せば外に出るか
//   topY : 物体の一番高いところ（上面）の高さ。段差かどうかの判定に使う
//   箱は向きのついた箱（OBB）、球は一番大きい方向の大きさを半径とする球として扱う
//   壁の当たり判定（ResolveWallCollisions）とアイテムの判定（CollectItems）の両方で使う
//------------------------------------------------------------------------------------
bool App::ComputeContact(const GameObject& object, const XMFLOAT3& p, float radius,
                         XMFLOAT3& n, float& push, float& topY) const
{
    const XMVECTOR c      = XMLoadFloat3(&p);
    const XMVECTOR center = XMLoadFloat3(&object.position);
    const XMVECTOR rel    = c - center;     // 物体の中心から見たボールの中心

    // 三角形で判定するモデル：全部の三角形の中で、ボールの中心に一番近い点を探す
    // 伸ばした・回転した物体でも距離が正しくなるように、三角形をワールド空間に変換してから調べる
    if (object.model >= 0 && object.model < static_cast<int>(m_Models.size()) && m_Models[object.model].useTriangles)
    {
        const ModelRange& m = m_Models[object.model];
        const XMMATRIX world = MakeWorldMatrix(object.position, object.rotation, object.scale);

        float    bestDist = FLT_MAX;
        XMVECTOR bestPoint = c;
        for (size_t i = 0; i + 2 < m.triIndices.size(); i += 3)
        {
            const XMVECTOR a  = XMVector3TransformCoord(XMLoadFloat3(&m.triPositions[m.triIndices[i + 0]]), world);
            const XMVECTOR bv = XMVector3TransformCoord(XMLoadFloat3(&m.triPositions[m.triIndices[i + 1]]), world);
            const XMVECTOR cv = XMVector3TransformCoord(XMLoadFloat3(&m.triPositions[m.triIndices[i + 2]]), world);
            const XMVECTOR pt = ClosestPointOnTriangle(c, a, bv, cv);
            const float dist  = XMVectorGetX(XMVector3Length(c - pt));
            if (dist < bestDist)
            {
                bestDist  = dist;
                bestPoint = pt;
            }
        }

        if (bestDist >= radius)
        {
            return false;   // 離れている
        }
        topY = XMVectorGetY(bestPoint);     // 三角形のモデルでは、当たっている点の高さを使う
        if (bestDist < 1e-6f)
        {
            n = XMFLOAT3(0.0f, 1.0f, 0.0f); // 中心が面の上にぴったり乗ったら上へ出す
            push = radius;
            return true;
        }
        XMStoreFloat3(&n, (c - bestPoint) / bestDist);
        push = radius - bestDist;
        return true;
    }

    if (object.model != Model_Sphere)
    {
        // 球以外は、モデルを囲む箱を、向きのついた箱（OBB）として扱う：中心・3本の向き・各方向の半分の長さ
        const XMMATRIX rot = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(object.rotation.x),
            XMConvertToRadians(object.rotation.y),
            XMConvertToRadians(object.rotation.z));
        if (object.model < 0 || object.model >= static_cast<int>(m_Models.size()))
        {
            return false;   // おかしなモデル番号
        }
        const ModelRange& m = m_Models[object.model];

        // 囲む箱の中心（ローカル空間）と大きさ。箱のモデルなら中心 (0,0,0)・大きさ 1×1×1
        const XMFLOAT3 localCenter((m.boundsMin.x + m.boundsMax.x) * 0.5f,
                                   (m.boundsMin.y + m.boundsMax.y) * 0.5f,
                                   (m.boundsMin.z + m.boundsMax.z) * 0.5f);
        const float half[3] =
        {
            object.scale.x * (m.boundsMax.x - m.boundsMin.x) * 0.5f,
            object.scale.y * (m.boundsMax.y - m.boundsMin.y) * 0.5f,
            object.scale.z * (m.boundsMax.z - m.boundsMin.z) * 0.5f,
        };

        // 囲む箱の中心（ワールド空間）= 物体の位置 + 回転した（大きさを掛けたローカルの中心）
        const XMVECTOR boxCenter = center + XMVector3TransformNormal(
            XMVectorSet(localCenter.x * object.scale.x, localCenter.y * object.scale.y, localCenter.z * object.scale.z, 0.0f), rot);
        const XMVECTOR boxRel = c - boxCenter;  // 囲む箱の中心から見たボールの中心

        // 上面の高さ = 中心の高さ + 3本の向きそれぞれの「上向き成分 × 半分の長さ」の合計（回転していても正しい）
        topY = XMVectorGetY(boxCenter);
        for (int i = 0; i < 3; ++i)
        {
            topY += fabsf(XMVectorGetY(rot.r[i])) * half[i];
        }

        // ボールの中心を箱の3本の向きに分解する
        float d[3];
        bool inside = true;
        for (int i = 0; i < 3; ++i)
        {
            d[i] = XMVectorGetX(XMVector3Dot(boxRel, rot.r[i]));   // rot.r[i] は回転した x・y・z の向き（長さ1）
            if (fabsf(d[i]) > half[i])
            {
                inside = false;
            }
        }

        if (!inside)
        {
            // 中心が箱の外：箱の範囲に収めて足し合わせると、表面の一番近い点になる
            XMVECTOR closest = boxCenter;
            for (int i = 0; i < 3; ++i)
            {
                closest += rot.r[i] * (std::max)(-half[i], (std::min)(half[i], d[i]));
            }
            const XMVECTOR diff = c - closest;
            const float dist = XMVectorGetX(XMVector3Length(diff));
            if (dist >= radius || dist < 1e-6f)
            {
                return false;   // 離れている
            }
            XMStoreFloat3(&n, diff / dist);
            push = radius - dist;
            return true;
        }

        // 中心が箱の中まで入った：一番近い面（めり込みが一番浅い方向）から外へ押し出す
        int   best      = 0;
        float bestDepth = FLT_MAX;
        for (int i = 0; i < 3; ++i)
        {
            const float depth = half[i] - fabsf(d[i]);   // その方向の面までの距離
            if (depth < bestDepth)
            {
                bestDepth = depth;
                best      = i;
            }
        }
        const float sign = (d[best] >= 0.0f) ? 1.0f : -1.0f;
        const XMVECTOR dir = rot.r[best] * sign;
        XMStoreFloat3(&n, dir);
        push = bestDepth + radius;                      // 面まで戻して、さらに半径の分だけ外へ
        return true;
    }

    if (object.model == Model_Sphere)
    {
        // 球：一番大きい方向の大きさを半径とする（伸ばした球は正確ではない）
        const float r   = (std::max)(object.scale.x, (std::max)(object.scale.y, object.scale.z));
        topY = object.position.y + r;           // 上面 = 中心 + 半径
        const float len = XMVectorGetX(XMVector3Length(rel));
        if (len < 1e-6f)
        {
            n = XMFLOAT3(0.0f, 1.0f, 0.0f);     // 中心が完全に重なったら上へ出す
            push = r + radius;
            return true;
        }
        const float dist = len - r;             // 球の表面までの距離。中に入っていると負
        if (dist >= radius)
        {
            return false;
        }
        XMStoreFloat3(&n, rel / len);
        push = radius - dist;
        return true;
    }

    return false;
}

//------------------------------------------------------------------------------------
// ボールが触れたアイテムを取る（Step 8・9）
//   役割が Item で、まだ取っていない物体について、ボールがめり込んでいれば「触れた」とする
//   壁と違って押し戻しはしない。取ったら active を false にして（描かない・判定しない）、数を増やす
//   配列から erase しないのは、後ろの物体の番号（プレイヤーの番号も）がずれてしまうため
//------------------------------------------------------------------------------------
void App::CollectItems()
{
    if (m_PlayerIndex < 0 || m_PlayerIndex >= static_cast<int>(m_Objects.size()))
    {
        return;
    }
    const GameObject& ball = m_Objects[m_PlayerIndex];
    const float radius = ball.scale.y;      // 当たり判定の半径（UpdateBall と同じ）

    for (auto& object : m_Objects)
    {
        if (object.role != Role_Item || !object.active)
        {
            continue;
        }

        XMFLOAT3 n;
        float push = 0.0f, topY = 0.0f;
        if (ComputeContact(object, ball.position, radius, n, push, topY))
        {
            object.active = false;      // 取った
            ++m_ItemCount;
        }
    }
}

//------------------------------------------------------------------------------------
// 安全な位置の記録
//   床に乗っている間だけ、0.5 秒ごとに今の位置を記録する。空中の位置は「安全」とは言えないので記録しない
//   古いものから消して、直近の 10 か所（約 5 秒分）だけ覚えておく
//------------------------------------------------------------------------------------
void App::RecordSafePoint(float dt)
{
    if (!m_OnGround || m_PlayerIndex < 0 || m_PlayerIndex >= static_cast<int>(m_Objects.size()))
    {
        return;
    }

    m_SafeRecordTimer += dt;
    if (m_SafeRecordTimer < 0.5f)
    {
        return;
    }
    m_SafeRecordTimer = 0.0f;

    m_SafePoints.push_back({ m_Objects[m_PlayerIndex].position, m_PlayTime });
    const size_t maxPoints = 10;
    if (m_SafePoints.size() > maxPoints)
    {
        m_SafePoints.erase(m_SafePoints.begin());   // 一番古いものを消す
    }
}

//------------------------------------------------------------------------------------
// 戻る場所：1 秒以上前に記録した、一番新しい安全な位置
//   触れる直前の位置に戻すと、すぐにまた Damage に触れてしまうので、1 秒以上前のものを使う
//   見つからなければ一番古いもの、記録が1つもなければスタート地点
//------------------------------------------------------------------------------------
XMFLOAT3 App::GetSafeRespawnPoint() const
{
    for (auto it = m_SafePoints.rbegin(); it != m_SafePoints.rend(); ++it)   // 新しいものから順に調べる
    {
        if (m_PlayTime - it->time >= 1.0f)
        {
            return it->pos;
        }
    }
    if (!m_SafePoints.empty())
    {
        return m_SafePoints.front().pos;
    }
    return m_RespawnPos;
}

//------------------------------------------------------------------------------------
// ボールを pos に戻す。Damage に触れたときと、落ちたときに使う
//   速度を 0 にして、カメラも一気に戻し、画面の真ん中に "Respawn" と表示する
//   取ったアイテムと開いたゲートはそのまま
//------------------------------------------------------------------------------------
void App::RespawnBall(const XMFLOAT3& pos)
{
    if (m_PlayerIndex < 0 || m_PlayerIndex >= static_cast<int>(m_Objects.size()))
    {
        return;
    }
    GameObject& ball = m_Objects[m_PlayerIndex];

    ball.position  = pos;
    m_BallVel      = XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_BallVelY     = 0.0f;
    m_OnGround     = false;
    m_GroundNormal = XMFLOAT3(0.0f, 1.0f, 0.0f);
    m_CameraRefY   = pos.y;
    UpdateFollowCamera(0.0f, true);             // カメラも一気に戻す

    // 記録は戻した場所からやり直す
    m_SafePoints.clear();
    m_SafePoints.push_back({ pos, m_PlayTime });
    m_SafeRecordTimer = 0.0f;

    m_RespawnMessageTime = 1.5f;                // "Respawn" を 1.5 秒表示する
}

//------------------------------------------------------------------------------------
// Damage に触れていれば、安全な位置へ戻して true を返す
//   判定はアイテムと同じ ComputeContact（箱・球・三角形のどの形でも使える）
//------------------------------------------------------------------------------------
bool App::CheckDamage()
{
    if (m_PlayerIndex < 0 || m_PlayerIndex >= static_cast<int>(m_Objects.size()))
    {
        return false;
    }
    const XMFLOAT3 ballPos = m_Objects[m_PlayerIndex].position;
    const float radius = m_Objects[m_PlayerIndex].scale.y;     // 当たり判定の半径（UpdateBall と同じ）

    for (const auto& object : m_Objects)
    {
        if (object.role != Role_Damage || !object.active)
        {
            continue;
        }

        XMFLOAT3 n;
        float push = 0.0f, topY = 0.0f;
        if (ComputeContact(object, ballPos, radius, n, push, topY))
        {
            RespawnBall(GetSafeRespawnPoint());
            return true;
        }
    }
    return false;
}

//------------------------------------------------------------------------------------
// Orbit が ON の物体を、中心の周りに回す
//   角度 ＝ プレイ時間 × Speed（度）。中心から見たスタート地点（orbitOffset）を、軸の周りにその角度だけ回す
//   Player（キー操作）と Gate（沈む動き）は、動きがぶつかるので回さない
//------------------------------------------------------------------------------------
void App::UpdateOrbits()
{
    for (auto& object : m_Objects)
    {
        if (!object.orbit || object.role == Role_Player || object.role == Role_Gate)
        {
            continue;
        }

        const float angle = XMConvertToRadians(m_PlayTime * object.orbitSpeed);
        XMMATRIX rot;
        switch (object.orbitAxis)
        {
        case 1:  rot = XMMatrixRotationX(angle); break;     // X 軸の周り（垂直な円）
        case 2:  rot = XMMatrixRotationZ(angle); break;     // Z 軸の周り（垂直な円）
        default: rot = XMMatrixRotationY(angle); break;     // Y 軸の周り（水平な円）
        }

        const XMVECTOR offset = XMVector3TransformNormal(XMLoadFloat3(&object.orbitOffset), rot);
        XMStoreFloat3(&object.position, XMLoadFloat3(&object.orbitCenter) + offset);
    }
}

//------------------------------------------------------------------------------------
// Spin が ON の物体を、その場で回す
//   Rotation の x・y・z のうち、軸に合うものに「Speed × 経過時間」を足す
//   Player はボールの転がる回転とぶつかるので回さない
//------------------------------------------------------------------------------------
void App::UpdateSpins(float dt)
{
    for (auto& object : m_Objects)
    {
        if (!object.spin || object.role == Role_Player)
        {
            continue;
        }

        float* angle = nullptr;     // 足す先の角度
        switch (object.spinAxis)
        {
        case 1:  angle = &object.rotation.x; break;     // X 軸の周り（縦に回る）
        case 2:  angle = &object.rotation.z; break;     // Z 軸の周り（縦に回る）
        default: angle = &object.rotation.y; break;     // Y 軸の周り（横に回る）
        }
        *angle = fmodf(*angle + object.spinSpeed * dt, 360.0f);    // 値が増え続けないように
    }
}

//------------------------------------------------------------------------------------
// Goal に触れていれば、ゴールにする
//   判定はアイテムと同じ ComputeContact。アイテムを集めていなくてもゴールできる
//   ゴールしたら、クリアタイムを覚えて、ボールを止める
//------------------------------------------------------------------------------------
void App::CheckGoal()
{
    if (m_Goaled || m_PlayerIndex < 0 || m_PlayerIndex >= static_cast<int>(m_Objects.size()))
    {
        return;
    }
    const XMFLOAT3 ballPos = m_Objects[m_PlayerIndex].position;
    const float radius = m_Objects[m_PlayerIndex].scale.y;     // 当たり判定の半径（UpdateBall と同じ）

    for (const auto& object : m_Objects)
    {
        if (object.role != Role_Goal || !object.active)
        {
            continue;
        }

        XMFLOAT3 n;
        float push = 0.0f, topY = 0.0f;
        if (ComputeContact(object, ballPos, radius, n, push, topY))
        {
            m_Goaled    = true;
            m_ClearTime = m_PlayTime;
            m_BallVel   = XMFLOAT3(0.0f, 0.0f, 0.0f);   // ボールを止める
            m_BallVelY  = 0.0f;
            return;
        }
    }
}

//------------------------------------------------------------------------------------
// ゲートを開く
//   1. 閉じているゲートごとに、同じグループ番号のアイテムを数える
//      アイテムが1つ以上あり、全部取ってあれば開き始める（グループ 0 と、アイテムのないグループは開かない）
//   2. 開き始めたゲートは、GateOpenSeconds 秒かけて、自分の高さの分だけ下へ沈む
//      動き始めと終わりがなめらかになるように、smoothstep（3t² − 2t³）で動かす
//------------------------------------------------------------------------------------
void App::UpdateGates(float dt)
{
    for (auto& gate : m_Objects)
    {
        if (gate.role != Role_Gate)
        {
            continue;
        }

        // 1. まだ閉じていれば、開く条件を調べる
        if (gate.gateTime < 0.0f)
        {
            if (gate.group == 0)
            {
                continue;   // どのグループにも属さないゲートは開かない
            }
            int total = 0, collected = 0;
            for (const auto& item : m_Objects)
            {
                if (item.role == Role_Item && item.group == gate.group)
                {
                    ++total;
                    if (!item.active)
                    {
                        ++collected;    // 取ったアイテムは active が false
                    }
                }
            }
            if (total == 0 || collected < total)
            {
                continue;   // まだ開かない
            }

            gate.gateTime = 0.0f;   // 開き始める
            m_HudMessage     = "Gate opened! (Group " + std::to_string(gate.group) + ")";
            m_HudMessageTime = 2.0f;
        }

        // 2. 沈める。沈む深さ ＝ ゲートの高さ（モデルの高さ × 大きさ y）
        gate.gateTime += dt;
        float t = gate.gateTime / GateOpenSeconds;
        t = (std::max)(0.0f, (std::min)(1.0f, t));
        const float ease = t * t * (3.0f - 2.0f * t);      // smoothstep：0 → 1 になめらかに変わる

        float modelHeight = 1.0f;
        if (gate.model >= 0 && gate.model < static_cast<int>(m_Models.size()))
        {
            modelHeight = m_Models[gate.model].boundsMax.y - m_Models[gate.model].boundsMin.y;
        }
        const float depth = modelHeight * gate.scale.y;
        gate.position.y = gate.gateBaseY - depth * ease;
    }
}

//------------------------------------------------------------------------------------
// Wall との当たり判定（Step 8）
//   Wall の物体ごとに「めり込んでいるか」と「押し戻す向き・量」を求め、向きで扱いを変える
//     ・物体の上面が「ボールの底 + 段差」より低い：床・段差なので何もしない（着地の処理にまかせる）
//     ・ほぼ真下向き：物体の下面に頭をぶつけた。下へ押し戻し、上向きの速度を消す
//     ・それ以外（横・斜め）：横方向にだけ押し戻し、壁に向かう速度を消す（Bounce の分だけ跳ね返す）
//   壁の角では、1枚目から押し出した先が2枚目の中ということがあるので、めり込みがなくなるまで最大4回くり返す
//   pos        : ボールの中心。押し戻された位置に書き換わる
//   radius     : ボールの半径
//   stepHeight : 乗り上げられる段差の高さ
//------------------------------------------------------------------------------------
void App::ResolveWallCollisions(XMFLOAT3& pos, float radius, float stepHeight)
{
    const int maxIterations = 4;
    for (int iter = 0; iter < maxIterations; ++iter)
    {
        bool anyHit = false;

        for (size_t j = 0; j < m_Objects.size(); ++j)
        {
            const GameObject& object = m_Objects[j];
            if (static_cast<int>(j) == m_PlayerIndex || !IsSolidRole(object.role) || !object.active)
            {
                continue;   // 当たるのは Wall だけ
            }

            XMFLOAT3 n;
            float push = 0.0f, topY = 0.0f;
            if (!ComputeContact(object, pos, radius, n, push, topY))
            {
                continue;
            }

            // 三角形のモデルで、上れる傾き（Max Slope 以下）の面なら坂なので押し戻さない（着地の処理にまかせる）
            if (object.model >= 0 && object.model < static_cast<int>(m_Models.size()) && m_Models[object.model].useTriangles
                && n.y >= cosf(XMConvertToRadians(m_PlayMaxSlope)))
            {
                continue;
            }

            // 床・段差：物体の上面がボールの底 + 段差 以下なら、乗り上げられるので押し戻さない
            const float bottom = pos.y - radius;
            if (topY <= bottom + stepHeight && n.y > -0.7f)
            {
                continue;
            }
            anyHit = true;

            // ほぼ真下向き：物体の下面に頭をぶつけた
            if (n.y < -0.7f)
            {
                pos.x += n.x * push;
                pos.y += n.y * push;
                pos.z += n.z * push;
                if (m_BallVelY > 0.0f)
                {
                    m_BallVelY = 0.0f;
                }
                continue;
            }

            // 横・斜め：横方向にだけ押し戻す
            float hx = n.x, hz = n.z;
            const float hLen = sqrtf(hx * hx + hz * hz);
            if (hLen < 1e-6f)
            {
                continue;
            }
            hx /= hLen;
            hz /= hLen;
            const float hPush = (std::min)(push / hLen, push + radius);   // 押し戻しすぎないように上限をつける
            pos.x += hx * hPush;
            pos.z += hz * hPush;

            // 壁に向かう速度を消す。Bounce の分だけ反対向きに跳ね返す
            const float vn = m_BallVel.x * hx + m_BallVel.z * hz;   // 壁に向かう速さ
            if (vn < 0.0f)
            {
                m_BallVel.x -= hx * vn * (1.0f + m_PlayBounce);
                m_BallVel.z -= hz * vn * (1.0f + m_PlayBounce);
            }
        }

        if (!anyHit)
        {
            break;      // めり込みがなくなった
        }
    }
}

//------------------------------------------------------------------------------------
// 追従カメラ（Step 7）
//   カメラの位置 = ボールの位置 + 決まったずらし量（m_FollowOffset）
//   カメラの向き = 目標の位置から注視点（ボールの少し上）への向き。ずらし量が同じなら向きも同じ（固定）
//   snap が false なら、目標の位置へ少し遅れてなめらかに近づく
//------------------------------------------------------------------------------------
void App::UpdateFollowCamera(float dt, bool snap)
{
    if (!m_FollowCamera || m_PlayerIndex < 0 || m_PlayerIndex >= static_cast<int>(m_Objects.size()))
    {
        return;
    }
    const GameObject& ball = m_Objects[m_PlayerIndex];

    // 1. 目標の位置と注視点
    //    高さはボールの今の高さではなく、着地したときの高さ（m_CameraRefY）を使う。ジャンプでカメラが上下しないように
    const XMFLOAT3 desired(
        ball.position.x + m_FollowOffset.x,
        m_CameraRefY    + m_FollowOffset.y,
        ball.position.z + m_FollowOffset.z);
    const XMFLOAT3 target(ball.position.x, m_CameraRefY + m_FollowHeight, ball.position.z);

    // 2. 向き：目標の位置 → 注視点 のベクトルから yaw・pitch を逆算する（ResetCamera と同じ計算）
    //    カメラの位置が遅れていても向きは変えないので、画面が揺れない
    const float dx = target.x - desired.x;
    const float dy = target.y - desired.y;
    const float dz = target.z - desired.z;
    const float horizontal = sqrtf(dx * dx + dz * dz);
    if (horizontal > 1e-6f || fabsf(dy) > 1e-6f)
    {
        m_CameraYaw   = XMConvertToDegrees(atan2f(dx, -dz));
        m_CameraPitch = XMConvertToDegrees(atan2f(dy, horizontal));
        m_CameraPitch = (std::max)(-89.0f, (std::min)(89.0f, m_CameraPitch));   // 真上・真下を向くと計算が壊れるので止める
    }

    // 3. 位置：一気に合わせるか、なめらかに近づく
    if (snap || m_FollowSmooth <= 0.0f)
    {
        m_CameraPos = desired;
        return;
    }

    // 毎フレーム、残りの距離の一部だけ近づく。expf を使うので、フレームレートが変わっても追いつき方がほぼ同じ
    const float t = 1.0f - expf(-m_FollowSmooth * dt);
    XMStoreFloat3(&m_CameraPos, XMVectorLerp(XMLoadFloat3(&m_CameraPos), XMLoadFloat3(&desired), t));
}

//------------------------------------------------------------------------------------
// プレイを始める
// 1. 役割が Player の物体を探す。いなければ始めない。2個以上なら番号の小さい方を使う
// 2. 今の配置・選択・カメラを丸ごとコピーして取っておく（StopPlay で元に戻す）
//------------------------------------------------------------------------------------
bool App::StartPlay()
{
    if (m_Playing)
    {
        return true;
    }

    // 1. プレイヤーを探す
    m_PlayerIndex = -1;
    for (size_t j = 0; j < m_Objects.size(); ++j)
    {
        if (m_Objects[j].role == Role_Player)
        {
            m_PlayerIndex = static_cast<int>(j);
            break;
        }
    }
    if (m_PlayerIndex < 0)
    {
        m_PlayMessage = "No player (set Role to Player)";
        return false;
    }

    // 2. 今の状態を取っておく
    m_EditSnapshot        = m_Objects;          // vector はこれで中身ごとコピーされる
    m_SnapshotSelected    = m_SelectedObject;
    m_SnapshotCameraPos   = m_CameraPos;
    m_SnapshotCameraYaw   = m_CameraYaw;
    m_SnapshotCameraPitch = m_CameraPitch;

    // ボールの速度を 0 にし、向きは配置したときの回転から始める
    m_BallVel = XMFLOAT3(0.0f, 0.0f, 0.0f);
    {
        const GameObject& ball = m_Objects[m_PlayerIndex];

        // ジャンプと落下の準備
        m_BallVelY   = 0.0f;
        m_OnGround   = false;           // 最初のフレームで着地の判定をする
        m_GroundNormal = XMFLOAT3(0.0f, 1.0f, 0.0f);
        m_PrevSpace  = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;   // 押したまま始めても、すぐ跳ばないように
        m_RespawnPos = ball.position;   // 落ちたらここに戻る
        m_KillY      = ball.position.y - 20.0f;
        m_CameraRefY = ball.position.y;

        XMStoreFloat4(&m_BallOrientation, XMQuaternionRotationRollPitchYaw(
            XMConvertToRadians(ball.rotation.x),
            XMConvertToRadians(ball.rotation.y),
            XMConvertToRadians(ball.rotation.z)));
    }

    // アイテムの数を数える。全部まだ取っていない状態から始める
    m_ItemCount = 0;
    m_ItemTotal = 0;
    for (auto& object : m_Objects)
    {
        object.active = true;
        if (object.role == Role_Item)
        {
            ++m_ItemTotal;
        }
        // ゲートは閉じた状態から始める。開くときに沈める基準として、今の高さを覚える
        object.gateBaseY = object.position.y;
        object.gateTime  = -1.0f;
        // 回る物体は、中心から見たスタート地点を覚える（置いた位置から回り始める）
        object.orbitOffset = XMFLOAT3(
            object.position.x - object.orbitCenter.x,
            object.position.y - object.orbitCenter.y,
            object.position.z - object.orbitCenter.z);
    }
    m_HudMessage.clear();
    m_HudMessageTime = 0.0f;

    // 安全な位置の記録を、スタート地点から始める
    m_PlayTime = 0.0f;
    m_SafeRecordTimer = 0.0f;
    m_RespawnMessageTime = 0.0f;

    // ゴールしていない状態から始める。Enter を押したまま始めても、すぐやり直しにならないように今の状態を覚える
    m_Goaled    = false;
    m_ClearTime = 0.0f;
    m_PrevEnter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    m_SafePoints.clear();
    m_SafePoints.push_back({ m_RespawnPos, 0.0f });

    m_SelectedObject = -1;      // プレイ中は何も選ばない
    m_MouseLooking   = false;
    m_PlayMessage.clear();
    m_Playing = true;
    m_InspectorCollapse = 1;    // プレイ中は Inspector を折りたたんで、左上のタイムが見えるようにする

    // 追従カメラを、ボールの斜め上の後ろへ一気に移動する
    UpdateFollowCamera(0.0f, true);
    return true;
}

//------------------------------------------------------------------------------------
// プレイを終えて、プレイ開始前の配置・選択・カメラに戻す
//------------------------------------------------------------------------------------
void App::StopPlay()
{
    if (!m_Playing)
    {
        return;
    }

    m_Objects        = m_EditSnapshot;
    m_SelectedObject = m_SnapshotSelected;
    m_CameraPos      = m_SnapshotCameraPos;
    m_CameraYaw      = m_SnapshotCameraYaw;
    m_CameraPitch    = m_SnapshotCameraPitch;

    m_EditSnapshot.clear();
    m_BallVel  = XMFLOAT3(0.0f, 0.0f, 0.0f);
    m_BallVelY = 0.0f;
    m_OnGround = false;
    m_PlayerIndex  = -1;
    m_MouseLooking = false;
    m_Playing      = false;
    m_InspectorCollapse = 2;    // エディタに戻ったら Inspector を開く
}
void App::HandleKeyInput(float dt)
{
    // Inspector の数値欄などに文字を入力している間は、物体を動かさない
    if (ImGui::GetIO().WantCaptureKeyboard)
    {
        return;
    }

    // Esc で選択を外す
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
    {
        m_SelectedObject = -1;
    }

    // カメラモードなら、物体ではなくカメラを動かす
    if (m_CameraMode)
    {
        MoveCamera(dt);
        return;     // 物体は動かさない
    }

    if (m_SelectedObject < 0
        || static_cast<size_t>(m_SelectedObject) >= m_Objects.size())
    {
        return;     // 何も選んでいないので動かすものがない
    }

    float ix = 0.0f;
    float iy = 0.0f;
    float iz = 0.0f;

    if ((GetAsyncKeyState('A') & 0x8000) ||
        (GetAsyncKeyState(VK_LEFT) & 0x8000)) ix -= 1.0f;

    if ((GetAsyncKeyState('D') & 0x8000) ||
        (GetAsyncKeyState(VK_RIGHT) & 0x8000)) ix += 1.0f;

    if ((GetAsyncKeyState('S') & 0x8000) ||
        (GetAsyncKeyState(VK_DOWN) & 0x8000)) iz += 1.0f;

    if ((GetAsyncKeyState('W') & 0x8000) ||
        (GetAsyncKeyState(VK_UP) & 0x8000)) iz -= 1.0f;

    // Q で下、E で上
    if (GetAsyncKeyState('Q') & 0x8000) iy -= 1.0f;
    if (GetAsyncKeyState('E') & 0x8000) iy += 1.0f;

    // Inspectorで選択中の物体を参照する
    GameObject& object = m_Objects[m_SelectedObject];

    // Shift を押している間は、微調整用にゆっくり動かす
    const float baseSpeed = 3.0f;       // 普段の速さ（1秒に3）
    const float slowRate  = 0.2f;       // Shift 中は普段の何倍か（0.2倍 = 1秒に0.6）
    const bool  slow      = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const float speed     = slow ? baseSpeed * slowRate : baseSpeed;

    object.position.x += ix * speed * dt;
    object.position.y += iy * speed * dt;
    object.position.z += iz * speed * dt;
}

//------------------------------------------------------------------------------------
// カメラが向いている方向（長さ1）
// yaw = 0, pitch = 0 のとき -Z（奥）を向く。yaw を増やすと右、pitch を増やすと上を向く
//------------------------------------------------------------------------------------
XMVECTOR App::GetCameraForward() const
{
    const float yaw   = XMConvertToRadians(m_CameraYaw);
    const float pitch = XMConvertToRadians(m_CameraPitch);
    return XMVectorSet(
        cosf(pitch) * sinf(yaw),    // x：右向きの成分
        sinf(pitch),                // y：上向きの成分
       -cosf(pitch) * cosf(yaw),    // z：奥（-Z）向きの成分
        0.0f);
}

//------------------------------------------------------------------------------------
// カメラを最初の位置に戻し、最初に見る点（原点）を向かせる
// 向きは「位置 → 見る点」のベクトルから yaw と pitch を逆算する
//------------------------------------------------------------------------------------
void App::ResetCamera()
{
    // 定数をそのまま代入せず、要素を取り出して作り直す（C++14 で static constexpr をリンクエラーにしないため）
    m_CameraPos = XMFLOAT3(DefaultCameraPos.x, DefaultCameraPos.y, DefaultCameraPos.z);

    const float dx = DefaultCameraTarget.x - DefaultCameraPos.x;
    const float dy = DefaultCameraTarget.y - DefaultCameraPos.y;
    const float dz = DefaultCameraTarget.z - DefaultCameraPos.z;
    const float horizontal = sqrtf(dx * dx + dz * dz);              // 水平方向の距離

    m_CameraYaw   = XMConvertToDegrees(atan2f(dx, -dz));             // GetCameraForward の式の逆
    m_CameraPitch = XMConvertToDegrees(atan2f(dy, horizontal));      // (0,6,12) → 原点なら約 -26.6 度
}

//------------------------------------------------------------------------------------
// 右ボタンを押しながらマウスを動かすと、カメラの向きを変える
//   横に動かした分を Yaw に、縦に動かした分を Pitch に足す。矢印キーと同じ変数を使う
//   Inspector の上で押し始めたドラッグでは回さない
//   シーンの上で押し始めたドラッグは、途中で Inspector の上を通っても回し続ける
//------------------------------------------------------------------------------------
void App::RotateCameraByMouse()
{
    const ImGuiIO& io = ImGui::GetIO();

    // 1. ドラッグの開始：押した瞬間に、マウスが ImGui のウィンドウの上になければ開始する
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !io.WantCaptureMouse)
    {
        m_MouseLooking = true;
    }

    // 2. ドラッグの終了：右ボタンを離したら終わり
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
    {
        m_MouseLooking = false;
    }

    if (!m_MouseLooking)
    {
        return;
    }

    // 3. 動いた分だけ回す。Shift を押している間は 0.2 倍
    const bool  slow        = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const float sensitivity = m_MouseSensitivity * (slow ? 0.2f : 1.0f);

    m_CameraYaw   += io.MouseDelta.x * sensitivity;     // 右に動かすと右を向く
    m_CameraPitch -= io.MouseDelta.y * sensitivity;     // 上に動かすと上を向く（画面の y は下向きが正なので引く）

    m_CameraYaw   = fmodf(m_CameraYaw, 360.0f);                           // 値が増え続けないように
    m_CameraPitch = (std::max)(-89.0f, (std::min)(89.0f, m_CameraPitch)); // 真上・真下を向くと計算が壊れるので止める
}

//------------------------------------------------------------------------------------
// ビュー行列：カメラの位置と向きから作る
//------------------------------------------------------------------------------------
XMMATRIX App::GetViewMatrix() const
{
    return XMMatrixLookToRH(
        XMLoadFloat3(&m_CameraPos),             // カメラの位置
        GetCameraForward(),                     // 向いている方向
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));   // 上方向
}

//------------------------------------------------------------------------------------
// 射影行列：視野角と画面の縦横比から作る
//------------------------------------------------------------------------------------
XMMATRIX App::GetProjMatrix() const
{
    const float aspect = static_cast<float>(m_Width) / static_cast<float>(m_Height);
    return XMMatrixPerspectiveFovRH(XMConvertToRadians(CameraFovYDeg), aspect, CameraNear, CameraFar);
}

namespace
{
    //--------------------------------------------------------------------------------
    // レイと「中心が原点・半径1の球」の交差判定（ローカル空間で使う）
    //   レイ上の点 = o + t * d。当たったら一番手前の t を返す
    //   |o + t d|^2 = 1 を t について解く（2次方程式 a t^2 + 2 b t + c = 0）
    //--------------------------------------------------------------------------------
    bool RayUnitSphere(const XMFLOAT3& o, const XMFLOAT3& d, float& tHit)
    {
        const float a = d.x * d.x + d.y * d.y + d.z * d.z;
        const float b = o.x * d.x + o.y * d.y + o.z * d.z;
        const float c = o.x * o.x + o.y * o.y + o.z * o.z - 1.0f;
        const float disc = b * b - a * c;           // 判別式。負なら当たらない
        if (a <= 0.0f || disc < 0.0f)
        {
            return false;
        }
        const float s  = sqrtf(disc);
        const float t0 = (-b - s) / a;              // 手前の交点
        const float t1 = (-b + s) / a;              // 奥の交点
        if (t1 < 0.0f)
        {
            return false;                           // 球がカメラより後ろにある
        }
        tHit = (t0 >= 0.0f) ? t0 : t1;              // カメラが球の中にいるときは奥の交点
        return true;
    }
}

//------------------------------------------------------------------------------------
// 左クリックした物体を選ぶ
// 1. クリックした位置から、カメラの奥へ伸びるレイを作る
// 2. 全部の物体について、レイを物体のローカル空間（拡大・回転する前の形）に変換して当たりを調べる
// 3. 一番手前で当たった物体を選ぶ。どれにも当たらなければ選択を外す
//------------------------------------------------------------------------------------
void App::PickObjectByMouse()
{
    const ImGuiIO& io = ImGui::GetIO();

    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || io.WantCaptureMouse)
    {
        return;
    }

    // 1. レイを作る：マウスの位置を手前と奥の2点として3D空間に戻す
    const XMMATRIX view = GetViewMatrix();
    const XMMATRIX proj = GetProjMatrix();
    const float w  = static_cast<float>(m_Width);
    const float h  = static_cast<float>(m_Height);
    const float mx = io.MousePos.x;
    const float my = io.MousePos.y;

    const XMVECTOR nearPt = XMVector3Unproject(XMVectorSet(mx, my, 0.0f, 1.0f), 0.0f, 0.0f, w, h, 0.0f, 1.0f, proj, view, XMMatrixIdentity());
    const XMVECTOR farPt  = XMVector3Unproject(XMVectorSet(mx, my, 1.0f, 1.0f), 0.0f, 0.0f, w, h, 0.0f, 1.0f, proj, view, XMMatrixIdentity());
    const XMVECTOR rayOrigin = nearPt;              // レイの始点
    const XMVECTOR rayDir    = farPt - nearPt;      // レイの向き（長さはそのまま。t の意味が全物体で同じになる）

    // 2. 全部の物体を調べて、一番手前で当たったものを探す
    int   hitIndex = -1;
    float hitT     = FLT_MAX;

    for (size_t j = 0; j < m_Objects.size(); ++j)
    {
        const GameObject& object = m_Objects[j];

        // ワールド行列（描画と同じ作り方）とその逆行列
        const XMMATRIX world =
            XMMatrixScaling(object.scale.x, object.scale.y, object.scale.z) *
            XMMatrixRotationRollPitchYaw(
                XMConvertToRadians(object.rotation.x),
                XMConvertToRadians(object.rotation.y),
                XMConvertToRadians(object.rotation.z)) *
            XMMatrixTranslation(object.position.x, object.position.y, object.position.z);
        XMVECTOR det;
        const XMMATRIX invWorld = XMMatrixInverse(&det, world);
        if (fabsf(XMVectorGetX(det)) < 1e-12f)
        {
            continue;   // 大きさ0などで逆行列が作れない物体は飛ばす
        }

        // レイをローカル空間へ。始点は「点」なので位置ごと、向きは「方向」なので回転・拡大だけ変換する
        XMFLOAT3 o, d;
        XMStoreFloat3(&o, XMVector3TransformCoord(rayOrigin, invWorld));
        XMStoreFloat3(&d, XMVector3TransformNormal(rayDir, invWorld));

        // 三角形のモデルは三角形で、球は球として、それ以外はモデルを囲む箱として判定する
        float t = 0.0f;
        bool  hit = false;
        if (object.model >= 0 && object.model < static_cast<int>(m_Models.size()) && m_Models[object.model].useTriangles)
        {
            const ModelRange& m = m_Models[object.model];
            size_t tri = 0;
            int    hitCount = 0;
            hit = RayTriangles(m.triPositions, m.triIndices, o, d, t, tri, hitCount);
        }
        else if (object.model == Model_Sphere)
        {
            hit = RayUnitSphere(o, d, t);
        }
        else if (object.model >= 0 && object.model < static_cast<int>(m_Models.size()))
        {
            const ModelRange& m = m_Models[object.model];
            float tMin = 0.0f, tMax = 0.0f;
            if (RayAabbRange(o, d, m.boundsMin, m.boundsMax, tMin, tMax))
            {
                t   = (tMin >= 0.0f) ? tMin : tMax;     // カメラが箱の中にいるときは出口
                hit = true;
            }
        }

        if (hit && t < hitT)
        {
            hitT     = t;
            hitIndex = static_cast<int>(j);
        }
    }

    // 3. 一番手前の物体を選ぶ。どれにも当たらなければ -1（未選択）
    m_SelectedObject = hitIndex;
}

//------------------------------------------------------------------------------------
// カメラモードのときのキー操作
//   W/S : 前後  A/D : 左右  Q/E : 下・上（前後左右は地面と平行に動く）
//   ← → : 左右を向く  ↑ ↓ : 上下を向く
//   Shift を押している間はゆっくり
//------------------------------------------------------------------------------------
void App::MoveCamera(float dt)
{
    const bool  slow      = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const float rate      = slow ? 0.2f : 1.0f;     // Shift 中は 0.2 倍
    const float moveSpeed = 6.0f  * rate;           // 1秒に進む距離
    const float turnSpeed = 90.0f * rate;           // 1秒に回る角度（度）

    // 1. 向きを変える：矢印キー
    if (GetAsyncKeyState(VK_LEFT)  & 0x8000) m_CameraYaw   -= turnSpeed * dt;
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) m_CameraYaw   += turnSpeed * dt;
    if (GetAsyncKeyState(VK_UP)    & 0x8000) m_CameraPitch += turnSpeed * dt;
    if (GetAsyncKeyState(VK_DOWN)  & 0x8000) m_CameraPitch -= turnSpeed * dt;

    m_CameraYaw   = fmodf(m_CameraYaw, 360.0f);                          // 値が増え続けないように
    m_CameraPitch = (std::max)(-89.0f, (std::min)(89.0f, m_CameraPitch)); // 真上・真下を向くと計算が壊れるので止める

    // 2. 動く方向：地面と平行な「前」と「右」
    const float yaw = XMConvertToRadians(m_CameraYaw);
    const float forwardX = sinf(yaw), forwardZ = -cosf(yaw);    // 前（上下の傾きは無視する）
    const float rightX   = cosf(yaw), rightZ   =  sinf(yaw);    // 右

    float f = 0.0f, r = 0.0f, u = 0.0f;     // 前・右・上 への入力
    if (GetAsyncKeyState('W') & 0x8000) f += 1.0f;
    if (GetAsyncKeyState('S') & 0x8000) f -= 1.0f;
    if (GetAsyncKeyState('D') & 0x8000) r += 1.0f;
    if (GetAsyncKeyState('A') & 0x8000) r -= 1.0f;
    if (GetAsyncKeyState('E') & 0x8000) u += 1.0f;
    if (GetAsyncKeyState('Q') & 0x8000) u -= 1.0f;

    // 3. 位置を動かす
    m_CameraPos.x += (forwardX * f + rightX * r) * moveSpeed * dt;
    m_CameraPos.z += (forwardZ * f + rightZ * r) * moveSpeed * dt;
    m_CameraPos.y += u * moveSpeed * dt;
}

//------------------------------------------------------------------------------------
// 選択中の物体の隣（+x側）で、重ならない位置を返す
//   新しい x = 選択中の x + 選択中の幅の半分 + 新しい物体の幅の半分 + すき間
//   y と z は選択中の物体と同じ。物体が1個もないときは原点
//------------------------------------------------------------------------------------
XMFLOAT3 App::GetPositionNextToSelected(const XMFLOAT3& newScale) const
{
    if (m_SelectedObject < 0
        || static_cast<size_t>(m_SelectedObject) >= m_Objects.size())
    {
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    const GameObject& selected = m_Objects[m_SelectedObject];
    const float gap = 0.5f;     // 物体どうしのすき間

    XMFLOAT3 pos = selected.position;
    pos.x += selected.scale.x * 0.5f + newScale.x * 0.5f + gap;
    return pos;
}

//------------------------------------------------------------------------------------
// Inspector のボタンで予約された、追加・複製・削除を実行する
// Update の最初に呼ぶ。ここでは m_Objects の要素への参照を誰も持っていないので安全
//------------------------------------------------------------------------------------
void App::ApplyPendingEdit()
{
    const EditRequest request = m_PendingEdit;
    m_PendingEdit = Edit_None;      // 予約は1回で消す

    const bool hasSelection =
        m_SelectedObject >= 0 && static_cast<size_t>(m_SelectedObject) < m_Objects.size();

    switch (request)
    {
    case Edit_AddSphere:
    case Edit_AddBox:
    {
        // 新しい物体。大きさ1・回転なしで、選択中の物体の隣に置く
        GameObject object;
        object.model    = (request == Edit_AddSphere) ? Model_Sphere : Model_Box;
        object.position = GetPositionNextToSelected(object.scale);

        if (AddObject(object))
        {
            m_SelectedObject   = static_cast<int>(m_Objects.size()) - 1;    // 追加した物体を選ぶ
            m_EditLimitReached = false;
        }
        else
        {
            m_EditLimitReached = true;      // 上限で追加できなかった
        }
        break;
    }

    case Edit_Duplicate:
    {
        if (!hasSelection) { break; }

        // 選択中の物体をそのままコピーし、位置だけ隣にずらす
        GameObject copy = m_Objects[m_SelectedObject];
        copy.position = GetPositionNextToSelected(copy.scale);

        if (AddObject(copy))
        {
            m_SelectedObject   = static_cast<int>(m_Objects.size()) - 1;    // 複製した物体を選ぶ
            m_EditLimitReached = false;
        }
        else
        {
            m_EditLimitReached = true;
        }
        break;
    }

    case Edit_Delete:
    {
        if (!hasSelection) { break; }

        m_Objects.erase(m_Objects.begin() + m_SelectedObject);

        // 1つ前の番号を選ぶ。0番を消したときは 0番のまま。全部消えたら未選択（-1）にする
        if (m_SelectedObject > 0)
        {
            --m_SelectedObject;
        }
        if (m_Objects.empty())
        {
            m_SelectedObject = -1;
        }
        m_EditLimitReached = false;     // 数が減ったので、また追加できる
        break;
    }

    case Edit_Load:
    {
        // 保存した配置に戻す。失敗したときは今の配置のまま
        LoadScene();
        m_EditLimitReached = false;
        break;
    }

    case Edit_None:
    default:
        break;
    }
}

//------------------------------------------------------------------------------------
//                            描画　処理
//------------------------------------------------------------------------------------
void App::Render()
{
    // コマンドの記録の開始
    // 使い終わったメモリの再利用準備
    _cmdAllocator[m_FrameIndex]->Reset();
    _cmdList->Reset(_cmdAllocator[m_FrameIndex].Get(), nullptr);
    // リソースバリアの設定：表示用から描画用に切り替える
    // 例
    // バックバッファに三角形を描き込む処理A
    // Present()で表示エンジンがそのバックバッファを画面に表示する処理B
    // その処理Aと処理Bが同時に行われないためにリソースバリアは必要
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; // 使用用途の遷移
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = _colorBuffer[m_FrameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // リソースバリアの生成
    _cmdList->ResourceBarrier(1, &barrier);

    // レンダーターゲットの設定
    _cmdList->OMSetRenderTargets(1, &m_HandleRTV[m_FrameIndex], FALSE, &m_HandleDSV);
    _cmdList->ClearDepthStencilView(m_HandleDSV, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    // レンダーターゲットビューをクリアする。色はInspectorで編集した値を使う
    _cmdList->ClearRenderTargetView(m_HandleRTV[m_FrameIndex], m_ClearColor, 0, nullptr);

    // 描く数。定数バッファに入る数（MaxObjects）を超えて書き込まないように、少ない方に合わせる
    // ※ std::min を () で囲むのは、Windows.h の min マクロと衝突しないようにするため
    const size_t drawCount = (std::min)(m_Objects.size(), static_cast<size_t>(MaxObjects));

    // カメラ：ビュー行列と射影行列。全物体で共通。ピッキングと同じ関数で作る
    const XMMATRIX view = GetViewMatrix();
    const XMMATRIX proj = GetProjMatrix();

    for (size_t j = 0; j < drawCount; ++j)
    {
        const GameObject& object = m_Objects[j];

        XMMATRIX S = XMMatrixScaling(
            object.scale.x,
            object.scale.y,
            object.scale.z);

        XMMATRIX R = XMMatrixRotationRollPitchYaw(
            XMConvertToRadians(object.rotation.x),
            XMConvertToRadians(object.rotation.y),
            XMConvertToRadians(object.rotation.z));

        // プレイ中のボールだけは、転がした向き（クォータニオン）で回す
        if (m_Playing && static_cast<int>(j) == m_PlayerIndex)
        {
            R = XMMatrixRotationQuaternion(XMLoadFloat4(&m_BallOrientation));
        }

        XMMATRIX T = XMMatrixTranslation(
            object.position.x,
            object.position.y,
            object.position.z);

        XMMATRIX world = S * R * T;

        // この物体専用の領域へ書き込む
        Transform& transform = m_pTransform[m_FrameIndex][j];

        transform.World = world;
        transform.View  = view;     // カメラは全物体で共通
        transform.Proj  = proj;
        transform.Color = object.color;     // 物体ごとの色。PS でテクスチャの色に掛ける

        // テクスチャのくり返しとずらし。ずらす量は 0〜1 にしておく（数が大きくなりすぎて精度が落ちないように）
        const float offsetU = fmodf(object.uvScroll.x * m_TotalTime, 1.0f);
        const float offsetV = fmodf(object.uvScroll.y * m_TotalTime, 1.0f);
        transform.UVParams = XMFLOAT4(object.uvScale.x, object.uvScale.y, offsetU, offsetV);
        transform.Material = XMFLOAT4(object.emissive, 0.0f, 0.0f, 0.0f);   // 光る強さ
        transform.WorldInvTrans =
            XMMatrixTranspose(XMMatrixInverse(nullptr, world));
    }

    // ライトの値を定数バッファに書く。Inspector で変えた値が毎フレーム反映される
    {
        auto pLight = m_pLight[m_FrameIndex];

        // 向きは長さ1にしてから渡す。全成分が0で長さ0だと計算できないので、真上からの光にする
        auto dir = XMLoadFloat3(&m_LightDir);
        if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-6f)
        {
            dir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
        }
        XMStoreFloat3(&pLight->LightDir, XMVector3Normalize(dir));

        pLight->LightIntensity    = m_LightIntensity;
        pLight->LightColor        = XMFLOAT3(m_LightColor[0], m_LightColor[1], m_LightColor[2]);
        pLight->Shininess         = m_Shininess;
        pLight->CameraPos         = m_CameraPos;
        pLight->SpecularIntensity = m_SpecularIntensity;
        pLight->AmbientColor      = XMFLOAT3(m_AmbientColor[0], m_AmbientColor[1], m_AmbientColor[2]);
        pLight->Padding           = 0.0f;
    }

    // 描画処理
    {
        // ポリゴン描画用の処理
        _cmdList->SetGraphicsRootSignature(_rootSignature.Get());

        // 先に「どのヒープを使うか」を設定する。GPUハンドルはこのヒープ内の位置として読まれるため
        ID3D12DescriptorHeap* ppHeaps[] = { _heapCBV_SRV_UAV.Get() };
        _cmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
        _cmdList->SetGraphicsRootConstantBufferView(RootParam_Light, _lightCB[m_FrameIndex]->GetGPUVirtualAddress());
        _cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        _cmdList->RSSetViewports(1, &m_Viewport);
        _cmdList->RSSetScissorRects(1, &m_Scissor);

        // 背景の空を先に描く。画面全体をおおう三角形1枚（頂点は SV_VertexID から作るので頂点バッファは不要）
        {
            SkyParam* sky = m_pSky[m_FrameIndex];
            sky->InvViewProj  = XMMatrixInverse(nullptr, view * proj);      // 画面の位置 → ワールドの位置
            sky->CameraPos    = XMFLOAT4(m_CameraPos.x, m_CameraPos.y, m_CameraPos.z, 1.0f);
            sky->TopColor     = XMFLOAT4(m_SkyTop[0], m_SkyTop[1], m_SkyTop[2], 1.0f);
            sky->HorizonColor = XMFLOAT4(m_SkyHorizon[0], m_SkyHorizon[1], m_SkyHorizon[2], 1.0f);
            sky->BottomColor  = XMFLOAT4(m_SkyBottom[0], m_SkyBottom[1], m_SkyBottom[2], 1.0f);
            sky->Params       = XMFLOAT4(static_cast<float>(m_SkyMode), m_SkyRotation / 360.0f, (std::max)(m_SkyExponent, 0.01f), 0.0f);

            // パノラマ画像：選んだテクスチャの SRV を t0 に設定する（グラデーションのときも何か設定しておく）
            int skyTex = m_SkyTexture;
            if (skyTex < 0 || skyTex >= static_cast<int>(m_Textures.size()))
            {
                skyTex = m_DefaultTexture;
            }
            D3D12_GPU_DESCRIPTOR_HANDLE skySrv = m_TextureHeapStart;
            skySrv.ptr += static_cast<UINT64>(m_SrvDescSize) * skyTex;
            _cmdList->SetGraphicsRootDescriptorTable(RootParam_Texture, skySrv);

            _cmdList->SetPipelineState(_skyPSO.Get());
            _cmdList->SetGraphicsRootConstantBufferView(RootParam_Transform, _skyCB[m_FrameIndex]->GetGPUVirtualAddress());
            _cmdList->DrawInstanced(3, 1, 0, 0);
        }

        // ここから物体
        _cmdList->SetPipelineState(_pSO.Get());
        _cmdList->IASetVertexBuffers(0, 1, &m_VBV);
        _cmdList->IASetIndexBuffer(&m_IBV);

        // メッシュごとに描く。頂点・インデックスは1本のバッファにまとめてあるので、範囲だけ変える
        for (size_t j = 0; j < drawCount; ++j)
        {
            // j番目の物体用TransformのGPUアドレス
            const D3D12_GPU_VIRTUAL_ADDRESS address =
                _cB[m_FrameIndex]->GetGPUVirtualAddress()
                + static_cast<UINT64>(sizeof(Transform)) * j;

            // b0に、今回描く物体のTransformを指定する
            _cmdList->SetGraphicsRootConstantBufferView(
                RootParam_Transform, address);

            // 取ったアイテムは描かない
            if (!m_Objects[j].active)
            {
                continue;
            }

            // [1] t0 : この物体のテクスチャ。SRV の場所 = ヒープの先頭 + 番号 × SRV 1個分の大きさ
            // 番号がおかしいときは既定のテクスチャを使う
            int textureId = m_Objects[j].texture;
            if (textureId < 0 || textureId >= static_cast<int>(m_Textures.size()))
            {
                textureId = m_DefaultTexture;
            }
            D3D12_GPU_DESCRIPTOR_HANDLE srv = m_TextureHeapStart;
            srv.ptr += static_cast<UINT64>(m_SrvDescSize) * textureId;
            _cmdList->SetGraphicsRootDescriptorTable(RootParam_Texture, srv);

            // この物体のモデルの番号。おかしな番号なら描かない
            const int modelId = m_Objects[j].model;
            if (modelId < 0 || modelId >= static_cast<int>(m_Models.size()))
            {
                continue;
            }

            // このモデルを構成する部品だけを描く。m_DrawRanges の first 番目から count 個
            const ModelRange& model = m_Models[modelId];
            for (UINT k = 0; k < model.count; ++k)
            {
                const DrawRange& range = m_DrawRanges[model.first + k];
                _cmdList->DrawIndexedInstanced(
                    range.IndexCount,
                    1,
                    range.StartIndex,
                    range.BaseVertex,
                    0);
            }
        }
    }

    DrawImGui();

    // リソースバリアの設定：描画用から表示用に戻す
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = _colorBuffer[m_FrameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // リソースバリア
    _cmdList->ResourceBarrier(1, &barrier);

    // コマンドの記録を終了
    _cmdList->Close();

    // コマンドを実行
    ID3D12CommandList* ppCommandLists[] = { _cmdList.Get() };
    _queue->ExecuteCommandLists(1, ppCommandLists);

    // 画面に表示
    Present(1);
}

//------------------------------------------------------------------
//              画面に表示し、次のフレームの準備
//------------------------------------------------------------------
void App::Present(uint32_t interval)
{
    // 画面に表示
    _swapchain->Present(interval, 0);

    // シグナル処理
    const auto currentValue = m_FenceCounter[m_FrameIndex];
    _queue->Signal(_fence.Get(), currentValue);

    // バックバッファ番号を更新
    m_FrameIndex = _swapchain->GetCurrentBackBufferIndex();

    // 次のフレームの描画準備がまだであれば待機する
    if (_fence->GetCompletedValue() < m_FenceCounter[m_FrameIndex])
    {
        _fence->SetEventOnCompletion(m_FenceCounter[m_FrameIndex], m_FenceEvent);
        WaitForSingleObjectEx(m_FenceEvent, INFINITE, FALSE);
    }

    // 次のフレームのフェンスカウンターを増やす
    m_FenceCounter[m_FrameIndex] = currentValue + 1;
}

//------------------------------------------------------------------------------------
// ゲームモードの準備（--editor なしで起動したとき）
//   ステージを読み込み、プレイヤーがいるか確かめて、タイトル画面から始める
//   失敗したらメッセージを出して false を返す（アプリは終了する）
//------------------------------------------------------------------------------------
bool App::PrepareGameMode()
{
    if (!LoadScene())
    {
        MessageBoxW(nullptr, L"ステージのファイル（res\\stage\\stage1.txt）が読み込めませんでした。", L"Lava Roll", MB_OK);
        return false;
    }

    bool hasPlayer = false;
    for (const auto& object : m_Objects)
    {
        if (object.role == Role_Player)
        {
            hasPlayer = true;
            break;
        }
    }
    if (!hasPlayer)
    {
        MessageBoxW(nullptr, L"ステージにプレイヤー（Role が Player の物体）がいません。", L"Lava Roll", MB_OK);
        return false;
    }

    m_OnTitle   = true;
    m_TitleTime = 0.0f;
    m_PrevEnter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;   // Enter を押したまま起動しても、すぐ始まらないように
    SetupTitleCamera();
    return true;
}

//------------------------------------------------------------------------------------
// タイトル画面のカメラ：ステージ全体を囲む範囲から、回る中心と半径を決める
//   各物体の「位置 ± 大きさの半分」の最小・最大を取り、その真ん中を中心、一番長い辺の分だけ離れて回る
//------------------------------------------------------------------------------------
void App::SetupTitleCamera()
{
    XMFLOAT3 mn( FLT_MAX,  FLT_MAX,  FLT_MAX);
    XMFLOAT3 mx(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const auto& o : m_Objects)
    {
        mn.x = (std::min)(mn.x, o.position.x - o.scale.x * 0.5f);
        mn.y = (std::min)(mn.y, o.position.y - o.scale.y * 0.5f);
        mn.z = (std::min)(mn.z, o.position.z - o.scale.z * 0.5f);
        mx.x = (std::max)(mx.x, o.position.x + o.scale.x * 0.5f);
        mx.y = (std::max)(mx.y, o.position.y + o.scale.y * 0.5f);
        mx.z = (std::max)(mx.z, o.position.z + o.scale.z * 0.5f);
    }
    if (mn.x > mx.x)    // 物体が1つもない
    {
        mn = XMFLOAT3(-5.0f, -1.0f, -5.0f);
        mx = XMFLOAT3( 5.0f,  1.0f,  5.0f);
    }

    m_TitleCenter = XMFLOAT3((mn.x + mx.x) * 0.5f, (mn.y + mx.y) * 0.5f, (mn.z + mx.z) * 0.5f);
    const float size = (std::max)(mx.x - mn.x, mx.z - mn.z);    // 横に一番長い辺
    m_TitleRadius = (std::max)(size * 1.1f, 6.0f);              // ステージ全体が見えるくらい離れる
}

//------------------------------------------------------------------------------------
// タイトル画面の毎フレームの処理
//   カメラをステージの周りでゆっくり回し、Enter（押した瞬間）でプレイを始め、Esc で終了する
//------------------------------------------------------------------------------------
void App::UpdateTitle(float dt)
{
    m_TitleTime += dt;

    // Esc で終了
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000)
    {
        QuitGame();
        return;
    }

    // Enter を押した瞬間にプレイを始める
    const bool enterNow = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    const bool enterPressed = enterNow && !m_PrevEnter;
    m_PrevEnter = enterNow;
    if (enterPressed)
    {
        m_OnTitle = false;
        StartPlay();    // 追従カメラはここでボールの後ろへ移動する
        return;
    }

    // カメラを中心の周りに回す（1秒に 8 度）。斜め上から見下ろす
    const float angle = XMConvertToRadians(m_TitleTime * 8.0f);
    m_CameraPos = XMFLOAT3(
        m_TitleCenter.x + sinf(angle) * m_TitleRadius,
        m_TitleCenter.y + m_TitleRadius * 0.5f,
        m_TitleCenter.z + cosf(angle) * m_TitleRadius);

    // 中心を向く yaw・pitch を逆算する（ResetCamera と同じ計算）
    const float dx = m_TitleCenter.x - m_CameraPos.x;
    const float dy = m_TitleCenter.y - m_CameraPos.y;
    const float dz = m_TitleCenter.z - m_CameraPos.z;
    m_CameraYaw   = XMConvertToDegrees(atan2f(dx, -dz));
    m_CameraPitch = XMConvertToDegrees(atan2f(dy, sqrtf(dx * dx + dz * dz)));
}

//------------------------------------------------------------------------------------
// ゲームを終了する。ウィンドウに「閉じる」を送り、通常の終了の流れ（WM_DESTROY → WM_QUIT）に乗せる
//------------------------------------------------------------------------------------
void App::QuitGame()
{
    PostMessage(m_hWnd, WM_CLOSE, 0, 0);
}

//------------------------------------------------------------------------------------
// タイトル画面の文字
//------------------------------------------------------------------------------------
void App::DrawTitle()
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;

    // 1. ゲームの名前（画面の上のほう、中央）
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.25f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Title_Name", nullptr, flags);
    ImGui::PushFont(m_UiFont, 110.0f);
    ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.1f, 1.0f), "LAVA ROLL");     // オレンジ
    ImGui::PopFont();
    ImGui::End();

    // 2. "Press Enter to Start"（ゆっくり点滅させる）
    const float alpha = 0.55f + 0.45f * sinf(m_TitleTime * 3.0f);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.58f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::Begin("Title_Start", nullptr, flags);
    ImGui::PushFont(m_UiFont, 40.0f);
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, alpha), "Press Enter to Start");
    ImGui::PopFont();
    ImGui::End();

    // 3. 操作方法と目的（画面の下、中央）
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 30.0f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::Begin("Title_Help", nullptr, flags);
    ImGui::PushFont(m_UiFont, 24.0f);
    ImGui::TextColored(ImVec4(0.9f, 0.9f, 0.9f, 1.0f), "WASD / Arrow keys : Roll     Space : Jump     Esc : Quit");
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.5f, 1.0f), "Collect items to open gates, avoid lava, and reach the goal!");
    ImGui::PopFont();
    ImGui::End();
}

//------------------------------------------------------------------------------------
// プレイ中に、ゲーム画面の上に表示するもの（HUD）
//   枠・背景・タイトルバーのない ImGui のウィンドウを置き、大きめの文字で表示する
//   マウスのクリックなどは受け付けない（下のゲーム画面の操作を邪魔しないように）
//------------------------------------------------------------------------------------
void App::DrawHud()
{
    const ImGuiIO& io = ImGui::GetIO();
    const ImGuiWindowFlags hudFlags =
        ImGuiWindowFlags_NoDecoration |         // タイトルバー・枠・スクロールバーなし
        ImGuiWindowFlags_NoBackground |         // 背景なし（文字だけ）
        ImGuiWindowFlags_AlwaysAutoResize |     // 文字の大きさに合わせる
        ImGuiWindowFlags_NoInputs |             // クリックを受け付けない
        ImGuiWindowFlags_NoSavedSettings |      // imgui.ini に位置を保存しない
        ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav;

    // 1. 画面の左上：タイムと、取ったアイテムの数
    //    エディタでは、Inspector は折りたたまれてタイトルバーだけが左上に残るので、その下に置く
    {
        const float t = m_Goaled ? m_ClearTime : m_PlayTime;   // ゴールしたら止める
        const int   minutes = static_cast<int>(t / 60.0f);
        const float seconds = t - static_cast<float>(minutes) * 60.0f;

        ImGui::SetNextWindowPos(ImVec2(10.0f, m_EditorMode ? 40.0f : 10.0f), ImGuiCond_Always);   // ゲームモードは Inspector がないので一番上
        ImGui::Begin("HUD_Status", nullptr, hudFlags);
        ImGui::PushFont(m_UiFont, 36.0f);   // 文字の大きさ（ピクセル）。m_UiFont が無ければ標準フォント
        ImGui::Text("Time  %02d:%05.2f", minutes, seconds);     // 例：01:23.45
        ImGui::Text("Items %d / %d", m_ItemCount, m_ItemTotal);
        ImGui::PopFont();
        ImGui::End();
    }

    // 2. 一時的な文字（"Gate opened!" など）を、アイテムの数の下に表示する
    if (m_HudMessageTime > 0.0f && !m_HudMessage.empty())
    {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 20.0f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
        ImGui::Begin("HUD_Message", nullptr, hudFlags);
        ImGui::PushFont(m_UiFont, 36.0f);   // 文字の大きさ（ピクセル）。m_UiFont が無ければ標準フォント
        ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.6f, 1.0f), "%s", m_HudMessage.c_str());   // 緑っぽい文字
        ImGui::PopFont();
        ImGui::End();
    }

    // ゴールしたら、画面の真ん中に "GOAL!"・クリアタイム・操作を表示する（ほかの真ん中の表示は出さない）
    if (m_Goaled)
    {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.45f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("HUD_Goal", nullptr, hudFlags);
        ImGui::PushFont(m_UiFont, 80.0f);   // 文字の大きさ（ピクセル）。m_UiFont が無ければ標準フォント
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "GOAL!");                  // 金色っぽい文字
        ImGui::PopFont();
        ImGui::PushFont(m_UiFont, 36.0f);   // 文字の大きさ（ピクセル）。m_UiFont が無ければ標準フォント
        ImGui::Text("Time: %.2f s", m_ClearTime);                                       // クリアタイム
        ImGui::PopFont();
        ImGui::PushFont(m_UiFont, 26.0f);   // 文字の大きさ（ピクセル）。m_UiFont が無ければ標準フォント
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Enter: Retry / Esc: Exit");  // 操作の案内
        ImGui::PopFont();
        ImGui::End();
        return;
    }

    // 3. 戻されたら、画面の真ん中に赤い文字で "Respawn" と表示する
    if (m_RespawnMessageTime > 0.0f)
    {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.4f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("HUD_Respawn", nullptr, hudFlags);
        ImGui::PushFont(m_UiFont, 56.0f);   // 文字の大きさ（ピクセル）。m_UiFont が無ければ標準フォント
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Respawn");     // 赤っぽい文字
        ImGui::PopFont();
        ImGui::End();
    }

    // 4. 全部取ったら、画面の真ん中に表示する
    if (m_ItemTotal > 0 && m_ItemCount >= m_ItemTotal)
    {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::Begin("HUD_Clear", nullptr, hudFlags);
        ImGui::PushFont(m_UiFont, 56.0f);   // 文字の大きさ（ピクセル）。m_UiFont が無ければ標準フォント
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f), "All items collected!");   // 金色っぽい文字
        ImGui::PopFont();
        ImGui::End();
    }
}

void App::DrawImGui()
{
    ImGui_ImplDX12_NewFrame(); // ImGui用のリソース作成
    ImGui_ImplWin32_NewFrame(); // ウィンドウの現在のサイズ、経過時間、マウスカーソルの位置などをImGuiに渡す
    ImGui::NewFrame(); // ImGui本体のフレーム開始

    // Inspector はエディタのときだけ出す（ゲームモードでは出さない）
    if (m_EditorMode)
    {
    // 初回だけウィンドウの位置とサイズを決める。以降はユーザーが動かした位置を保つ
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340, 440), ImGuiCond_FirstUseEver);   // ライト・テクスチャの項目まで収まる大きさ

    // プレイの開始・終了で、Inspector を折りたたむ・開く（予約されていれば、この1回だけ）
    if (m_InspectorCollapse != 0)
    {
        ImGui::SetNextWindowCollapsed(m_InspectorCollapse == 1, ImGuiCond_Always);
        m_InspectorCollapse = 0;
    }

    ImGui::Begin("Inspector");

    // -------------------------------------------------
    // プレイ / ストップ
    // -------------------------------------------------
    if (!m_Playing)
    {
        if (ImGui::Button("Play")) { StartPlay(); }
        if (!m_PlayMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", m_PlayMessage.c_str());   // "No player" など
        }
    }
    else
    {
        if (ImGui::Button("Stop")) { StopPlay(); }
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Playing (Esc or Stop to end)");
    }

    // ボールの操作感の調整。プレイ中も変えられる（保存はしない）
    ImGui::SeparatorText("Play Settings");
    ImGui::DragFloat("Accel", &m_PlayAccel, 0.1f, 0.0f, 100.0f);          // 加速の強さ
    ImGui::DragFloat("Friction", &m_PlayFriction, 0.05f, 0.0f, 20.0f);    // 摩擦
    ImGui::DragFloat("Max Speed", &m_PlayMaxSpeed, 0.1f, 0.0f, 100.0f);   // 最高速度
    ImGui::Checkbox("Follow Camera", &m_FollowCamera);                     // OFF ならプレイ中はカメラが止まったまま
    ImGui::DragFloat3("Cam Offset", &m_FollowOffset.x, 0.1f);              // ボールからカメラまでのずらし量
    ImGui::DragFloat("Cam Height", &m_FollowHeight, 0.05f, -5.0f, 20.0f);  // 注視点の高さ
    ImGui::DragFloat("Cam Smooth", &m_FollowSmooth, 0.1f, 0.0f, 50.0f);    // 追いつく速さ。0 で遅れなし
    ImGui::DragFloat("Jump Speed", &m_PlayJumpSpeed, 0.1f, 0.0f, 50.0f);  // ジャンプで飛び出す速さ
    ImGui::DragFloat("Gravity", &m_PlayGravity, 0.1f, 0.0f, 100.0f);      // 重力の強さ
    ImGui::DragFloat("Bounce", &m_PlayBounce, 0.01f, 0.0f, 1.0f);         // 壁で跳ね返る強さ。0 で沿って滑る
    ImGui::DragFloat("Max Slope", &m_PlayMaxSlope, 0.5f, 0.0f, 89.0f);     // 上れる坂の最大の傾き（度）
    if (m_Playing)
    {
        const float speed = sqrtf(m_BallVel.x * m_BallVel.x + m_BallVel.z * m_BallVel.z);
        ImGui::Text("Speed: %.2f  On Ground: %s", speed, m_OnGround ? "Yes" : "No");   // 今の速さと、地面に乗っているか
        ImGui::Text("Items: %d / %d", m_ItemCount, m_ItemTotal);                       // 取ったアイテムの数
    }

    // -------------------------------------------------
    // カメラ
    // -------------------------------------------------
    ImGui::SeparatorText("Camera");
    ImGui::Checkbox("Move Camera", &m_CameraMode);       // ON のとき WASDQE・矢印キーでカメラが動き、物体は動かない
    ImGui::DragFloat3("Cam Pos", &m_CameraPos.x, 0.05f);
    ImGui::DragFloat("Cam Yaw", &m_CameraYaw, 0.5f);
    ImGui::DragFloat("Cam Pitch", &m_CameraPitch, 0.5f, -89.0f, 89.0f);
    ImGui::DragFloat("Mouse Sens", &m_MouseSensitivity, 0.01f, 0.01f, 2.0f);   // 右ドラッグで回すときの感度（度/ピクセル）
    if (ImGui::Button("Reset Camera")) { ResetCamera(); }

    // プレイ中は、物体の編集・追加削除・保存読み込みをできなくする
    // プレイ中に Save すると、遊んでいる途中の配置が保存されてしまうため
    const bool wasPlaying = m_Playing;  // 途中で Stop が押されても Begin/End の数がずれないように覚えておく
    ImGui::BeginDisabled(wasPlaying);

    // -------------------------------------------------
    // 選択中の物体
    // -------------------------------------------------
    ImGui::SeparatorText("Object");

    // 選択番号が範囲外（物体が消えたなど）なら、未選択にする
    if (m_SelectedObject >= static_cast<int>(m_Objects.size()))
    {
        m_SelectedObject = -1;
    }

    const bool hasSelection = (m_SelectedObject >= 0);

    if (m_Objects.empty())
    {
        ImGui::TextUnformatted("No objects");
    }
    else
    {
        // 編集する物体の番号を選ぶ。-1 は「None（未選択）」
        const int lastIndex = static_cast<int>(m_Objects.size()) - 1;
        ImGui::SliderInt("Object", &m_SelectedObject, -1, lastIndex, hasSelection ? "%d" : "None");
        ImGui::SameLine();
        if (ImGui::Button("Deselect")) { m_SelectedObject = -1; }   // Esc キーでも外せる
    }

    if (m_SelectedObject >= 0)
    {
        // 選択した物体そのものを参照する
        GameObject& object = m_Objects[m_SelectedObject];

        // 旧変数ではなく、その物体のデータを編集する
        ImGui::DragFloat3(
            "Position", &object.position.x, 0.01f);

        ImGui::DragFloat3(
            "Rotation", &object.rotation.x, 1.0f);

        ImGui::DragFloat3(
            "Scale", &object.scale.x, 0.01f, 0.01f, 100.0f);

        // どのモデルで描くか。並びは ModelId と同じにする
        // 並びは m_Models と同じ。res\model フォルダのモデルも並ぶ
        {
            const bool validModel = object.model >= 0 && object.model < static_cast<int>(m_Models.size());
            const char* preview = validModel ? m_Models[object.model].name.c_str() : "(none)";
            if (ImGui::BeginCombo("Model", preview))
            {
                for (int i = 0; i < static_cast<int>(m_Models.size()); ++i)
                {
                    const bool isSelected = (i == object.model);
                    if (ImGui::Selectable(m_Models[i].name.c_str(), isSelected))
                    {
                        object.model = i;
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
        }

        // この物体のテクスチャ。番号を変えるだけなので、その場で変えてよい（GPU のリソースは差し替えない）
        if (!m_TextureNames.empty())
        {
            const bool valid = object.texture >= 0 && object.texture < static_cast<int>(m_TextureNames.size());
            const char* preview = valid ? m_TextureNames[object.texture].c_str() : "(default)";

            if (ImGui::BeginCombo("Texture", preview))
            {
                for (int i = 0; i < static_cast<int>(m_TextureNames.size()); ++i)
                {
                    const bool isSelected = (i == object.texture);
                    if (ImGui::Selectable(m_TextureNames[i].c_str(), isSelected))
                    {
                        object.texture = i;
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();   // 開いたときに今のテクスチャの位置を表示する
                    }
                }
                ImGui::EndCombo();
            }
        }

        // 物体の色。テクスチャの色に掛け算される。a（透明度）は今は見た目に使わない（保存はされる）
        ImGui::ColorEdit4("Color", &object.color.x);

        // 見た目（Material）
        ImGui::DragFloat2("UV Scale", &object.uvScale.x, 0.05f, 0.01f, 100.0f);    // テクスチャを何回くり返して貼るか
        ImGui::DragFloat2("UV Scroll", &object.uvScroll.x, 0.005f, -10.0f, 10.0f);  // 1秒にずらす量（流れる溶岩など）
        ImGui::DragFloat("Emissive", &object.emissive, 0.01f, 0.0f, 10.0f);        // 自分で光る強さ

        // 役割。プレイモードでの扱いが変わる。並びは ObjectRole と同じにする
        const char* roleNames[Role_Count] = { "None", "Player", "Wall", "Item", "Gate", "Damage", "Goal" };
        const int prevRole = object.role;
        ImGui::Combo("Role", &object.role, roleNames, Role_Count);
        // Item・Damage・Goal に変えたときは、見つけやすいように Spin を ON にする（あとから OFF にできる）
        if (object.role != prevRole && (object.role == Role_Item || object.role == Role_Damage || object.role == Role_Goal) && !object.spin)
        {
            object.spin      = true;
            object.spinSpeed = 90.0f;
            object.spinAxis  = 0;
        }
        ImGui::TextDisabled("None: pass through / Wall, Gate: solid / Damage: respawn / Goal: clear");   // 役割の意味のメモ（灰色の文字）

        // グループ番号。アイテムとゲートのときだけ表示する
        if (object.role == Role_Item || object.role == Role_Gate)
        {
            ImGui::InputInt("Group", &object.group);
            object.group = (std::max)(0, object.group);     // マイナスにはしない
            ImGui::TextDisabled("Gate opens when all items of the same group are collected");
        }

        // 円周に沿って回る（Orbit）。Player と Gate は回せない
        ImGui::SeparatorText("Orbit");
        const bool canOrbit = (object.role != Role_Player && object.role != Role_Gate);
        ImGui::BeginDisabled(!canOrbit);
        ImGui::Checkbox("Orbit", &object.orbit);
        ImGui::EndDisabled();
        if (!canOrbit)
        {
            ImGui::TextDisabled("Player and Gate cannot orbit");
        }
        else if (object.orbit)
        {
            ImGui::DragFloat3("Orbit Center", &object.orbitCenter.x, 0.05f);   // 円の中心
            ImGui::DragFloat("Orbit Speed", &object.orbitSpeed, 1.0f, -720.0f, 720.0f, "%.0f deg/s");   // 1秒に何度回るか
            const char* axisNames[3] = { "Y (horizontal)", "X (vertical)", "Z (vertical)" };
            ImGui::Combo("Orbit Axis", &object.orbitAxis, axisNames, 3);

            // 半径 ＝ 置いた位置と中心の、軸に垂直な方向の距離
            const float dx = object.position.x - object.orbitCenter.x;
            const float dy = object.position.y - object.orbitCenter.y;
            const float dz = object.position.z - object.orbitCenter.z;
            float radius = 0.0f;
            switch (object.orbitAxis)
            {
            case 1:  radius = sqrtf(dy * dy + dz * dz); break;
            case 2:  radius = sqrtf(dx * dx + dy * dy); break;
            default: radius = sqrtf(dx * dx + dz * dz); break;
            }
            const float speed = fabsf(object.orbitSpeed);
            ImGui::Text("Radius: %.2f  Lap: %.1f s", radius, speed > 0.0f ? 360.0f / speed : 0.0f);   // 半径と、1周にかかる時間

            // 便利ボタン
            if (ImGui::Button("Center = Origin"))
            {
                object.orbitCenter = XMFLOAT3(0.0f, 0.0f, 0.0f);    // 中心を原点にする
            }
            ImGui::SameLine();
            if (ImGui::Button("Center = Here"))
            {
                // 中心を今の位置にして、物体を軸に垂直な方向へ 2 だけずらす（半径 2 の円になる）
                object.orbitCenter = object.position;
                if (object.orbitAxis == 1) { object.position.y += 2.0f; }
                else                       { object.position.x += 2.0f; }
            }
        }

        // その場で回る（Spin）。Player は回せない
        ImGui::SeparatorText("Spin");
        const bool canSpin = (object.role != Role_Player);
        ImGui::BeginDisabled(!canSpin);
        ImGui::Checkbox("Spin", &object.spin);
        ImGui::EndDisabled();
        if (!canSpin)
        {
            ImGui::TextDisabled("Player cannot spin");
        }
        else if (object.spin)
        {
            ImGui::DragFloat("Spin Speed", &object.spinSpeed, 1.0f, -1440.0f, 1440.0f, "%.0f deg/s");   // 1秒に何度回るか
            const char* spinAxisNames[3] = { "Y (turn)", "X (roll)", "Z (roll)" };
            ImGui::Combo("Spin Axis", &object.spinAxis, spinAxisNames, 3);
            const float speed = fabsf(object.spinSpeed);
            ImGui::Text("Turn: %.1f s", speed > 0.0f ? 360.0f / speed : 0.0f);     // 1回転にかかる時間
        }
    }
    else if (!m_Objects.empty())
    {
        ImGui::TextUnformatted("No selection");
    }

    // -------------------------------------------------
    // 物体の追加・複製・削除。ここでは予約だけして、次のフレームの Update で実行する
    // -------------------------------------------------
    ImGui::SeparatorText("Edit");

    if (ImGui::Button("Add Sphere")) { m_PendingEdit = Edit_AddSphere; }
    ImGui::SameLine();      // 横に並べる
    if (ImGui::Button("Add Box"))    { m_PendingEdit = Edit_AddBox; }

    // 複製・削除は、物体を選んでいるときだけ押せるようにする
    ImGui::BeginDisabled(m_SelectedObject < 0);
    if (ImGui::Button("Duplicate"))  { m_PendingEdit = Edit_Duplicate; }
    ImGui::SameLine();
    if (ImGui::Button("Delete"))     { m_PendingEdit = Edit_Delete; }
    ImGui::EndDisabled();

    // 配置の保存・読み込み。Save は読むだけなのでその場で実行、Load は m_Objects を作り直すので予約する
    if (ImGui::Button("Save"))       { SaveScene(); }
    ImGui::SameLine();
    if (ImGui::Button("Load"))       { m_PendingEdit = Edit_Load; }
    if (!m_SceneMessage.empty())
    {
        ImGui::SameLine();
        ImGui::TextUnformatted(m_SceneMessage.c_str());     // "Saved" など、最後の結果
    }

    ImGui::Text("Objects: %d / %d", static_cast<int>(m_Objects.size()), static_cast<int>(MaxObjects));
    if (m_EditLimitReached)
    {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Reached MaxObjects");   // 赤っぽい文字
    }

    ImGui::EndDisabled();   // ここまでがプレイ中に灰色になる部分

    ImGui::Checkbox("Auto Rotate", &m_AutoRotate);
    ImGui::ColorEdit4("Clear Color", m_ClearColor);

    // -------------------------------------------------
    // ライトの設定
    // -------------------------------------------------
    ImGui::SeparatorText("Light");

    ImGui::DragFloat3(
        "Direction", &m_LightDir.x, 0.01f, -1.0f, 1.0f);

    ImGui::ColorEdit3("Light Color", m_LightColor);

    ImGui::DragFloat(
        "Intensity", &m_LightIntensity, 0.01f, 0.0f, 10.0f);

    ImGui::ColorEdit3("Ambient", m_AmbientColor);

    ImGui::DragFloat(
        "Specular", &m_SpecularIntensity, 0.01f, 0.0f, 5.0f);

    ImGui::DragFloat(
        "Shininess", &m_Shininess, 0.5f, 1.0f, 256.0f);

    // -------------------------------------------------
    // 背景の空
    // -------------------------------------------------
    ImGui::SeparatorText("Sky");
    const char* skyModeNames[2] = { "Gradient", "Texture" };
    ImGui::Combo("Sky Mode", &m_SkyMode, skyModeNames, 2);
    if (m_SkyMode == 0)
    {
        ImGui::ColorEdit3("Sky Top", m_SkyTop);             // 真上の色
        ImGui::ColorEdit3("Sky Horizon", m_SkyHorizon);     // 地平線の色
        ImGui::ColorEdit3("Sky Bottom", m_SkyBottom);       // 真下の色
        ImGui::DragFloat("Sky Curve", &m_SkyExponent, 0.01f, 0.01f, 8.0f);     // 小さいほど地平線の色が上まで広がる
    }
    else if (!m_TextureNames.empty())
    {
        // パノラマ画像を選ぶ
        const bool valid = m_SkyTexture >= 0 && m_SkyTexture < static_cast<int>(m_TextureNames.size());
        if (ImGui::BeginCombo("Sky Texture", valid ? m_TextureNames[m_SkyTexture].c_str() : "(default)"))
        {
            for (int i = 0; i < static_cast<int>(m_TextureNames.size()); ++i)
            {
                const bool isSelected = (i == m_SkyTexture);
                if (ImGui::Selectable(m_TextureNames[i].c_str(), isSelected))
                {
                    m_SkyTexture = i;
                }
                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        ImGui::DragFloat("Sky Rotation", &m_SkyRotation, 0.5f, -360.0f, 360.0f, "%.0f deg");   // 空を横に回す
    }

    ImGui::End(); // 記録の終了
    }   // if (m_EditorMode)

    // ゲームモードのタイトル画面、またはプレイ中の HUD
    if (!m_EditorMode && m_OnTitle)
    {
        DrawTitle();
    }
    else if (m_Playing)
    {
        DrawHud();
    }

    ImGui::Render(); // ここまでに組み立てたUIを描画データに変換し確定させる

    // ImGuiがテクスチャを参照するためのヒープを設定
    ID3D12DescriptorHeap* ppHeaps[1] = { _heapImGui.Get() }; // m_pHeapImGuiはディスクリプタヒープのポイント
    _cmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps); // コマンドリストにこのディスクリプタヒープを使うよという設定
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), _cmdList.Get()); // ImGuiの描画命令を記録
}

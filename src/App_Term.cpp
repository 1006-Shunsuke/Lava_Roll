//-----------------------------------------------------------------------------
// File : App_Term.cpp
// Desc : 終了処理。TermApp・TermWnd・WaitGpu・TermD3D・OnTerm
//-----------------------------------------------------------------------------
#include "App.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"

void App::TermApp()
{
    // 終了時に配置を自動保存する。初期化に失敗したときは保存しない（空の配置で上書きしないため）
    if (m_SceneReady && m_EditorMode)   // ゲームモードでは保存しない（遊んだ人がステージを書き換えないように）
    {
        // プレイ中に閉じたときは、遊んだあとの配置ではなく、プレイ開始前の配置に戻してから保存する
        if (m_Playing)
        {
            StopPlay();
        }
        SaveScene();
    }

    WaitGpu();      // GPU の処理がすべて終わるまで待つ。以降の終了処理はこの1回を前提にする
    TermImGui();    // デバイスを解放する前に ImGui を終了する
    OnTerm();
    TermD3D();
    TermWnd();
    CoUninitialize();
}

void App::TermWnd()
{
    if (m_hInst != nullptr)
    {
        UnregisterClass(ClassName, m_hInst);
    }
    m_hInst = nullptr;
    m_hWnd = nullptr;

}

//-------------------------------------------------------------------
//                  GPUの処理完了を待つ
//-------------------------------------------------------------------
void App::WaitGpu()
{
    if (_queue == nullptr || _fence == nullptr || m_FenceEvent == nullptr)
    {
        return;
    }

    // シグナル処理

    _queue->Signal(_fence.Get(), m_FenceCounter[m_FrameIndex]);

    // 完了時にイベント設定
    _fence->SetEventOnCompletion(m_FenceCounter[m_FrameIndex], m_FenceEvent);

    // 待機処理
    WaitForSingleObjectEx(m_FenceEvent, INFINITE, FALSE);

    // カウンターを増やす
    m_FenceCounter[m_FrameIndex]++;
}

//-------------------------------------------------------------------
//                  Direct3Dの終了処理
//-------------------------------------------------------------------
// 作成したオブジェクトを一つずつ開放
void App::TermD3D()
{
    // イベント破棄
    if (m_FenceEvent != nullptr)
    {
        CloseHandle(m_FenceEvent);
        m_FenceEvent = nullptr;
    }

    // 6.フェンス破棄
    _fence.Reset();

    // 5.RTV破棄
    _heapRTV.Reset();
    for (auto i = 0u; i < FrameCount; ++i)
    {
        _colorBuffer[i].Reset();
    }

    // 4.2.コマンドリスト破棄
    _cmdList.Reset();


    // 4.1.コマンドアロケータ破棄
    for (auto i = 0u; i < FrameCount; ++i)
    {
        _cmdAllocator[i].Reset();
    }

    // 3.スワップチェイン破棄
    _swapchain.Reset();

    // 2.コマンドキュー破棄
    _queue.Reset();

    // 1.デバイス破棄
    _device.Reset();

}

//-----------------------------------------------------------------------------
//      終了時の処理
//-----------------------------------------------------------------------------
void App::OnTerm()
{
    // 深度バッファの解放
    _depthBuffer.Reset();
    _heapDSV.Reset();
    m_HandleDSV = {};

    for (auto i = 0u; i < FrameCount; ++i)
    {
        if (_cB[i].Get() != nullptr)
        {
            _cB[i]->Unmap(0, nullptr);
            m_pTransform[i] = nullptr;
        }
        _cB[i].Reset();

        // ライト用の定数バッファ。Transform と同じく Unmap してから解放する
        if (_lightCB[i].Get() != nullptr)
        {
            _lightCB[i]->Unmap(0, nullptr);
            m_pLight[i] = nullptr;
        }
        _lightCB[i].Reset();

        // 空の定数バッファ
        if (_skyCB[i].Get() != nullptr)
        {
            _skyCB[i]->Unmap(0, nullptr);
            m_pSky[i] = nullptr;
        }
        _skyCB[i].Reset();
    }
    // テクスチャとCBV_SRV_UAVヒープの破棄
    m_Textures.clear();         // 全テクスチャを解放する
    m_TextureFiles.clear();
    m_TextureNames.clear();
    _heapCBV_SRV_UAV.Reset();
    m_TextureHeapStart = {};

    _vB.Reset();
    _iB.Reset();
    _pSO.Reset();
    _skyPSO.Reset();
    _rootSignature.Reset();
}

void App::TermImGui()
{
    // InitApp が途中で失敗した場合はコンテキストが無いので何もしない
    if (ImGui::GetCurrentContext() == nullptr)
    {
        return;
    }

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    _heapImGui.Reset();
}
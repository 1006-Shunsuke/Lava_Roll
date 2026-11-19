#if defined(DEBUG)||defined(_DEBUG)
#define _CRTDBG_MAP_ALLOC
#include<crtdbg.h>
#endif // defined(DEBUG)||defined(_DEBUG)



#include "App.h"
// argc・argv : 起動オプション。"--editor" があればエディタとして、なければゲームとして起動する
int wmain(int argc, wchar_t* argv[])
{
    #if defined(DEBUG)||defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    #endif // defined(DEBUG)||defined(_DEBUG)

    // 起動オプションに --editor があるか調べる。Visual Studio では「プロジェクトのプロパティ → デバッグ → コマンド引数」に書く
    bool editorMode = false;
    for (int i = 1; i < argc; ++i)
    {
        if (_wcsicmp(argv[i], L"--editor") == 0)
        {
            editorMode = true;
        }
    }

    App app(1600, 900, editorMode);     // 最初のウィンドウの大きさ（描画される部分の幅・高さ）と、エディタとして起動するか
    app.Run();
    return 0;
}

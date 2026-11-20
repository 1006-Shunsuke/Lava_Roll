//-----------------------------------------------------------------------------
// File : SkyVS.hlsl
// Desc : 背景の空を描く頂点シェーダ
//        頂点バッファは使わず、頂点の番号（SV_VertexID）から画面全体をおおう大きな三角形を1枚作る
//        各頂点で「カメラからその方向を見たときの向き」を求め、PS で空の色を決める
//-----------------------------------------------------------------------------

// ピクセルシェーダへ渡すデータ
struct VSOutput
{
    float4 Position : SV_Position;      // 画面上の位置
    float3 Dir      : DIRECTION;        // カメラから見た方向（ワールド空間）。PS で長さ1にする
    nointerpolation float4 TopColor     : TOP_COLOR;        // 真上の色
    nointerpolation float4 HorizonColor : HORIZON_COLOR;    // 地平線の色
    nointerpolation float4 BottomColor  : BOTTOM_COLOR;     // 真下の色
    nointerpolation float4 Params       : PARAMS;           // x: 空の種類（0 = グラデーション、1 = 画像）、y: 横の回転（1で1周）、z: グラデーションの曲がり具合
};

// b0 : 空の設定。App.h の SkyParam と同じ並び
cbuffer SkyParam : register(b0)
{
    float4x4 InvViewProj  : packoffset(c0);     // (ビュー行列 × 射影行列) の逆行列。画面の位置 → ワールドの位置
    float4   CameraPos    : packoffset(c4);     // カメラの位置
    float4   TopColor     : packoffset(c5);
    float4   HorizonColor : packoffset(c6);
    float4   BottomColor  : packoffset(c7);
    float4   Params       : packoffset(c8);
}

VSOutput main(uint id : SV_VertexID)
{
    VSOutput output = (VSOutput) 0;
    float2 ndc = float2((id == 2) ? 3.0f : -1.0f, (id == 1) ? 3.0f : -1.0f);

    // 一番奥に置く。深度バッファには書き込まない設定なので、物体は必ず手前に描かれる
    output.Position = float4(ndc, 1.0f, 1.0f);

    // 画面の位置を、一番奥の面の上のワールドの位置に戻し、カメラからの方向にする
    float4 world = mul(InvViewProj, float4(ndc, 1.0f, 1.0f));
    output.Dir = world.xyz / world.w - CameraPos.xyz;

    // 色と設定は PS から b0 が見えないので、ここで受け取って渡す
    output.TopColor     = TopColor;
    output.HorizonColor = HorizonColor;
    output.BottomColor  = BottomColor;
    output.Params       = Params;
    return output;
}

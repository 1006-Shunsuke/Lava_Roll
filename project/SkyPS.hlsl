//-----------------------------------------------------------------------------
// File : SkyPS.hlsl
// Desc : 背景の空を描くピクセルシェーダ
//        Params.x = 0 : グラデーション。見ている方向の高さで、真上・地平線・真下の3色をなめらかにつなぐ
//        Params.x = 1 : 画像。t0 のテクスチャをパノラマ（正距円筒図法）として貼る
//-----------------------------------------------------------------------------

// 頂点シェーダからの入力。SkyVS.hlsl の VSOutput と同じ
struct VSOutput
{
    float4 Position : SV_Position;
    float3 Dir      : DIRECTION;
    nointerpolation float4 TopColor     : TOP_COLOR;
    nointerpolation float4 HorizonColor : HORIZON_COLOR;
    nointerpolation float4 BottomColor  : BOTTOM_COLOR;
    nointerpolation float4 Params       : PARAMS;
};

SamplerState ColorSmp : register(s0);
Texture2D    ColorMap : register(t0);

static const float PI = 3.14159265f;

float4 main(VSOutput input) : SV_TARGET0
{
    float3 dir = normalize(input.Dir);      // 見ている方向（長さ1）

    if (input.Params.x < 0.5f)
    {
        // グラデーション：dir.y が 1 なら真上、0 なら地平線、-1 なら真下
        // pow で曲がり具合を変える。小さいほど、地平線の色が上のほうまで広がる
        float  h = dir.y;
        float3 color;
        if (h >= 0.0f)
        {
            color = lerp(input.HorizonColor.rgb, input.TopColor.rgb, pow(h, input.Params.z));
        }
        else
        {
            color = lerp(input.HorizonColor.rgb, input.BottomColor.rgb, pow(-h, input.Params.z));
        }
        return float4(color, 1.0f);
    }

    // 画像：方向を、パノラマ画像の横（経度）と縦（緯度）の位置にする
    //   u : 水平方向の角度。atan2 で -π〜π を 0〜1 にして、横の回転を足す
    //   v : 上下の角度。真上が 0、真下が 1
    float u = atan2(dir.x, -dir.z) / (2.0f * PI) + 0.5f + input.Params.y;
    float v = acos(clamp(dir.y, -1.0f, 1.0f)) / PI;

    // ミップマップの段を 0 に固定する。u が 1 → 0 に飛ぶ境目で、ミップマップの計算がおかしくなって線が出るのを防ぐ
    float3 color = ColorMap.SampleLevel(ColorSmp, float2(u, v), 0.0f).rgb;
    return float4(color, 1.0f);
}

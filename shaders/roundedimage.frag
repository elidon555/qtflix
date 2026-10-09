#version 440
// RoundedImage: draws `source` cover-cropped (uvRect) over a solid underlay (bg), with per-corner rounded
// corners cut by an antialiased signed-distance test. One pass, no offscreen layer, no MSAA needed.

layout(location = 0) in vec2 qt_TexCoord0;
layout(location = 0) out vec4 fragColor;

layout(std140, binding = 0) uniform buf {
    mat4 qt_Matrix;
    float qt_Opacity;
    vec2 itemSize;       // item size in item units
    vec4 radii;          // top-left, top-right, bottom-right, bottom-left
    vec4 uvRect;         // texture crop: offset (xy), scale (zw)
    vec4 bg;             // underlay colour (shown until / where the image is not drawn)
    float imageOpacity;  // 0..1 fade of the image over the underlay
};
layout(binding = 1) uniform sampler2D source;

void main()
{
    vec2 p = (qt_TexCoord0 - 0.5) * itemSize;           // centred, y down
    vec2 halfSize = 0.5 * itemSize;
    float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    vec2 q = abs(p) - halfSize + r;
    float d = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
    float aa = max(fwidth(d), 1e-4);                     // one device pixel, also while scaled
    float coverage = clamp(0.5 - d / aa, 0.0, 1.0);

    vec4 img = texture(source, uvRect.xy + qt_TexCoord0 * uvRect.zw) * imageOpacity;
    vec4 bgp = vec4(bg.rgb * bg.a, bg.a);
    fragColor = (img + bgp * (1.0 - img.a)) * coverage * qt_Opacity;
}

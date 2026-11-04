#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform sampler2D source;
layout(set=0,binding=2) uniform sampler2D coverage;
layout(set=0,binding=1) uniform Display {
    vec4 mapping;
    vec4 grading;
    vec4 vignette;
    vec4 bloom;
    vec4 post;
    vec4 screen;
    vec4 blur;
} display;
vec3 sample_color(vec2 position) {
    return texture(source,min(position,1.0-1.0/vec2(textureSize(source,0)))).rgb;
}
void main() {
    if(display.blur.w==0) {
        vec2 position=min(uv,1.0-1.0/vec2(textureSize(source,0)));
        vec4 color=texture(source,position);
        float alpha=clamp(color.a,0,1);
        float amount=clamp(texture(coverage,position).a,0,1);
        vec3 radiance=alpha>0 ? color.rgb/alpha*amount : vec3(0);
        float luminance=dot(radiance,vec3(0.2126,0.7152,0.0722));
        result=vec4(clamp(radiance*clamp(luminance-display.bloom.y,0,1),0,1),1);
        return;
    }
    const float centers[5]=float[5](0.375,0.3125,0.2734375,0.24609375,0.2255859375);
    const vec3 offsets[5]=vec3[5](vec3(1.2,0,0),vec3(1.2857142857142858,0,0),
        vec3(1.3333333333333333,3.111111111111111,0),
        vec3(1.3636363636363635,3.1818181818181817,0),
        vec3(1.3846153846153846,3.230769230769231,5.076923076923077));
    const vec3 weights[5]=vec3[5](vec3(0.3125,0,0),vec3(0.328125,0,0),
        vec3(0.328125,0.03515625,0),vec3(0.322265625,0.0537109375,0),
        vec3(0.314208984375,0.06982421875,0.003173828125));
    int level=int(display.blur.z);
    vec3 color=sample_color(uv)*centers[level];
    vec2 step_size=display.blur.xy*display.screen.xy*display.screen.z;
    for(int tap=0;tap<3;++tap) {
        if(weights[level][tap]==0) continue;
        vec2 delta=step_size*offsets[level][tap];
        color+=(sample_color(uv-delta)+sample_color(uv+delta))*weights[level][tap];
    }
    result=vec4(color,1);
}

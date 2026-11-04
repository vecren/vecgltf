#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform sampler2D source;
layout(set=0,binding=1) uniform Display {
    vec4 mapping;
    vec4 grading;
    vec4 vignette;
    vec4 bloom;
    vec4 post;
    vec4 screen;
    vec4 blur;
} display;
layout(set=0,binding=3) uniform sampler2D bloom0;
layout(set=0,binding=4) uniform sampler2D bloom1;
layout(set=0,binding=5) uniform sampler2D bloom2;
layout(set=0,binding=6) uniform sampler2D bloom3;
layout(set=0,binding=7) uniform sampler2D bloom4;
vec3 encoded(vec3 color) {
    color=max(color,0);
    return mix(12.92*color,1.055*pow(color,vec3(1.0/2.4))-0.055,
               greaterThan(color,vec3(0.0031308)));
}
vec3 bloom_sample(sampler2D image) {
    return texture(image,min(uv,1.0-1.0/vec2(textureSize(image,0)))).rgb;
}
void main() {
    vec4 input_color=texture(source,uv);
    float alpha=clamp(input_color.a,0,1);
    if(alpha<=0) {result=vec4(0);return;}
    vec3 color=input_color.rgb/alpha;
    if(display.bloom.x>0) {
        vec4 weight=mix(vec4(1.1,0.9,0.6,0.3),vec4(0.1,0.3,0.6,0.9),display.bloom.z);
        float last=mix(0.1,1.1,display.bloom.z);
        color+=display.bloom.x*(bloom_sample(bloom0)*weight.x+
            bloom_sample(bloom1)*weight.y+bloom_sample(bloom2)*weight.z+
            bloom_sample(bloom3)*weight.w+bloom_sample(bloom4)*last);
    }
    color*=display.mapping.x*display.mapping.z;
    float luminance=dot(color,vec3(0.2126,0.7152,0.0722));
    color=mix(vec3(luminance),color,display.mapping.w);
    color=max(color*(1+display.grading.x)-0.5*display.grading.x,0);
    if(display.mapping.y==0)
        color=clamp((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14),0,1);
    else if(display.mapping.y==1)
        color=color/(1+max(max(color.r,color.g),color.b));
    if(display.vignette.z>0 && display.bloom.w==0) {
        float radius=distance(uv,display.vignette.xy);
        float attenuation=1-display.grading.y*smoothstep(display.grading.z,display.grading.w,radius);
        color*=attenuation;
    }
    result=display.bloom.w>0 ? vec4(encoded(color),alpha) : vec4(color*alpha,alpha);
}

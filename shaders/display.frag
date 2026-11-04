#version 450
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform sampler2D source;
layout(set=0,binding=1) uniform Display {
    vec4 mapping;
    vec4 grading;
    vec4 vignette;
} display;
void main() {
    vec4 input_color=texture(source,uv);
    float alpha=clamp(input_color.a,0,1);
    if(alpha<=0) {result=vec4(0);return;}
    vec3 color=input_color.rgb/alpha*display.mapping.x*display.mapping.z;
    float luminance=dot(color,vec3(0.2126,0.7152,0.0722));
    color=mix(vec3(luminance),color,display.mapping.w);
    color=max(color*(1+display.grading.x)-0.5*display.grading.x,0);
    if(display.mapping.y==0)
        color=clamp((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14),0,1);
    else if(display.mapping.y==1)
        color=color/(1+max(max(color.r,color.g),color.b));
    if(display.vignette.z>0) {
        float radius=distance(uv,display.vignette.xy);
        float attenuation=1-display.grading.y*smoothstep(display.grading.z,display.grading.w,radius);
        color*=attenuation;
    }
    result=vec4(color*alpha,alpha);
}

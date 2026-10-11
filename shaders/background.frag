#version 450
layout(location=0) in vec2 ndc;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform Frame {vec4 eye;vec4 options;mat4 inverse_view_projection;} frame;
layout(set=0,binding=4) uniform sampler2D environment;
void main() {
    vec4 world=frame.inverse_view_projection*vec4(ndc,1,1);
    vec3 d=normalize(world.xyz/world.w-frame.eye.xyz);
    vec2 uv=vec2(atan(d.z,d.x)/6.28318530718+0.5,acos(clamp(d.y,-1,1))/3.14159265359);
    vec3 color=texture(environment,uv).rgb*frame.options.x;
    color=clamp((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14),0,1);
    result=vec4(color,1);
}

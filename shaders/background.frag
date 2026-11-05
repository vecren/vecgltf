#version 450
layout(location=0) in vec2 ndc;
layout(location=0) out vec4 result;
layout(set=0,binding=0) uniform Frame {
    vec4 eye;
    vec4 options;
    mat4 inverse_view_projection;
    mat4 projection;
    vec4 lighting;
    mat4 world_to_environment;
    vec4 light_directions[8];
    vec4 light_radiances[8];
    vec4 environment_lod;
} frame;
layout(set=0,binding=4) uniform sampler2D environment;
layout(set=0,binding=7) uniform Points {
    vec4 positions[8];
    vec4 radiances[8];
    vec4 attenuation[8];
    vec4 metadata;
    vec4 directions[8];
    vec4 cones[8];
} points;
void main() {
    // The far plane can have w=0 for an infinite perspective projection.
    vec4 near_world=frame.inverse_view_projection*vec4(ndc,0,1);
    vec4 middle_world=frame.inverse_view_projection*vec4(ndc,0.5,1);
    vec3 direction=middle_world.xyz*near_world.w-near_world.xyz*middle_world.w;
    vec3 d=mat3(frame.world_to_environment)*normalize(direction);
    vec2 uv=vec2(atan(d.z,d.x)/6.28318530718+0.5,acos(clamp(d.y,-1,1))/3.14159265359);
    vec3 color=(frame.options.z==3 ? points.metadata.yzw : texture(environment,uv).rgb*frame.lighting.w)*frame.options.x;
    if(frame.lighting.y==1) color=color/(1+max(max(color.r,color.g),color.b));
    else if(frame.lighting.y!=2) color=clamp((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14),0,1);
    result=vec4(color,1);
}

#version 450
#ifndef SHADOW_CATCHER
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 color;
layout(location=3) in vec4 tangent;
layout(location=4) in vec2 texture_uv[5];
layout(location=9) in uvec4 joints;
layout(location=10) in vec4 weights;
#endif
layout(location=0) out vec3 world_normal;
layout(location=1) out vec4 vertex_color;
layout(location=2) out vec3 relative_position;
layout(location=3) out vec4 world_tangent;
layout(location=4) out vec2 texcoords[5];
layout(location=9) flat out float winding;
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
#ifndef SHADOW_CATCHER
layout(set=2,binding=0,std430) readonly buffer Poses {vec4 values[];} pose;
layout(set=3,binding=0,std430) readonly buffer Morphs {vec4 values[];} morph;
#endif
layout(push_constant) uniform Draw {
    mat4 view;
    vec4 model_row0;
    vec4 model_row1;
    vec4 model_row2;
    uint skin_offset;
    uint weight_offset;
    uint morph_count;
    uint vertex_count;
} draw;
void main() {
#ifdef SHADOW_CATCHER
    const vec2 corners[6]=vec2[6](vec2(-1,-1),vec2(1,-1),vec2(1,1),vec2(-1,-1),vec2(1,1),vec2(-1,1));
    vec2 coord=corners[gl_VertexIndex];
    vec3 p=vec3(coord,0),n=vec3(0,0,1),t=vec3(1,0,0);
#else
    vec3 p=position,n=normal,t=tangent.xyz;
    for(uint target=0;target<draw.morph_count;++target) {
        float weight=pose.values[draw.weight_offset+target/4][target%4];
        uint offset=(target*draw.vertex_count+gl_VertexIndex)*3;
        p+=morph.values[offset].xyz*weight;
        n+=morph.values[offset+1].xyz*weight;
        t+=morph.values[offset+2].xyz*weight;
    }
#endif
    mat4 model=transpose(mat4(draw.model_row0,draw.model_row1,draw.model_row2,vec4(0,0,0,1)));
#ifndef SHADOW_CATCHER
    if(draw.skin_offset!=0xffffffff) {
        model=mat4(0);
        for(uint i=0;i<4;++i) {
            uint offset=draw.skin_offset+joints[i]*4;
            model+=mat4(pose.values[offset],pose.values[offset+1],pose.values[offset+2],pose.values[offset+3])*weights[i];
        }
    }
#endif
    mat3 transform=mat3(model);
    winding=sign(determinant(transform));
    world_normal=transpose(inverse(transform))*n;
#ifdef SHADOW_CATCHER
    world_tangent=vec4(transform*t,winding);
#else
    world_tangent=vec4(transform*t,tangent.w*winding);
#endif
    vec4 relative_world=model*vec4(p,1);
    relative_position=relative_world.xyz;
#ifdef SHADOW_CATCHER
    vertex_color=vec4(1);
    for(int slot=0;slot<5;++slot) texcoords[slot]=coord*0.5+0.5;
#else
    vertex_color=color;
    for(int slot=0;slot<5;++slot) texcoords[slot]=texture_uv[slot];
#endif
    precise vec4 camera_position=draw.view*relative_world;
    gl_Position=frame.projection*camera_position;
}

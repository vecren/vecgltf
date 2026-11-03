#version 450
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec4 color;
layout(location=3) in vec4 tangent;
layout(location=4) in vec2 texture_uv[5];
layout(location=9) in uvec4 joints;
layout(location=10) in vec4 weights;
layout(location=0) out vec3 world_normal;
layout(location=1) out vec4 vertex_color;
layout(location=2) out vec3 relative_position;
layout(location=3) out vec4 world_tangent;
layout(location=4) out vec2 texcoords[5];
layout(location=9) flat out float winding;
layout(set=0,binding=0) uniform Frame {vec4 eye;vec4 options;mat4 inverse_view_projection;mat4 projection;} frame;
layout(set=2,binding=0,std430) readonly buffer Poses {vec4 values[];} pose;
layout(set=3,binding=0,std430) readonly buffer Morphs {vec4 values[];} morph;
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
    vec3 p=position,n=normal,t=tangent.xyz;
    for(uint target=0;target<draw.morph_count;++target) {
        float weight=pose.values[draw.weight_offset+target/4][target%4];
        uint offset=(target*draw.vertex_count+gl_VertexIndex)*3;
        p+=morph.values[offset].xyz*weight;
        n+=morph.values[offset+1].xyz*weight;
        t+=morph.values[offset+2].xyz*weight;
    }
    mat4 model=transpose(mat4(draw.model_row0,draw.model_row1,draw.model_row2,vec4(0,0,0,1)));
    if(draw.skin_offset!=0xffffffff) {
        model=mat4(0);
        for(uint i=0;i<4;++i) {
            uint offset=draw.skin_offset+joints[i]*4;
            model+=mat4(pose.values[offset],pose.values[offset+1],pose.values[offset+2],pose.values[offset+3])*weights[i];
        }
    }
    mat3 transform=mat3(model);
    winding=sign(determinant(transform));
    world_normal=transpose(inverse(transform))*n;
    world_tangent=vec4(transform*t,tangent.w*winding);
    vec4 relative_world=model*vec4(p,1);
    relative_position=relative_world.xyz;
    vertex_color=color;
    for(int slot=0;slot<5;++slot) texcoords[slot]=texture_uv[slot];
    precise vec4 camera_position=draw.view*relative_world;
    gl_Position=frame.projection*camera_position;
}

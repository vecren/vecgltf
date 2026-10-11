#version 450
layout(location=0) out vec2 ndc;
void main() {
    vec2 uv=vec2((gl_VertexIndex<<1)&2,gl_VertexIndex&2);
    ndc=uv*2-1;
    gl_Position=vec4(ndc,1,1);
}

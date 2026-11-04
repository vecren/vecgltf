#version 450
layout(location=0) in vec3 world_normal;
layout(location=1) in vec4 vertex_color;
layout(location=2) in vec3 relative_position;
layout(location=3) in vec4 world_tangent;
layout(location=4) in vec2 texcoords[5];
layout(location=9) flat in float winding;
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
layout(set=0,binding=1) uniform sampler2D specular_environment;
layout(set=0,binding=2) uniform sampler2D diffuse_environment;
layout(set=0,binding=3) uniform sampler2D brdf_lut;
layout(set=1,binding=0) uniform sampler2D base_texture;
layout(set=1,binding=1) uniform sampler2D physical_texture;
layout(set=1,binding=2) uniform sampler2D normal_texture;
layout(set=1,binding=3) uniform sampler2D occlusion_texture;
layout(set=1,binding=4) uniform sampler2D emissive_texture;
layout(set=1,binding=5) uniform Material {
    vec4 base;
    vec4 emissive;
    vec4 factors;
    vec4 flags;
    vec4 specular;
    vec4 offset_scale[5];
    vec4 rotation_uv[5];
} material;
const float pi=3.14159265359;
vec2 uv(int index) {
    vec4 transform=material.offset_scale[index];
    vec4 rotation=material.rotation_uv[index];
    vec2 coord=texcoords[index]*transform.zw;
    return transform.xy+mat2(rotation.x,rotation.y,-rotation.y,rotation.x)*coord;
}
vec3 mapped_normal(vec3 normal) {
    if(material.rotation_uv[2].w==0) return normal;
    vec3 map=texture(normal_texture,uv(2)).xyz*2-1;
    map.xy*=material.factors.z;
    vec3 tangent,bitangent;
    if(material.rotation_uv[2].x==1 && material.rotation_uv[2].y==0 && all(equal(material.offset_scale[2].zw,vec2(1)))) {
        tangent=normalize(world_tangent.xyz-normal*dot(normal,world_tangent.xyz));
        bitangent=cross(normal,tangent)*world_tangent.w;
    } else {
        vec3 dp1=dFdx(relative_position),dp2=dFdy(relative_position);
        vec2 duv1=dFdx(uv(2)),duv2=dFdy(uv(2));
        float determinant=duv1.x*duv2.y-duv1.y*duv2.x;
        if(abs(determinant)<1e-12) return normal;
        tangent=normalize((dp1*duv2.y-dp2*duv1.y)/determinant);
        tangent=normalize(tangent-normal*dot(normal,tangent));
        bitangent=normalize((dp2*duv1.x-dp1*duv2.x)/determinant);
        bitangent=normalize(bitangent-normal*dot(normal,bitangent)-tangent*dot(tangent,bitangent));
    }
    return normalize(mat3(tangent,bitangent,normal)*map);
}
vec2 environment_uv(vec3 direction) {
    direction=mat3(frame.world_to_environment)*direction;
    return vec2(atan(direction.z,direction.x)/(2*pi)+0.5,acos(clamp(direction.y,-1,1))/pi);
}
vec3 tone_map(vec3 color) {
    color*=frame.options.x;
    if(frame.lighting.y==1) return color/(1+max(max(color.r,color.g),color.b));
    if(frame.lighting.y==2) return color;
    return clamp((color*(2.51*color+0.03))/(color*(2.43*color+0.59)+0.14),0,1);
}
void main() {
    bool front=gl_FrontFacing!=(winding<0);
    if(!front && material.flags.z==0) discard;
    vec4 base=material.base*vertex_color*texture(base_texture,uv(0));
    if(material.flags.x==1 && base.a<material.flags.y) discard;
    if(material.flags.x==2) {
#ifdef BLEND_OPAQUE
        if(base.a<1) discard;
#else
        if(base.a<=0 || base.a>=1) discard;
#endif
    }
    float alpha=material.flags.x==2 ? base.a:1;
    int debug_view=int(frame.options.y);
    if(debug_view==1) {result=vec4(base.rgb,alpha);return;}
    int shading=int(material.flags.w);
    if((shading&1)!=0 && debug_view==0) { result=vec4(tone_map(base.rgb),alpha);return; }
    vec3 n=mapped_normal(normalize(world_normal));
    if(!front) n=-n;
    vec3 v=normalize(frame.eye.xyz-relative_position);
    vec4 physical=texture(physical_texture,uv(1));
    bool specular_glossiness=(shading&2)!=0;
    float metallic=specular_glossiness ? 0:clamp(material.factors.x*physical.b,0,1);
    float roughness=clamp(specular_glossiness ? 1-material.specular.w*physical.a:material.factors.y*physical.g,0.04,1);
    float nv=max(dot(n,v),0.001);
    vec3 f0=specular_glossiness ? material.specular.rgb*physical.rgb:mix(vec3(0.04),base.rgb,metallic);
    vec3 diffuse_color=base.rgb*(specular_glossiness ? 1-max(max(f0.r,f0.g),f0.b):1-metallic);
    float a=roughness*roughness,a2=a*a;
    vec3 color=vec3(0);
    for(int index=0;index<int(frame.lighting.z);++index) {
        vec3 l=frame.light_directions[index].xyz;
        vec3 h=v+l;
        h=dot(h,h)>0 ? normalize(h):n;
        float nl=max(dot(n,l),0),nh=max(dot(n,h),0),vh=max(dot(v,h),0);
        vec3 fresnel=f0+(1-f0)*pow(1-vh,5);
        float denom=nh*nh*(a2-1)+1;
        float distribution=a2/(pi*denom*denom);
        float visibility=0.5/max(nl*sqrt(nv*nv*(1-a2)+a2)+nv*sqrt(nl*nl*(1-a2)+a2),0.0001);
        vec3 diffuse_fresnel=specular_glossiness ? fresnel:vec3(0.04+0.96*pow(1-vh,5));
        vec3 diffuse=(1-diffuse_fresnel)*diffuse_color/pi;
        color+=(diffuse+distribution*visibility*fresnel)*nl*frame.light_radiances[index].rgb;
    }
    float occlusion=mix(1,texture(occlusion_texture,uv(3)).r,material.factors.w);
    vec3 reflected=reflect(-v,n);
    vec3 irradiance=texture(diffuse_environment,environment_uv(n)).rgb;
    float lod_roughness=frame.environment_lod.y==1 ? roughness:pow(roughness,frame.environment_lod.y);
    float environment_level=clamp(lod_roughness*frame.environment_lod.x,0,frame.options.w);
    vec3 prefiltered=textureLod(specular_environment,environment_uv(reflected),environment_level).rgb;
    vec2 integrated=texture(brdf_lut,vec2(nv,roughness)).rg;
    vec3 diffuse_f0=specular_glossiness ? f0:vec3(0.04);
    vec3 ambient_fresnel=diffuse_f0+(max(vec3(1-roughness),diffuse_f0)-diffuse_f0)*pow(1-nv,5);
    color+=((1-ambient_fresnel)*diffuse_color*irradiance+prefiltered*(f0*integrated.x+integrated.y))*occlusion*frame.lighting.x;
    color+=material.emissive.rgb*texture(emissive_texture,uv(4)).rgb;
    if(debug_view==1) color=base.rgb;
    if(debug_view==2) color=n*0.5+0.5;
    if(debug_view==3) color=vec3(metallic);
    if(debug_view==4) color=vec3(roughness);
    if(debug_view==5) color=material.emissive.rgb*texture(emissive_texture,uv(4)).rgb;
    result=vec4(debug_view==0 ? tone_map(color):color,alpha);
}

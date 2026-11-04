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
    vec4 balance_shadows;
    vec4 balance_midtones;
    vec4 balance_highlights;
} display;
vec3 encoded(vec3 color) {
    color=max(color,0);
    return mix(12.92*color,1.055*pow(color,vec3(1.0/2.4))-0.055,
               greaterThan(color,vec3(0.0031308)));
}
vec3 linear(vec3 color) {
    color=max(color,0);
    return mix(color/12.92,pow((color+0.055)/1.055,vec3(2.4)),
               greaterThan(color,vec3(0.04045)));
}
vec3 sample_color(vec2 position) {
    return texture(source,min(position,1.0-1.0/vec2(textureSize(source,0)))).rgb;
}
float green(vec2 position) { return sample_color(position).g; }
vec3 antialiased(vec2 position) {
    vec2 pixel=display.screen.xy;
    float center=green(position);
    float north=green(position-vec2(0,pixel.y));
    float south=green(position+vec2(0,pixel.y));
    float west=green(position-vec2(pixel.x,0));
    float east=green(position+vec2(pixel.x,0));
    float low=min(center,min(min(north,south),min(west,east)));
    float high=max(center,max(max(north,south),max(west,east)));
    float range=high-low;
    if(range<max(0.0625,high*0.125)) return sample_color(position);
    float nw=green(position-pixel);
    float ne=green(position+vec2(pixel.x,-pixel.y));
    float sw=green(position+vec2(-pixel.x,pixel.y));
    float se=green(position+pixel);
    float horizontal=abs(nw+sw-2*west)+2*abs(north+south-2*center)+abs(ne+se-2*east);
    float vertical=abs(nw+ne-2*north)+2*abs(west+east-2*center)+abs(sw+se-2*south);
    bool across_y=horizontal>=vertical;
    float negative=across_y ? north : west;
    float positive=across_y ? south : east;
    float negative_gradient=abs(negative-center);
    float positive_gradient=abs(positive-center);
    bool use_negative=negative_gradient>=positive_gradient;
    float gradient=max(negative_gradient,positive_gradient);
    float edge_average=0.5*(center+(use_negative ? negative : positive));
    float sign_step=use_negative ? -1 : 1;
    vec2 normal=across_y ? vec2(0,pixel.y) : vec2(pixel.x,0);
    vec2 tangent=across_y ? vec2(pixel.x,0) : vec2(0,pixel.y);
    vec2 midpoint=position+normal*(0.5*sign_step);
    vec2 start=midpoint-tangent;
    vec2 end=midpoint+tangent;
    float start_delta=green(start)-edge_average;
    float end_delta=green(end)-edge_average;
    bool start_done=abs(start_delta)>=gradient*0.25;
    bool end_done=abs(end_delta)>=gradient*0.25;
    const float distances[4]=float[4](1.5,2,4,12);
    for(int search=0;search<4;++search) {
        if(start_done && end_done) break;
        if(!start_done) start-=tangent*distances[search];
        if(!end_done) end+=tangent*distances[search];
        if(search==3) break;
        if(!start_done) {
            start_delta=green(start)-edge_average;
            start_done=abs(start_delta)>=gradient*0.25;
        }
        if(!end_done) {
            end_delta=green(end)-edge_average;
            end_done=abs(end_delta)>=gradient*0.25;
        }
    }
    float before=across_y ? position.x-start.x : position.y-start.y;
    float after=across_y ? end.x-position.x : end.y-position.y;
    bool nearest_negative=before<after;
    float nearest_delta=nearest_negative ? start_delta : end_delta;
    bool crosses=(nearest_delta<0)!=(center-edge_average<0);
    float edge_offset=crosses ? 0.5-min(before,after)/(before+after) : 0;
    float average=(2*(north+south+west+east)+nw+ne+sw+se)/12;
    float subpixel=clamp(abs(average-center)/range,0,1);
    subpixel=subpixel*subpixel*(3-2*subpixel);
    subpixel=subpixel*subpixel*0.75;
    return sample_color(position+normal*(sign_step*max(edge_offset,subpixel)));
}
float noise(vec2 coordinate) {
    vec3 point=fract(vec3(coordinate.xyx)*0.1031);
    point+=dot(point,point.yzx+19.19);
    float value=2*fract((point.x+point.y)*point.z)-1;
    float triangular=value==0 ? 0 : sign(value)*sqrt(abs(value));
    return (triangular-sign(value)+1)*0.5;
}
void main() {
    float alpha=clamp(texture(source,uv).a,0,1);
    if(alpha<=0) { result=vec4(0);return; }
    vec3 color=display.post.x>0 ? antialiased(uv) : sample_color(uv);
    float amount=clamp(texture(coverage,uv).a,0,1);
    if(display.post.y>0 && amount>0) {
        vec2 step_size=display.screen.xy*display.screen.z;
        vec3 neighbors=sample_color(uv-step_size)+sample_color(uv+step_size)+
            sample_color(uv+vec2(step_size.x,-step_size.y))+
            sample_color(uv+vec2(-step_size.x,step_size.y));
        color+=display.post.y*amount*(4*color-neighbors);
    }
    if(display.vignette.z>0) {
        float radius=distance(uv,display.vignette.xy);
        float attenuation=1-display.grading.y*smoothstep(display.grading.z,display.grading.w,radius);
        color=encoded(linear(color)*attenuation);
    }
    if(display.post.z>0) {
        vec2 coordinate=vec2(gl_FragCoord.x,display.screen.w-gl_FragCoord.y);
        float grain=noise(coordinate+1000*fract(display.post.w));
        color=mix(color,color*(color+(1-color)*2*grain),display.post.z);
    }
    result=vec4(linear(color)*alpha,alpha);
}

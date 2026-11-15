#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform VertParams {
  vec4 hvdf_offset;
  mat4 camera;
  float fog_constant;
  float _pad_fog_constant_0;
  float _pad_fog_constant_1;
  float _pad_fog_constant_2;
  float fog_min;
  float _pad_fog_min_0;
  float _pad_fog_min_1;
  float _pad_fog_min_2;
  float fog_max;
  float _pad_fog_max_0;
  float _pad_fog_max_1;
  float _pad_fog_max_2;
  int decal;
  float _pad_decal_0;
  float _pad_decal_1;
  float _pad_decal_2;
  vec4 persp0;
  vec4 persp1;
  mat4 cam_no_persp;
};

layout (location = 0) in vec3 position_in;
layout (location = 1) in vec3 tex_coord_in;
layout (location = 2) in int time_of_day_index;






// GLES has no 1D textures/samplers at all -- the C++ side now creates this as a GL_TEXTURE_2D
// with height 1, so indexing needs an explicit y=0 via ivec2 below.
layout(binding = 10) uniform highp sampler2D tex_T10;


layout(location = 0) out vec4 fragment_color;
layout(location = 1) out vec3 tex_coord;
layout(location = 2) out float fogginess;

// etie stuff




void main() {
  float fog1 = camera[3].w + camera[0].w * position_in.x + camera[1].w * position_in.y + camera[2].w * position_in.z;
  fogginess = 255. - clamp(fog1 + hvdf_offset.w, fog_min, fog_max);
  vec4 vf17 = cam_no_persp[3];
  vf17 += cam_no_persp[0] * position_in.x;
  vf17 += cam_no_persp[1] * position_in.y;
  vf17 += cam_no_persp[2] * position_in.z;
  vec4 p_proj = vec4(persp1.x * vf17.x, persp1.y * vf17.y, persp1.z, persp1.w);
  p_proj += persp0 * vf17.z;

  float pQ = 1.f / p_proj.w;
  vec4 transformed = p_proj * pQ;
  transformed.w = p_proj.w;

  // correct xy offset
  transformed.xy -= (2048.);
  // correct z scale
  transformed.z /= (8388608.);
  transformed.z -= 1.;
  // correct xy scale
  transformed.x /= (256.);
  transformed.y /= -(128.);
  // hack
  transformed.xyz *= transformed.w;
  // scissoring area adjust
  transformed.y *= (512.0 / 448.0) * 1.0;
  gl_Position = transformed;



  if (decal == 1) {
    fragment_color = vec4(1.0, 1.0, 1.0, 1.0);
  } else {
    // time of day lookup
    fragment_color = texelFetch(tex_T10, ivec2(time_of_day_index, 0), 0);
    // color adjustment
    fragment_color *= 2.;
    fragment_color.a *= 2.;
  }

  tex_coord = tex_coord_in;
}

#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  float alpha_min;
  float _pad_alpha_min_0;
  float _pad_alpha_min_1;
  float _pad_alpha_min_2;
  float alpha_max;
  float _pad_alpha_max_0;
  float _pad_alpha_max_1;
  float _pad_alpha_max_2;
  vec4 fog_color;
  int gfx_hack_no_tex;
  float _pad_gfx_hack_no_tex_0;
  float _pad_gfx_hack_no_tex_1;
  float _pad_gfx_hack_no_tex_2;
};

out vec4 color;

layout(location = 0) in vec4 fragment_color;
layout(location = 1) in vec3 tex_coord;
layout(location = 2) in float fogginess;
layout(binding = 0) uniform highp sampler2D tex_T0;








void main() {
  if (gfx_hack_no_tex == 0) {
    //vec4 T0 = texture(tex_T0, tex_coord);
    vec4 T0 = texture(tex_T0, tex_coord.xy);
    color = fragment_color * T0;
  } else {
    color = fragment_color/2.;
  }

  if (color.a < alpha_min || color.a > alpha_max) {
    discard;
  }

  color.rgb = mix(color.rgb, fog_color.rgb, clamp(fogginess * fog_color.a, 0., 1.));
}

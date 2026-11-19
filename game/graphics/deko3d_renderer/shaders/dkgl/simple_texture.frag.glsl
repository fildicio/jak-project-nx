#version 310 es
precision highp float;
precision highp int;
layout(std140, binding = 7) uniform FragParams {
  vec4 color;
};

out vec4 out_color;
layout(location = 0) in vec2 tex;

layout(binding = 0) uniform highp sampler2D tex_T0;

void main() {
  out_color = texture(tex_T0, tex) * color;
}
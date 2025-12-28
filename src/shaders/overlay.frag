#version 450

layout(location = 0) in vec2 inTexCoord;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D overlayTex;

layout(push_constant) uniform PushConstants {
    float alpha;
} pc;

void main() {
    vec4 texColor = texture(overlayTex, inTexCoord);
    outColor = texColor * pc.alpha;
}

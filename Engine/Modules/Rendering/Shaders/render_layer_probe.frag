#version 450
layout(location = 0) flat in vec4 instancePayload;
layout(location = 0) out vec4 color;
void main() { color = instancePayload; }

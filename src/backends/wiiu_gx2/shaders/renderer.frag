#version 330

uniform sampler2D uTexture;

in vec2 vTexCoord;
in vec4 vColor;
out vec4 fragmentColor;

void main() {
    fragmentColor = texture(uTexture, vTexCoord) * vColor;
}

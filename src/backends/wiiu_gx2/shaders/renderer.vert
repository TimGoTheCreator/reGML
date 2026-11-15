#version 330

in vec2 aPosition;
in vec2 aTexCoord;
in vec4 aColor;

out vec2 vTexCoord;
out vec4 vColor;

void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}

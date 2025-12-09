#version 330 core
in vec3 vertexColor;
out vec4 FragColor;

// if u_useOverride == 1 then use u_overrideColor;
otherwise use vertexColor
uniform int u_useOverride;
uniform vec3 u_overrideColor;
uniform float u_overrideAlpha;
// optional alpha (default 1.0)

void main()
{
    vec3 col = (u_useOverride == 1) ? u_overrideColor : vertexColor;
float a = (u_overrideAlpha > 0.0) ? u_overrideAlpha : 1.0;
    FragColor = vec4(col, a);
}
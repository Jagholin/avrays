#include <raylib.h>
#include <raymath.h>
#include <unistd.h>

int main() {
  InitWindow(1024, 768, "Texture and shader demo");
  Image pic = LoadImage("picture.png");
  Texture2D tex = LoadTextureFromImage(pic);
  const char *vs_shader = "#version 330\n"
                          "in vec3 vertexPosition;\n"
                          "in vec2 vertexTexCoord;\n"
                          "uniform mat4 mvp;\n"
                          "out vec2 fragTexCoord;\n"
                          "void main() {\n"
                          " fragTexCoord = vertexTexCoord; \n"
                          " gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
                          "}\n";

  const char *fs_shader =
      "#version 330\n"
      "in vec2 fragTexCoord;\n"
      "out vec4 finalColor2;\n"
      "uniform sampler2D texture0;\n"
      "void main() {\n"
      " finalColor2 = texture(texture0, fragTexCoord).grba;\n"
      "}\n";
  Shader alt_shader = LoadShaderFromMemory(vs_shader, fs_shader);

  while (!WindowShouldClose()) {
    ClearBackground(BLACK);
    BeginDrawing();

    BeginShaderMode(alt_shader);
    DrawTexture(tex, 0, 0, WHITE);
    EndShaderMode();

    EndDrawing();
  }

  UnloadTexture(tex);
  UnloadImage(pic);
  CloseWindow();
  return 0;
}

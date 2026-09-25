// Run from the source root with Qt's DLL and platform-plugin paths configured.
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_4_1_Core>
#include <QFile>
#include <QSurfaceFormat>
#include <iostream>

int main(int argc, char** argv)
{
  QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
  QGuiApplication app(argc, argv);
  QSurfaceFormat format;
  format.setVersion(4, 1);
  format.setProfile(QSurfaceFormat::CoreProfile);
  QOpenGLContext context;
  context.setFormat(format);
  if (!context.create()) { std::cerr << "OpenGL context unavailable\n"; return 1; }
  QOffscreenSurface surface;
  surface.setFormat(context.format());
  surface.create();
  if (!context.makeCurrent(&surface)) { std::cerr << "Offscreen surface unavailable\n"; return 1; }
  auto gl = context.versionFunctions<QOpenGLFunctions_4_1_Core>();
  if (!gl || !gl->initializeOpenGLFunctions()) return 1;
  // Match Noggit's raw shaderSource path, without Qt's shader preamble injection.
  auto compile = [gl](GLenum type, char const* path) -> GLuint {
    QFile file(path);
    if (!file.open(QFile::ReadOnly | QFile::Text)) return 0;
    auto source = file.readAll();
    auto pointer = source.constData();
    GLuint shader = gl->glCreateShader(type);
    gl->glShaderSource(shader, 1, &pointer, nullptr);
    gl->glCompileShader(shader);
    GLint passed = 0;
    gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &passed);
    if (!passed)
    {
      char log[8192]{};
      gl->glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
      std::cerr << log;
      gl->glDeleteShader(shader);
      return 0;
    }
    return shader;
  };
  GLuint vertex = compile(GL_VERTEX_SHADER, "src/noggit/rendering/glsl/terrain_vert.glsl");
  GLuint fragment = compile(GL_FRAGMENT_SHADER, "src/noggit/rendering/glsl/terrain_frag.glsl");
  if (!vertex || !fragment) return 1;
  GLuint program = gl->glCreateProgram();
  gl->glAttachShader(program, vertex);
  gl->glAttachShader(program, fragment);
  gl->glLinkProgram(program);
  GLint linked = 0;
  gl->glGetProgramiv(program, GL_LINK_STATUS, &linked);
  if (!linked)
  {
    char log[8192]{};
    gl->glGetProgramInfoLog(program, sizeof(log), nullptr, log);
    std::cerr << log;
    return 1;
  }
  if (gl->glGetUniformLocation(program, "painted_selection_color") < 0
      || gl->glGetUniformLocation(program, "draw_painted_stamp_selection") < 0)
  { std::cerr << "Selection uniforms missing\n"; return 1; }
  std::cout << "Terrain vertex/fragment shaders compiled and linked on OpenGL "
            << context.format().majorVersion() << '.' << context.format().minorVersion()
            << "; selection uniforms active.\n";
  gl->glDeleteProgram(program);
  gl->glDeleteShader(vertex);
  gl->glDeleteShader(fragment);
  return 0;
}

#include "core/gl/GLTextureWidget.h"

#include <QMouseEvent>
#include <QWheelEvent>

namespace core::gl {
namespace {
const char* kVert = R"(#version 330 core
layout(location=0) in vec2 aPos;
out vec2 vUv;
uniform vec2 uScale; uniform vec2 uOffset;
void main(){ vUv = vec2(aPos.x*0.5+0.5, 0.5 - aPos.y*0.5); gl_Position = vec4(aPos*uScale+uOffset,0.0,1.0); }
)";
const char* kFrag = R"(#version 330 core
in vec2 vUv; out vec4 FragColor;
uniform sampler2D uTex; uniform int uChannel; uniform int uChecker;
void main(){
    vec4 c = texture(uTex, vUv);
    vec3 rgb; float a = 1.0;
    if      (uChannel==0){ rgb=c.rgb; a=c.a; }
    else if (uChannel==1) rgb=c.rrr;
    else if (uChannel==2) rgb=c.ggg;
    else if (uChannel==3) rgb=c.bbb;
    else                  rgb=c.aaa;
    if (uChecker==1 && uChannel==0){
        vec2 b=floor(gl_FragCoord.xy/8.0); float chk=mod(b.x+b.y,2.0);
        vec3 bg=mix(vec3(0.40),vec3(0.55),chk);
        FragColor=vec4(mix(bg,rgb,a),1.0);
    } else FragColor=vec4(rgb, uChannel==0 ? a : 1.0);
}
)";
}

GLTextureWidget::GLTextureWidget(QWidget* parent) : QOpenGLWidget(parent) { setMouseTracking(true); }
GLTextureWidget::~GLTextureWidget()
{
    if (context()) { makeCurrent();
        if (m_tex) glDeleteTextures(1, &m_tex);
        if (m_prog) glDeleteProgram(m_prog);
        if (m_vbo) glDeleteBuffers(1, &m_vbo);
        if (m_vao) glDeleteVertexArrays(1, &m_vao);
        doneCurrent(); }
}

void GLTextureWidget::setImage(const QImage& img)
{
    m_image = img.convertToFormat(QImage::Format_RGBA8888);
    m_pendingUpload = true; resetView(); update();
}
void GLTextureWidget::clearImage() { m_image = QImage(); m_pendingUpload = false; if (m_tex && context()) { makeCurrent(); glDeleteTextures(1, &m_tex); m_tex = 0; doneCurrent(); } update(); }
void GLTextureWidget::setChannel(Channel c) { m_channel = c; update(); }
void GLTextureWidget::setCheckerboard(bool on) { m_checker = on; update(); }
void GLTextureWidget::resetView() { m_zoom = 1; m_panX = m_panY = 0; update(); }

void GLTextureWidget::initializeGL()
{
    if (!initializeOpenGLFunctions()) { m_glBroken = true; return; }
    // A re-initialisation means the previous context is gone with everything in it.
    m_prog = m_vao = m_vbo = m_tex = 0;
    m_pendingUpload = !m_image.isNull();
    glClearColor(0.10f, 0.10f, 0.11f, 1.0f);
    auto compile = [this](GLenum t, const char* s) { GLuint sh = glCreateShader(t); glShaderSource(sh, 1, &s, nullptr); glCompileShader(sh); return sh; };
    GLuint vs = compile(GL_VERTEX_SHADER, kVert), fs = compile(GL_FRAGMENT_SHADER, kFrag);
    m_prog = glCreateProgram(); glAttachShader(m_prog, vs); glAttachShader(m_prog, fs); glBindAttribLocation(m_prog, 0, "aPos"); glLinkProgram(m_prog);
    glDeleteShader(vs); glDeleteShader(fs);
    static const float quad[] = {-1,-1, 1,-1, -1,1, 1,1};
    glGenVertexArrays(1, &m_vao); glBindVertexArray(m_vao);
    glGenBuffers(1, &m_vbo); glBindBuffer(GL_ARRAY_BUFFER, m_vbo); glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2*sizeof(float), nullptr); glBindVertexArray(0);
}

void GLTextureWidget::uploadPending()
{
    m_pendingUpload = false;
    if (m_tex) { glDeleteTextures(1, &m_tex); m_tex = 0; }
    if (m_image.isNull()) return;
    glGenTextures(1, &m_tex); glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_image.width(), m_image.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, m_image.constBits());
    m_texW = m_image.width(); m_texH = m_image.height();
}

void GLTextureWidget::resizeGL(int w, int h) { glViewport(0, 0, w, h); }

void GLTextureWidget::paintGL()
{
    if (m_glBroken) return;
    if (m_pendingUpload) uploadPending();
    glClear(GL_COLOR_BUFFER_BIT);
    if (!m_tex || !m_prog) return;
    float sx = 1, sy = 1;
    const float wA = height() > 0 ? float(width())/height() : 1, tA = m_texH > 0 ? float(m_texW)/m_texH : 1;
    if (tA > wA) sy = wA/tA; else sx = tA/wA;
    glUseProgram(m_prog);
    glUniform2f(glGetUniformLocation(m_prog, "uScale"), sx*m_zoom, sy*m_zoom);
    glUniform2f(glGetUniformLocation(m_prog, "uOffset"), m_panX, m_panY);
    glUniform1i(glGetUniformLocation(m_prog, "uChannel"), int(m_channel));
    glUniform1i(glGetUniformLocation(m_prog, "uChecker"), m_checker ? 1 : 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_tex);
    glUniform1i(glGetUniformLocation(m_prog, "uTex"), 0);
    glBindVertexArray(m_vao); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); glBindVertexArray(0);
}

bool GLTextureWidget::widgetToPixel(const QPoint& p, QPoint& px) const
{
    if (m_image.isNull() || width() <= 0 || height() <= 0) return false;
    float sx = 1, sy = 1;
    const float wA = float(width())/height(), tA = m_texH > 0 ? float(m_texW)/m_texH : 1;
    if (tA > wA) sy = wA/tA; else sx = tA/wA;
    sx *= m_zoom; sy *= m_zoom;
    const float ndcX = float(p.x())/width()*2 - 1, ndcY = 1 - float(p.y())/height()*2;
    if (sx == 0 || sy == 0) return false;
    const float ax = (ndcX - m_panX)/sx, ay = (ndcY - m_panY)/sy;
    if (ax < -1 || ax > 1 || ay < -1 || ay > 1) return false;
    px.setX(int((ax*0.5f + 0.5f) * m_texW)); px.setY(int((0.5f - ay*0.5f) * m_texH));
    px.setX(qBound(0, px.x(), m_texW-1)); px.setY(qBound(0, px.y(), m_texH-1));
    return true;
}

void GLTextureWidget::wheelEvent(QWheelEvent* e) { if (m_image.isNull()) return; m_zoom = qBound(0.1f, m_zoom * (e->angleDelta().y() > 0 ? 1.15f : 1/1.15f), 60.0f); update(); e->accept(); }
void GLTextureWidget::mousePressEvent(QMouseEvent* e) { if (e->button() == Qt::LeftButton) { m_dragging = true; m_lastPos = e->pos(); setCursor(Qt::ClosedHandCursor); } }
void GLTextureWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (m_dragging) { const QPoint d = e->pos() - m_lastPos; m_lastPos = e->pos();
        if (width() > 0) m_panX += 2.f*d.x()/width();
        if (height() > 0) m_panY -= 2.f*d.y()/height();
        update(); return; }
    QPoint px;
    if (widgetToPixel(e->pos(), px)) emit hoverPixel(px, m_image.pixelColor(px));
    else emit hoverPixel(QPoint(-1, -1), QColor());
}
void GLTextureWidget::mouseReleaseEvent(QMouseEvent* e) { if (e->button() == Qt::LeftButton) { m_dragging = false; setCursor(Qt::ArrowCursor); } }
void GLTextureWidget::mouseDoubleClickEvent(QMouseEvent*) { resetView(); m_dragging = false; setCursor(Qt::ArrowCursor); }
void GLTextureWidget::leaveEvent(QEvent*) { emit hoverPixel(QPoint(-1, -1), QColor()); }

}  // namespace core::gl

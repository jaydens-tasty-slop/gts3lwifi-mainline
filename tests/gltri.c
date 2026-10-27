/* Headless GLES test: surfaceless EGL, render a triangle into an FBO, read pixels back. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <string.h>
static const char *vs = "attribute vec2 p; void main(){ gl_Position = vec4(p,0.0,1.0); }";
static const char *fs = "precision mediump float; void main(){ gl_FragColor = vec4(0.0,1.0,0.0,1.0); }";
static GLuint sh(GLenum t, const char *s) { GLuint o = glCreateShader(t); glShaderSource(o,1,&s,0); glCompileShader(o); GLint ok; glGetShaderiv(o,GL_COMPILE_STATUS,&ok); if(!ok){char l[512];glGetShaderInfoLog(o,512,0,l);printf("shader: %s\n",l);} return o; }
int main(void) {
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	if (!eglInitialize(d, 0, 0)) { printf("eglInitialize failed\n"); return 1; }
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, ca);
	if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) { printf("makecurrent failed\n"); return 1; }
	printf("renderer: %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	GLuint fb, rb; glGenFramebuffers(1,&fb); glBindFramebuffer(GL_FRAMEBUFFER,fb);
	glGenRenderbuffers(1,&rb); glBindRenderbuffer(GL_RENDERBUFFER,rb);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA4, 256, 256);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
	printf("fbo status: 0x%x\n", glCheckFramebufferStatus(GL_FRAMEBUFFER));
	glViewport(0,0,256,256); glClearColor(1,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
	GLuint pr = glCreateProgram(); glAttachShader(pr, sh(GL_VERTEX_SHADER,vs)); glAttachShader(pr, sh(GL_FRAGMENT_SHADER,fs));
	glBindAttribLocation(pr,0,"p"); glLinkProgram(pr); glUseProgram(pr);
	float v[] = { -0.8f,-0.8f, 0.8f,-0.8f, 0.0f,0.8f };
	glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,v); glEnableVertexAttribArray(0);
	glDrawArrays(GL_TRIANGLES,0,3); glFinish();
	unsigned char px[4];
	glReadPixels(128,128,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px); printf("center (inside triangle): %d %d %d %d\n",px[0],px[1],px[2],px[3]);
	glReadPixels(5,250,1,1,GL_RGBA,GL_UNSIGNED_BYTE,px);   printf("corner (clear colour):  %d %d %d %d\n",px[0],px[1],px[2],px[3]);
	printf("glGetError: 0x%x\n", glGetError());
	return 0;
}

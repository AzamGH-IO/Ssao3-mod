// SSAO untuk GTA SA 2.10 (arm64-v8a) via AML. BELUM DIUJI - ikut README bawah.
// Bina: NDK, -std=c++17, link -lEGL -lGLESv2 -llog, output libSSAO.so
#include <dlfcn.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "aml/amlmod.h"   // semak nama API Hook dalam versi AML anda

MYMOD(net.user.ssao, GTASA_SSAO, 0.1, user)

// ---- Tetapan (laras ikut prestasi / rupa) ----
static const float RADIUS    = 0.6f;   // jejari sampel (unit dunia)
static const float INTENSITY = 1.2f;
static const float ZNEAR     = 0.1f;   // anggaran; laras jika AO nampak salah
static const float ZFAR      = 300.0f; // anggaran
static const int   SAMPLES   = 8;

static EGLBoolean (*orig_swap)(EGLDisplay, EGLSurface);
static void (*orig_bindFb)(GLenum, GLuint);

static GLuint fbo, colorTex, depthTex, prog, vbo;
static int W, H;
static bool inited, inPass;

static const char* VS =
 "attribute vec2 p; varying vec2 uv;"
 "void main(){ uv=p*0.5+0.5; gl_Position=vec4(p,0.,1.); }";

static const char* FS =
 "precision mediump float; varying vec2 uv;"
 "uniform sampler2D uC; uniform sampler2D uD; uniform vec2 uPx;"
 "uniform float uN, uF, uR, uI;"
 "float lin(float d){ return uN*uF/(uF-d*(uF-uN)); }"
 "float rnd(vec2 c){ return fract(sin(dot(c,vec2(12.9898,78.233)))*43758.5453); }"
 "void main(){"
 " vec3 col=texture2D(uC,uv).rgb;"
 " float z=lin(texture2D(uD,uv).r);"
 " if(z>uF*0.9){ gl_FragColor=vec4(col,1.); return; }"
 " float occ=0.; float ang=rnd(uv/uPx)*6.2831;"
 " for(int i=0;i<8;i++){"
 "  float a=ang+float(i)*0.785398; float r=(float(i)+1.)/8.;"
 "  vec2 off=vec2(cos(a),sin(a))*r*uR*60./z*uPx*8.;"
 "  float zs=lin(texture2D(uD,uv+off).r);"
 "  float d=z-zs;"
 "  occ+=step(0.02,d)*smoothstep(uR*2.,0.,d);"
 " }"
 " float ao=1.-clamp(occ/8.*uI,0.,0.8);"
 " gl_FragColor=vec4(col*ao,1.);"
 "}";

static GLuint mk(GLenum t, const char* s){
    GLuint o=glCreateShader(t); glShaderSource(o,1,&s,nullptr); glCompileShader(o); return o;
}

static void init(EGLDisplay d, EGLSurface s){
    eglQuerySurface(d,s,EGL_WIDTH,&W); eglQuerySurface(d,s,EGL_HEIGHT,&H);
    glGenTextures(1,&colorTex); glBindTexture(GL_TEXTURE_2D,colorTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,W,H,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);

    glGenTextures(1,&depthTex); glBindTexture(GL_TEXTURE_2D,depthTex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_DEPTH_COMPONENT,W,H,0,GL_DEPTH_COMPONENT,GL_UNSIGNED_INT,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1,&fbo); orig_bindFb(GL_FRAMEBUFFER,fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,colorTex,0);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depthTex,0);
    // Jika permainan guna stencil, mungkin perlu GL_DEPTH_STENCIL_OES (extension).

    prog=glCreateProgram();
    glAttachShader(prog,mk(GL_VERTEX_SHADER,VS));
    glAttachShader(prog,mk(GL_FRAGMENT_SHADER,FS));
    glBindAttribLocation(prog,0,"p"); glLinkProgram(prog);

    const float q[]={-1,-1, 1,-1, -1,1, 1,1};
    glGenBuffers(1,&vbo); glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glBufferData(GL_ARRAY_BUFFER,sizeof(q),q,GL_STATIC_DRAW);
    inited=true;
}

// Permainan bind FB 0 -> kita tukar ke FBO kita
static void hook_bindFb(GLenum t, GLuint f){
    if(inited && !inPass && t==GL_FRAMEBUFFER && f==0) f=fbo;
    orig_bindFb(t,f);
}

static EGLBoolean hook_swap(EGLDisplay d, EGLSurface s){
    if(!inited){ init(d,s); orig_bindFb(GL_FRAMEBUFFER,fbo); return orig_swap(d,s); }

    // simpan keadaan
    GLint oProg,oAB,oTex0,oAct; GLint vp[4];
    GLboolean dt=glIsEnabled(GL_DEPTH_TEST), bl=glIsEnabled(GL_BLEND),
              cf=glIsEnabled(GL_CULL_FACE), sc=glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_CURRENT_PROGRAM,&oProg); glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&oAB);
    glGetIntegerv(GL_ACTIVE_TEXTURE,&oAct); glGetIntegerv(GL_VIEWPORT,vp);
    glActiveTexture(GL_TEXTURE0); glGetIntegerv(GL_TEXTURE_BINDING_2D,&oTex0);

    inPass=true;
    orig_bindFb(GL_FRAMEBUFFER,0);
    glViewport(0,0,W,H);
    glDisable(GL_DEPTH_TEST); glDisable(GL_BLEND); glDisable(GL_CULL_FACE); glDisable(GL_SCISSOR_TEST);

    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,colorTex);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,depthTex);
    glUniform1i(glGetUniformLocation(prog,"uC"),0);
    glUniform1i(glGetUniformLocation(prog,"uD"),1);
    glUniform2f(glGetUniformLocation(prog,"uPx"),1.f/W,1.f/H);
    glUniform1f(glGetUniformLocation(prog,"uN"),ZNEAR);
    glUniform1f(glGetUniformLocation(prog,"uF"),ZFAR);
    glUniform1f(glGetUniformLocation(prog,"uR"),RADIUS);
    glUniform1f(glGetUniformLocation(prog,"uI"),INTENSITY);
    glBindBuffer(GL_ARRAY_BUFFER,vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableVertexAttribArray(0);

    // pulih keadaan
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D,0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D,oTex0);
    glActiveTexture(oAct);
    glBindBuffer(GL_ARRAY_BUFFER,oAB); glUseProgram(oProg);
    glViewport(vp[0],vp[1],vp[2],vp[3]);
    if(dt) glEnable(GL_DEPTH_TEST); if(bl) glEnable(GL_BLEND);
    if(cf) glEnable(GL_CULL_FACE);  if(sc) glEnable(GL_SCISSOR_TEST);

    EGLBoolean r=orig_swap(d,s);
    orig_bindFb(GL_FRAMEBUFFER,fbo);   // frame seterusnya lukis ke FBO kita
    inPass=false;
    return r;
}

extern "C" void OnModLoad(){
    void* egl=dlopen("libEGL.so",RTLD_NOW);
    void* gl =dlopen("libGLESv2.so",RTLD_NOW);
    void* fSwap=dlsym(egl,"eglSwapBuffers");
    void* fBind=dlsym(gl,"glBindFramebuffer");
    // Nama fungsi hook mungkin berbeza ikut versi AML (Hook / HookInline)
    aml->Hook(fSwap,(void*)hook_swap,(void**)&orig_swap);
    aml->Hook(fBind,(void*)hook_bindFb,(void**)&orig_bindFb);
}

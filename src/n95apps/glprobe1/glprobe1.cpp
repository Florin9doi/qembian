// glprobe1: the smallest OpenGL ES 1.1 client of an S60 3rd FP1 phone's libgles_cm.dll (EGL 1.1 inside it): a full-
// screen window-server window, EGL on it, ~4 s of frames (the clear colour cycles, a white triangle on top), then
// exit. Every step's result goes to C:\data\glprobe1.txt, so a boot can tell whether the phone's GL path (sygl,
// docs/devices.md [gl]) works. Started by the agent: sydb rerun build/n95apps/glprobe1.exe
// SPDX-License-Identifier: MIT
#include <e32base.h>
#include <f32file.h>
#include <w32std.h>
#include <GLES/egl.h>
#include <GLES/gl.h>

static RFs Fs;
static RFile Log;

static void Say(const char* aFmt, TInt a = 0, TInt b = 0)
	{
	TBuf8<160> line;
	TPtrC8 fmt((const TUint8*)aFmt);
	line.Format(fmt, a, b);
	line.Append('\n');
	Log.Write(line);
	RDebug::Printf("GLPROBE1: %s", aFmt);
	}

static TInt Run()
	{
	RWsSession ws;
	TInt r = ws.Connect();
	Say("ws.Connect %d", r);
	if (r) return r;
	CWsScreenDevice* screen = new CWsScreenDevice(ws);
	screen->Construct();
	TSize size = screen->SizeInPixels();
	Say("screen %d x %d", size.iWidth, size.iHeight);
	RWindowGroup group(ws);
	group.Construct(0x5a5a, ETrue);
	group.SetOrdinalPosition(0, 10);
	RWindow win(ws);
	win.Construct(group, 0x5a5b);
	win.SetExtent(TPoint(0, 0), size);
	win.Activate();
	ws.Flush();

	EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint maj = 0, min = 0;
	Say("eglInitialize %d", eglInitialize(dpy, &maj, &min));
	Say("EGL %d.%d", maj, min);
	EGLint attrs[] = { EGL_BUFFER_SIZE, 16, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE };
	EGLConfig cfg;
	EGLint n = 0;
	Say("eglChooseConfig %d, %d configs", eglChooseConfig(dpy, attrs, &cfg, 1, &n), n);
	EGLSurface surf = eglCreateWindowSurface(dpy, cfg, &win, NULL);
	Say("surface %d (error 0x%x)", surf != EGL_NO_SURFACE, eglGetError());
	EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, NULL);
	Say("context %d", ctx != EGL_NO_CONTEXT);
	Say("eglMakeCurrent %d", eglMakeCurrent(dpy, surf, surf, ctx));
	const char* rend = (const char*)glGetString(GL_RENDERER);
	Log.Write(TPtrC8((const TUint8*)(rend ? rend : "(none)")));
	Log.Write(_L8(" = GL_RENDERER\n"));

	static const GLfloat tri[] = { -0.6f, -0.5f, 0.6f, -0.5f, 0.0f, 0.6f };
	glViewport(0, 0, size.iWidth, size.iHeight);
	TInt swaps = 0;
	for (TInt i = 0; i < 120; ++i)
		{
		glClearColor((i % 40) / 40.0f, 0.3f, 1.0f - (i % 40) / 40.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(2, GL_FLOAT, 0, tri);
		glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		swaps += eglSwapBuffers(dpy, surf) ? 1 : 0;
		User::After(30000);
		}
	Say("frames %d, swapped %d", 120, swaps);
	Say("glGetError 0x%x", glGetError());
	eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(dpy, ctx);
	eglDestroySurface(dpy, surf);
	eglTerminate(dpy);
	win.Close();
	group.Close();
	delete screen;
	ws.Close();
	return KErrNone;
	}

GLDEF_C TInt E32Main();

// ---- process start-up for devagent/startup.s (as eglprobe/oracle.cpp): heap, C++ runtime, E32Main ----
struct SDaThreadCreateInfo
	{
	TAny* iHandle;
	TInt iType;
	TThreadFunction iFunction;
	TAny* iPtr;
	};
struct SStdEpocThreadCreateInfo;
IMPORT_C TInt DaSetupThreadHeap(TBool, SStdEpocThreadCreateInfo&) asm("_ZN8UserHeap15SetupThreadHeapEiR24SStdEpocThreadCreateInfo");
IMPORT_C void DaExceptionsGlobals(TAny* aThis) asm("_ZN23TCppRTExceptionsGlobalsC1Ev");

extern "C" void DaRunThread(TBool aNotFirst, SDaThreadCreateInfo& aInfo)
	{
	TUint64 exceptionGlobals[64];
	TInt r = DaSetupThreadHeap(aNotFirst, (SStdEpocThreadCreateInfo&)aInfo);
	if (r == KErrNone)
		{
		DaExceptionsGlobals(exceptionGlobals);		// after the heap: its Dll::SetTls allocates
		if (aNotFirst)
			r = (*aInfo.iFunction)(aInfo.iPtr);
		else
			{
			User::InitProcess();
			r = E32Main();
			}
		}
	User::Exit(r);
	}

extern "C" void DaInvariant() { User::Invariant(); }
extern "C" void DaHandleException(TAny* aInfo) { User::HandleException(aInfo); }

GLDEF_C TInt E32Main()
	{
	CTrapCleanup* cleanup = CTrapCleanup::New();
	Fs.Connect();
	Fs.MkDirAll(_L("C:\\data\\"));
	Log.Replace(Fs, _L("C:\\data\\glprobe1.txt"), EFileWrite);
	TInt r = Run();						// nothing in it leaves (built without exceptions, as devagent)
	Say("done %d", r);
	Log.Close();
	Fs.Close();
	delete cleanup;
	return 0;
	}

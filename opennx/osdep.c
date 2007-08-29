/* $Id$
 *
 * Copyright (C) 2006 The OpenNX Team
 * Author: Fritz Elfert
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

/*
 * Defines canonicalized platform names (e.g. __LINUX__)
 */
#include <wx/platform.h>

#ifdef __WXMSW__
/* dummies for now */
char *x11_socket_path = "";
char *x11_keyboard_type = "";

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <stdlib.h>

static char _spath[_MAX_PATH+1];
static char _kbd[_MAX_PATH+1];


long getppid()
{
    OSVERSIONINFO osver;
    HINSTANCE hKernel32;
    HANDLE hSnapShot;
    PROCESSENTRY32 procentry;
    long mypid;
    long ppid = 0;

	/* ToolHelp Function Pointers.*/
    HANDLE (WINAPI *lpfCreateToolhelp32Snapshot)(DWORD,DWORD);
    BOOL (WINAPI *lpfProcess32First)(HANDLE,LPPROCESSENTRY32);
    BOOL (WINAPI *lpfProcess32Next)(HANDLE,LPPROCESSENTRY32);
	
    osver.dwOSVersionInfoSize = sizeof(osver);
    if (!GetVersionEx(&osver))
        return 0;
	if (osver.dwPlatformId != VER_PLATFORM_WIN32_NT)
        return 0;
    if ((hKernel32 = LoadLibraryA("kernel32.dll")) == NULL)
        return 0;
	lpfCreateToolhelp32Snapshot= (HANDLE(WINAPI *)(DWORD,DWORD))
        GetProcAddress(hKernel32, "CreateToolhelp32Snapshot");
	lpfProcess32First= (BOOL(WINAPI *)(HANDLE,LPPROCESSENTRY32))
        GetProcAddress(hKernel32, "Process32First");
    lpfProcess32Next= (BOOL(WINAPI *)(HANDLE,LPPROCESSENTRY32))
        GetProcAddress(hKernel32, "Process32Next");
    if (lpfProcess32Next == NULL || lpfProcess32First == NULL || lpfCreateToolhelp32Snapshot == NULL) {
        FreeLibrary(hKernel32);
        return 0;
    }
    hSnapShot = lpfCreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapShot == INVALID_HANDLE_VALUE) {
        FreeLibrary(hKernel32);
        return 0;
    }
    memset((LPVOID)&procentry,0,sizeof(PROCESSENTRY32));
    procentry.dwSize = sizeof(PROCESSENTRY32);
    mypid = GetCurrentProcessId();
    if (lpfProcess32First(hSnapShot, &procentry)) {
        do {
            if (mypid == (long)procentry.th32ProcessID) {
                ppid =  procentry.th32ParentProcessID;
                break;
            }
            procentry.dwSize = sizeof(PROCESSENTRY32);
        } while (lpfProcess32Next(hSnapShot, &procentry));
    }	
	FreeLibrary(hKernel32);
    return ppid;
}

#else /* ! __WXMSW__ */

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xmu/WinUtil.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dlfcn.h>
#include <limits.h>
#include <string.h>

typedef int (*PFconnect)(int s, const struct sockaddr *name, socklen_t namelen);
static PFconnect real_connect = NULL;
static void *libc = NULL;
static int do_save = 1;

static char _spath[PATH_MAX+1];
static char _kbd[PATH_MAX+1];

char *x11_socket_path = _spath;
char *x11_keyboard_type = _kbd;

/**
 * A horrific hack for retrieving the path of the X11 local server socket.
 *
 * This path is needed for setting the NX_TEMP environment variable before
 * starting nxssh. As NoMachine's code in libXcomp silently asumes, that
 * the socket is always in TEMP if NX_TEMP is not set. On my machine, TEMP is
 * set to /dev/shm for speed reasons, but the X11 socket still resides in
 * /tmp/.X11-unix/. With that setting, the original client fails immediately
 * upon connect.
 * Normally, there's no way to retrieve the socket path via some X11 function.
 * So here is ho it works:
 *
 *  1. We implement our own connect() function
 *  2. In getx11socket [called automatically during app-startup because of the
 *     __attribute__((constructor))], we load libc dynamically and set the
 *     function-pointer "real_connect" to the original function. Since our
 *     binary already contains a symbol connect(), the symbol connect() from
 *     libc is not used in normal relocation. Inside our connect, we simply
 *     call the original (now named "real_connect").
 *  3. Just call XOpenDisplay(). The Xlib function will do it's normal work and
 *     connect to the X server. In our connect() function, we save the path
 *     argument from this call in a global var.
 */ 
int connect(int s, const struct sockaddr *name, socklen_t namelen)
{
    if (do_save) {
        struct sockaddr_un *a = (struct sockaddr_un *)name;
        if (a->sun_family == AF_UNIX) {
            strncpy(_spath, a->sun_path, sizeof(_spath));
            do_save = 0;
        }
    }
    return real_connect(s, name, namelen);
}

#ifdef __WXMAC__
# define X11_APPLE "/Applications/Utilities/X11.app"
# define X11_DARWIN "/Applications/XDarwin.app"
static Display *launchX11() {
    struct stat st;
    do_save = 1;
    memset(&_spath, 0, sizeof(_spath));
    memset(&_kbd, 0, sizeof(_kbd));
    if (stat(X11_APPLE, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            system(X11_APPLE "/Contents/MacOS/X11 &");
            setenv("DISPLAY", ":0", 1);
            sleep(4);
            return XOpenDisplay(NULL);
        }
    }
    if (stat(X11_DARWIN, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            system(X11_DARWIN "/Contents/MacOS/XDarwin &");
            setenv("DISPLAY", ":0", 1);
            sleep(4);
            return XOpenDisplay(NULL);
        }
    }
    return NULL;
}
#endif

static void __attribute__ ((constructor))
getx11socket()
{
    memset(&_spath, 0, sizeof(_spath));
    memset(&_kbd, 0, sizeof(_kbd));
#define NotImplemented
#ifdef __LINUX__
    libc = dlopen("libc.so.6", RTLD_NOW);
# undef NotImplemented
#endif
#ifdef __OPENBSD__
    libc = dlopen("libc.so", RTLD_NOW);
# undef NotImplemented
#endif
#ifdef __WXMAC__
    libc = dlopen("libc.dylib", RTLD_NOW);
# undef NotImplemented
#endif
#ifdef NotImplemented
# error Specify libc name
#endif
    if (!libc) {
        fprintf(stderr, "Can't load libc: %s\n", dlerror());
        exit(1);
    }
    real_connect = dlsym(libc, "connect");
    if (!real_connect) {
        fprintf(stderr, "Can't find symbol connect in libc: %s\n", dlerror());
        exit(1);
    }
    Display *dpy = XOpenDisplay(NULL);
#ifdef __WXMAC__
    // Start X server, if necessary
    if (!dpy)
        dpy = launchX11();
#endif
    if (dpy) {
        Atom a = XInternAtom(dpy, "_XKB_RULES_NAMES", True);
        if (a != None) {
            Atom type;
            int fmt;
            unsigned long n;
            unsigned long ba;
            char *prop;
            int res = XGetWindowProperty(dpy,
                    RootWindowOfScreen(DefaultScreenOfDisplay(dpy)),
                    a, 0, PATH_MAX, False, AnyPropertyType,
                    &type, &fmt, &n, &ba, (unsigned char **)&prop);
            if ((res == Success) && (fmt == 8) && prop) {
                unsigned long i = 0;
                int idx = 0;
                while (i < n) {
                    switch (idx++) {
                        case 1:
                            strncat(_kbd, prop, PATH_MAX);
                            strncat(_kbd, "/", PATH_MAX);
                            break;
                        case 2:
                            strncat(_kbd, prop, PATH_MAX);
                            break;
                    }
                    i += strlen(prop) + 1;
                    prop += strlen(prop) + 1;
                }
            }
        }
        XCloseDisplay(dpy);
    }
}

static void __attribute__ ((destructor))
free_libc()
{
    if (libc)
        dlclose(libc);
}
#endif

/*
 * Close a foreign X11 window (just like a window-manager would do.
 */
void close_foreign(long parentID)
{
#if defined(__LINUX__) || defined(__OPENBSD__) || defined(__WXMAC__)
    Display *dpy = XOpenDisplay(NULL);
    if (dpy) {
        XClientMessageEvent ev;
        ev.type = ClientMessage;
        ev.window = parentID;
        ev.message_type = XInternAtom(dpy, "WM_PROTOCOLS", True);
        ev.format = 32;
        ev.data.l[0] = XInternAtom(dpy, "WM_DELETE_WINDOW", True);
        ev.data.l[1] = CurrentTime;
        XSendEvent(dpy, parentID, False, 0, (XEvent *)&ev);
        XCloseDisplay(dpy);
    }
#else
	(void)parentID;
#pragma message(__FILE__ " : warning: Implement close_foreign")
#endif
}

/*
 * Reparent our custom toolbar to the top center of a given foreign
 * X11 window. Since there is no way to retrieve our toolbar's X11-WindowID,
 * we use the _NET_WM_PID property (which automatically get's set by gdk), to find
 * our window. (At the time of the call it obviously should be the only one).
 * After that, proceed with a standard X11 reparenting ...
 */
void reparent_pulldown(long parentID)
{
#if defined(__LINUX__) || defined(__OPENBSD__) || defined(__WXMAC__)
    Display *dpy = XOpenDisplay(NULL);
    if (dpy) {
        Atom a = XInternAtom(dpy, "_NET_WM_PID", True);
        if (a != None) {
            int s;
            for (s = 0; s < ScreenCount(dpy); s++) {
                Window root = RootWindow(dpy, s);
                Window dummy;
                Window *childs = NULL;
                unsigned int nchilds = 0;
                if (XQueryTree(dpy, root, &dummy, &dummy, &childs, &nchilds)) {
                    int c;
                    for (c = 0; c < nchilds; c++) {
                        Window w = childs[c];
                        if (w != None) {
                            Atom type;
                            int fmt;
                            unsigned long n;
                            unsigned long ba;
                            unsigned long *prop;
                            int res = XGetWindowProperty(dpy, w, a, 0, 32, False,
                                    XA_CARDINAL, &type, &fmt, &n, &ba, (unsigned char **)&prop);
                            if ((res == Success) && (fmt = 32) && (n == 1) && prop) {
                                if (*prop == getpid()) {
                                    int dummy;
                                    unsigned int pw, cw, udummy;
                                    Window wdummy;

                                    XGetGeometry(dpy, w, &wdummy, &dummy, &dummy,
                                            &cw, &udummy, &udummy, &udummy);
                                    XGetGeometry(dpy, parentID, &wdummy, &dummy, &dummy,
                                            &pw, &udummy, &udummy, &udummy);
                                    XReparentWindow(dpy, w, parentID, pw / 2 - cw / 2, 0);
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }
        XCloseDisplay(dpy);
    }
#else
	(void)parentID;
#pragma message(__FILE__ " : warning: Implement reparent_pulldown")
#endif
}

void close_sid(const char *sid)
{
#if defined(__LINUX__) || defined(__OPENBSD__) || defined(__WXMAC__)
    Display *dpy = XOpenDisplay(NULL);
    int closed = 0;
    if (dpy) {
        int s;
        for (s = 0; s < ScreenCount(dpy); s++) {
            Window root = RootWindow(dpy, s);
            Window dummy;
            Window *childs = NULL;
            unsigned int nchilds = 0;
            if (XQueryTree(dpy, root, &dummy, &dummy, &childs, &nchilds)) {
                int c;
                for (c = 0; c < nchilds; c++) {
                    Window w = XmuClientWindow(dpy, childs[c]);
                    if (w != None) {
                        char **cargv = NULL;
                        int cargc = 0;
                        if (XGetCommand(dpy, w, &cargv, &cargc)) { 
                            if ((cargc > 3) &&
                                    (strcmp(cargv[0], "/usr/NX/bin/nxagent") == 0) &&
                                    (strcmp(cargv[1], "-D") == 0) &&
                                    (strcmp(cargv[2], "-options") == 0) &&
                                    (strstr(cargv[3], sid) != NULL)) {
                                close_foreign(w);
                                closed = 1;
                                XFreeStringList(cargv);
                                break;
                            }
                        }
                        XFreeStringList(cargv);
                    }
                }
            }
            if (closed)
                break;
        }
        XCloseDisplay(dpy);
    }
#else
	(void)sid;
#pragma message(__FILE__ " : warning: Implement reparent_pulldown")
#endif
}


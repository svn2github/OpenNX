// $Id$
//
// Copyright (C) 2006 The OpenNX Team
// Author: Fritz Elfert
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU Library General Public License as
// published by the Free Software Foundation; either version 2 of the
// License, or (at your option) any later version.
// 
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU Library General Public
// License along with this program; if not, write to the
// Free Software Foundation, Inc.,
// 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
//

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#if defined(__GNUG__) && !defined(__APPLE__)
#pragma implementation "SessionList.h"
#endif

// For compilers that support precompilation, includes "wx/wx.h".
#include "wx/wxprec.h"

#ifdef __BORLANDC__
#pragma hdrstop
#endif

#ifndef WX_PRECOMP
#include "wx/wx.h"
#endif

#include "CardWaiterDialog.h"
#include "ConnectDialog.h"
#include "MySession.h"
#include "MyXmlConfig.h"
#include "MyIPC.h"
#include "ResumeDialog.h"
#include "WinShare.h"
#include "opennxApp.h"
#include "osdep.h"
#include "pwcrypt.h"
#include "ProxyPasswordDialog.h"
#include "SimpleXauth.h"

#include <wx/filename.h>
#include <wx/regex.h>
#include <wx/wfstream.h>
#include <wx/txtstrm.h>
#include <wx/socket.h>
#include <wx/config.h>
#include <wx/utils.h>
#include <wx/tokenzr.h>
#include <wx/regex.h>
#include <wx/protocol/http.h>
#include <wx/sckstrm.h>
#include <wx/dir.h>
#include <wx/process.h>
#include <wx/textfile.h>
#ifdef __VISUALC__
# include <fstream.h>
#else
# include <fstream>
#endif
#ifndef __WXMSW__
# include <grp.h>
#endif

#include "trace.h"
ENABLE_TRACE;

static const int CUPS_PORT_OFFSET  = 2000;
static const int SMB_PORT_OFFSET   = 3000;
static const int PROXY_PORT_OFFSET = 4000;
static const int X_PORT_OFFSET     = 6000;
static const int SOUND_PORT_OFFSET = 7000;
static const int KBD_PORT_OFFSET   = 8000;
static const int HTTP_PORT_OFFSET  = 9000;
static const int USBIP_PORT_OFFSET = 40000;

#define X11ARCH_NONE   0
#define X11ARCH_CYGWIN 1
#define X11ARCH_MINGW  2

#ifdef __WXMSW__
wxString cygPath(const wxString &dir, const wxString &file = wxEmptyString)
{
    wxFileName fn(dir);
    wxString ret = wxT("/cygdrive/");
    ret += fn.GetVolume().Lower();
    ret += fn.GetShortPath().AfterFirst(wxT(':'));
    if (!file.IsEmpty())
        ret << wxT("\\") << file;
    ret.Replace(wxT("\\"), wxT("/"));
    return ret;
}

class FontpathTraverser : public wxDirTraverser
{
    public:
        wxString GetFontPath(MySession::tXarch x11arch)
        {
            switch (x11arch) {
                case MySession::XARCH_CYGWIN:
                    return m_sFontPathCygwin;
                    break;
                case MySession::XARCH_XMING:
                    return m_sFontPath;
                    break;
                default:
                    ::wxLogError(_("Invalid X11 server platform"));
                    break;
            }
            return wxEmptyString;
        }

        virtual wxDirTraverseResult OnFile(const wxString& WXUNUSED(filename))
        {
            return wxDIR_CONTINUE;
        }

        virtual wxDirTraverseResult OnDir(const wxString& dirname)
        {
            if (!m_sFontPath.IsEmpty())
                m_sFontPath += wxT(",");
            m_sFontPath += dirname;
            if (!m_sFontPathCygwin.IsEmpty())
                m_sFontPathCygwin += wxT(",");
            m_sFontPathCygwin += cygPath(dirname);
            return wxDIR_IGNORE;
        }

    private:
        wxString m_sFontPath;
        wxString m_sFontPathCygwin;

};

#else
wxString cygPath(const wxString &path)
{
    return wxFileName(path).GetFullPath();
}

wxString cygPath(const wxString &dir, const wxString &file)
{
    return wxFileName(dir, file).GetFullPath();
}
#endif

class RunLog : public wxLogChain
{
    public:
        RunLog(wxLog *logger) :wxLogChain(logger) { }

        void DoLog(wxLogLevel level, const wxChar *szString, time_t t)
        {
            PassMessages(level <= minlevel);
            wxLogChain::DoLog((level > minlevel) ? minlevel : level, szString, t);
        }
    private:
        static const wxLogLevel minlevel = wxLOG_Message;
};

class SessionCleaner : public wxDirTraverser
{
    public:
        SessionCleaner(const wxString &toplevel)
        {
            m_sTopLevel = toplevel;
            wxString s = wxFileName::GetPathSeparator();
            if (!toplevel.EndsWith(s))
                m_sTopLevel += s;
            wxString r = m_sTopLevel + wxT("temp") + s + wxT("([0-9]+)");
            r.Replace(wxT("\\"), wxT("\\\\"));
            m_cRegex.Compile(r, wxRE_ADVANCED);
            wxASSERT(m_cRegex.IsValid());
        }

        ~SessionCleaner()
        {
            int n = m_aFiles.GetCount() - 1;
            while (n >= 0)
                ::wxRemoveFile(m_aFiles[n--]);
            n = m_aDirs.GetCount() - 1;
            while (n >= 0)
                ::wxRmdir(m_aDirs[n--]);
        }

        virtual wxDirTraverseResult OnFile(const wxString &name)
        {
            for (size_t i = 0; i < m_aDirs.GetCount(); i++) {
                if (name.StartsWith(m_aDirs[i])) {
                    ::myLogTrace(MYTRACETAG, wxT("adding file '%s'"), name.c_str());
                    m_aFiles.Add(name);
                    return wxDIR_CONTINUE;
                }
            }
            return wxDIR_CONTINUE;
        }

        virtual wxDirTraverseResult OnDir(const wxString &name)
        {
            if (name.StartsWith(m_sTopLevel + wxT("S-"))) {
                ::myLogTrace(MYTRACETAG, wxT("Session dir: '%s'"), name.c_str());
                wxTextFile tf(name + wxFileName::GetPathSeparator() + wxT("session"));
                if (tf.Exists()) {
                    wxString line;
                    if (tf.Open()) {
                        for (line = tf.GetFirstLine(); !tf.Eof(); line = tf.GetNextLine()) {
                            if (line.Contains(wxT("mode with pid"))) {
                                long pid;
                                line.AfterFirst(wxT('\'')).BeforeLast(wxT('\'')).ToLong(&pid);
                                ::myLogTrace(MYTRACETAG, wxT("Proxy-PID: %d"), pid);
                                if (!wxProcess::Exists(pid)) {
                                    ::myLogTrace(MYTRACETAG, wxT("PID does not exist, adding '%s'"), name.c_str());
                                    m_aDirs.Add(name);
                                    return wxDIR_CONTINUE;
                                }
                            }
                        }
                    }
                }
                ::myLogTrace(MYTRACETAG, wxT("Keeping '%s'"), name.c_str());
            }
            if (name.StartsWith(m_sTopLevel + wxT("D-"))) {
                ::myLogTrace(MYTRACETAG, wxT("Service dir: '%s'"), name.c_str());
                wxTextFile tf(name + wxFileName::GetPathSeparator() + wxT("pid"));
                if (tf.Exists()) {
                    long pid;
                    if (tf.Open()) {
                        tf.GetFirstLine().ToLong(&pid);
                        ::myLogTrace(MYTRACETAG, wxT("Service-PID: %d"), pid);
                        if (!wxProcess::Exists(pid)) {
                            ::myLogTrace(MYTRACETAG, wxT("PID does not exist, adding '%s'"), name.c_str());
                            m_aDirs.Add(name);
                            return wxDIR_CONTINUE;
                        }
                    }
                }
                ::myLogTrace(MYTRACETAG, wxT("Keeping '%s'"), name.c_str());
            }
            if (name.StartsWith(m_sTopLevel + wxT("F-"))) {
                ::myLogTrace(MYTRACETAG, wxT("Failed session dir: '%s'"), name.c_str());
                m_aDirs.Add(name);
                return wxDIR_CONTINUE;
            }
            if (m_cRegex.Matches(name)) {
                ::myLogTrace(MYTRACETAG, wxT("Temp dir: '%s'"), name.c_str());
                long mpid = 0;
                long spid = 0;
                m_cRegex.GetMatch(name, 1).ToLong(&mpid);
                ::myLogTrace(MYTRACETAG, wxT("Main PID: %d"), mpid);
                wxTextFile tf(name + wxFileName::GetPathSeparator() + wxT("sshlog"));
                if (tf.Exists()) {
                    if (tf.Open()) {
                        wxString line = tf.GetFirstLine();
                        line.AfterLast(wxT(':')).Strip(wxString::both).ToLong(&spid);
                        ::myLogTrace(MYTRACETAG, wxT("Ssh-PID: %d"), spid);
                    }
                }
                if (mpid && (!wxProcess::Exists(mpid)) && ((!spid) || (!wxProcess::Exists(spid)))) {
                    ::myLogTrace(MYTRACETAG, wxT("PIDs do not exist, adding '%s'"), name.c_str());
                    m_aDirs.Add(name);
                    return wxDIR_CONTINUE;
                }
                ::myLogTrace(MYTRACETAG, wxT("Keeping '%s'"), name.c_str());
                return wxDIR_IGNORE;
            }
            if (name.StartsWith(m_sTopLevel + wxT("temp")))
                return wxDIR_CONTINUE;
            return wxDIR_IGNORE;
        }

    private:
        wxArrayString m_aDirs;
        wxArrayString m_aFiles;
        wxString m_sTopLevel;
        wxRegEx m_cRegex;
};

DECLARE_EVENT_TYPE(wxEVT_SESSION, -6);
DEFINE_EVENT_TYPE(wxEVT_SESSION);

/**
 * This class is a helper for watching a file until a specific
 * string has appeared in it. When found, it triggers an event.
 */
class SessionWatch : public wxThreadHelper
{
    public:
        SessionWatch(wxEvtHandler *handler, const wxString &logfile, const wxString &search)
            : wxThreadHelper()
        {
            m_pHandler = handler;
            m_sLog = logfile;
            m_sSearch = search;
            if (Create() == wxTHREAD_NO_ERROR)
                GetThread()->Run();
        }

    private:
        virtual wxThread::ExitCode Entry() {
            wxStopWatch sw;
            wxTextFile tf(m_sLog);
            long timeout = 5000;
            bool bFound = false;
            while ((!bFound) && (sw.Time() < timeout)) {
                if (tf.Exists()) {
                    if (timeout == 5000)
                        timeout = sw.Time() + 10000;
                    if (!tf.Open())
                        break;
                    wxString line;
                    for (line = tf.GetFirstLine(); !tf.Eof(); line = tf.GetNextLine()) {
                        if (line.Contains(m_sSearch)) {
                            bFound = true;
                            break;
                        }
                    }
                    tf.Close();
                }
                wxThread::Sleep(500);
            }
            if (m_pHandler) {
                wxCommandEvent upevent(wxEVT_SESSION, wxID_ANY);
                upevent.SetInt(bFound ? 1 : 0);
                m_pHandler->ProcessEvent(upevent);
            }
            return 0;
        }

        wxEvtHandler *m_pHandler;
        wxString m_sLog;
        wxString m_sSearch;
};

/**
 * Our specialization of wxHTTP. Needed, because wxHTTP stops
 * parsing headers when recognizing an error status header (4xx, 5xx ...)
 * but we want it to continue parsing in order to get the Server: header.
 */
class MyHTTP : public wxHTTP {
    public:
        MyHTTP() : wxHTTP() {}
        wxInputStream *GetInputStream(const wxString& path)
        {
            wxSocketInputStream *inp_stream;
            wxString new_path;
            m_perr = wxPROTO_CONNERR;
            if (!m_addr)
                return NULL;

            // We set m_connected back to false so wxSocketBase will know what to do.
#ifdef __WXMAC__
            wxSocketClient::Connect(*m_addr , false );
            wxSocketClient::WaitOnConnect(10);

            if (!wxSocketClient::IsConnected())
                return NULL;
#else
            if (!wxProtocol::Connect(*m_addr))
                return NULL;
#endif

            if (!BuildRequest(path, m_post_buf.empty() ? wxHTTP_GET : wxHTTP_POST))
                return NULL;

            inp_stream = new wxSocketInputStream(*this);

            Notify(false);
            SetFlags(wxSOCKET_BLOCK | wxSOCKET_WAITALL);

            return inp_stream;
        }

    protected:
        bool BuildRequest(const wxString& path, wxHTTP_Req req)
        {
            bool ret = wxHTTP::BuildRequest(path, req);
            ::myLogTrace(MYTRACETAG, wxT("calling ParseHeaders()"));
            ParseHeaders();
            return ret;
        }

};

IMPLEMENT_CLASS(MySession, wxEvtHandler);

BEGIN_EVENT_TABLE(MySession, wxEvtHandler)
    EVT_COMMAND(wxID_ANY, wxEVT_NXSSH, MySession::OnSshEvent)
    EVT_COMMAND(wxID_ANY, wxEVT_SESSION, MySession::OnSessionEvent)
END_EVENT_TABLE();

MySession::MySession(wxString dir, wxString status, wxString stype, wxString host, int port, wxString md5)
    : wxEvtHandler()
    , m_pSshLog(NULL)
    , m_pNxSsh(NULL)
    , m_pCfg(NULL)
    , m_pDlg(NULL)
    , m_pParent(NULL)
    , m_pSessionWatch(NULL)
    , m_bTouched(true)
    , m_iPort(port)
    , m_lPid(0)
    , m_eSessionType(None)
    , m_eSessionStatus(Unknown)
    , m_sHost(host)
    , m_sMd5(md5)
    , m_sDir(dir)
{
    m_bValid = false;
    if (stype == wxT("C"))
        m_eSessionType = Server;
    if (stype == wxT("S"))
        m_eSessionType = Client;

    if (status == wxT("T"))
        m_eSessionStatus = Terminated;
    if (status == wxT("F"))
        m_eSessionStatus = Failed;

    m_rePID.Compile(wxT("([[:digit:]]+)"));
    initversion();

    CheckState();
}

MySession::MySession(const MySession &src)
    : wxEvtHandler()
    , m_pSshLog(NULL)
    , m_pNxSsh(NULL)
    , m_pCfg(NULL)
    , m_pDlg(NULL)
    , m_pParent(NULL)
    , m_pSessionWatch(NULL)
{
    m_sHost = src.m_sHost;
    m_iPort = src.m_iPort;
    m_lPid = src.m_lPid;
    m_sMd5 = src.m_sMd5;
    m_eSessionType = src.m_eSessionType;
    m_eSessionStatus = src.m_eSessionStatus;
    m_sDir = src.m_sDir;
    m_bTouched = src.m_bTouched;
    m_rePID.Compile(wxT("([[:digit:]]+)"));
    m_iReader = src.m_iReader;
    m_lProtocolVersion = src.m_lProtocolVersion;
    m_sProtocolVersion = src.m_sProtocolVersion;
}

MySession::MySession()
    : wxEvtHandler()
    , m_pSshLog(NULL)
    , m_pNxSsh(NULL)
    , m_pCfg(NULL)
    , m_pDlg(NULL)
    , m_pParent(NULL)
    , m_pSessionWatch(NULL)
{
    m_iReader = -1;
    initversion();
}

MySession & MySession::operator =(const MySession &src)
{
    m_pSshLog = NULL;
    m_pCfg = NULL;
    m_pDlg = NULL;
    m_pNxSsh = NULL;
    m_pParent = NULL;
    m_pSessionWatch = NULL;
    m_sHost = src.m_sHost;
    m_iPort = src.m_iPort;
    m_lPid = src.m_lPid;
    m_iReader = src.m_iReader;
    m_sMd5 = src.m_sMd5;
    m_eSessionType = src.m_eSessionType;
    m_eSessionStatus = src.m_eSessionStatus;
    m_sDir = src.m_sDir;
    m_bTouched = src.m_bTouched;
    m_rePID.Compile(wxT("([[:digit:]]+)"));
    m_lProtocolVersion = src.m_lProtocolVersion;
    m_sProtocolVersion = src.m_sProtocolVersion;
    return *this;
}

MySession::~MySession()
{
    ::myLogTrace(MYTRACETAG, wxT("~MySession"));
    if (m_pSshLog) {
        m_pSshLog->Flush();
        delete m_pSshLog;
    }
    m_pSshLog = NULL;
    if (m_pSessionWatch) {
        delete m_pSessionWatch;
        m_pSessionWatch = NULL;
    }
    delete wxLog::SetActiveTarget(NULL);
}

    wxString
MySession::sGetCreationTime()
{
    if (!m_bValid)
        return _("unknown");
    wxDateTime ctime;
    wxFileName fn(m_sDir, wxT("session"));
    fn.GetTimes(NULL, NULL, &ctime);
    return ctime.Format();
}

    wxString
MySession::sGetSessionStatus()
{
    switch (m_eSessionStatus) {
        case Terminated:
            return _("terminated");
            break;
        case Failed:
            return _("failed");
            break;
        case Running:
            return _("running");
            break;
        case Unknown:
            return _("unknown");
            break;
    }
    return _("unknown");
}

    wxString
MySession::sGetSessionType()
{
    switch (m_eSessionType) {
        case Server:
            return _("Server");
            break;
        case Client:
            return _("Client");
            break;
        case None:
            return _("Unknown");
            break;
    }
    return _("Unknown");
}

    bool
MySession::bGetPidFromFile()
{
    m_lPid = 0;
    if (!m_bValid)
        return false;
    wxFileInputStream input(m_sDir + wxFileName::GetPathSeparator() + wxT("session"));
    wxTextInputStream text(input);
    int cnt = 0;

    while ((!input.Eof()) && (cnt < 100)) {
        wxString line = text.ReadLine();
        int idx = line.Find(wxT("pid"));

        if (idx != -1 &&
                line.Find(wxT("NXAGENT")) == -1 &&
                line.Find(wxT("Agent")) == -1)
        {
            line = line.Mid(idx + 4);
            if (m_rePID.Matches(line))
                m_rePID.GetMatch(line, 1).ToLong(&m_lPid);
        }
        cnt++;
    }
    return (m_lPid > 0);
}

    void
MySession::CheckState()
{
    wxString logfilename = m_sDir + wxFileName::GetPathSeparator() + wxT("session");
    m_eSessionStatus = Unknown;

    if (!wxFile::Exists(logfilename))
        return;
    if ((m_eSessionStatus == Terminated) || (m_eSessionStatus == Failed))
        return;

    wxFile fi(logfilename);
    if (!fi.IsOpened())
        return;

    m_bValid = true;
    wxFileInputStream input(fi);

    wxTextInputStream text(input);
    while (!input.Eof()) {
        wxString line = text.ReadLine();
        if (line.StartsWith(wxT("Session: Session terminated"))) {
            m_eSessionStatus = Terminated;
            break;
        }

    }
    if (bGetPidFromFile()) {
        if (wxProcess::Exists(m_lPid))
            m_eSessionStatus = Running;
    }
}

    unsigned short
MySession::getFirstFreePort(unsigned short startPort)
{
#ifdef __WXMAC__
    // wxSocketServer appears to be broken on wxMac,
    // so we use plain unix code, implemented in osdep.c ...
    return macFirstFreePort(startPort);
#else
    unsigned short port = startPort;
    wxIPV4address a;

    a.LocalHost();
    while (port < 65535) {
        a.Service(port);
        wxSocketServer ss(a);
        if (ss.Ok())
            return port;
        port++;
    }
#endif
    return 0;
}

    wxString
MySession::getXauthCookie(int display /* = 0 */, wxString proto)
{

#ifdef __UNIX__
    wxString dpy;
    if (wxGetEnv(wxT("DISPLAY"), &dpy) && (!dpy.IsEmpty())) {
        wxString cmd = wxT("xauth list :");
        cmd << dpy.AfterFirst(wxT(':'));
        wxArrayString clines;
        if (::wxExecute(cmd, clines) == 0) {
            size_t count = clines.GetCount();
            for (size_t i = 0; i < count; i++) {
                wxString line = clines[i];
                if (line.Contains(wxT("MIT-MAGIC-COOKIE-1"))) {
                    wxStringTokenizer t(line, wxT("\t "));
                    int ecount = 0;
                    while (t.HasMoreTokens()) {
                        wxString tok = t.GetNextToken();
                        if (++ecount == 3)
                            return tok;
                    }
                }
            }
        }
    }
#endif
#ifdef __WXMSW__
    // On windows we *create* the cookie instead
    SimpleXauth xa(getXauthPath());
    xa.AddDisplay(display);
    return xa.GetCookie();
#else
    wxUnusedVar(display);
    wxUnusedVar(proto);
#endif
    return wxEmptyString;
}

    wxString
MySession::getXauthPath(tXarch xarch)
{
    wxFileName fn;
#ifdef __UNIX__
    wxString cmd = wxT("xauth info");
    wxArrayString clines;
    if (::wxExecute(cmd, clines) == 0) {
        size_t count = clines.GetCount();
        for (size_t i = 0; i < count; i++) {
            wxString line = clines[i];
            if (line.StartsWith(wxT("Authority file:"))) {
                return line.AfterFirst(wxT(':')).Strip(wxString::both);
            }
        }
    }
    // Fallback: $HOME/.Xauthority
    fn.AssignHomeDir();
    fn.SetName(wxT(".Xauthority"));
    return fn.GetFullPath();
#endif
#ifdef __WXMSW__
    switch (xarch) {
        case XARCH_CYGWIN:
            return cygPath(m_sUserDir, wxT(".Xauthority"));
        case XARCH_XMING:
            fn.Assign(m_sUserDir, wxT(".Xauthority"));
            return fn.GetFullPath();
        default:
            ::wxLogError(_("Invalid X11 server platform"));
            return wxEmptyString;
    }
#endif
    return wxEmptyString;
}

    wxString
MySession::formatOptFilename()
{
#ifdef __WXMSW__
    return cygPath(m_sOptFilename);
#else
    return m_sOptFilename;
#endif
}

    void
MySession::OnSessionEvent(wxCommandEvent &event)
{
    if (event.GetInt())
        m_bSessionEstablished = true;
    else
        m_bGotError = true;
}

    void
MySession::OnSshEvent(wxCommandEvent &event)
{
    MyIPC::tSessionEvents e = wx_static_cast(MyIPC::tSessionEvents, event.GetInt());
    wxString msg(event.GetString());
    wxString scmd;

    switch (e) {
        case MyIPC::ActionNone:
        case MyIPC::ActionStdout:
        case MyIPC::ActionStderr:
            break;
        case MyIPC::ActionStatus:
            m_pDlg->SetStatusText(msg);
            break;
        case MyIPC::ActionLog:
            m_pDlg->SetProgress(m_iProgress++);
            if (m_bCollectSessions)
                m_aParseBuffer.Add(msg);
            if (m_bCollectConfig) {
                m_sConfigBuffer << msg << wxT("\n");
                if (m_sConfigBuffer.Length() >= m_nSessionPushLength) {
                    ::wxLogInfo(wxT("session override finished"));
                    m_bCollectConfig = false;
                    m_pCfg->LoadFromString(m_sConfigBuffer, true);
                    if (m_pCfg->IsWritable())
                        m_pCfg->SaveToFile();
                }
            }
            if (m_pSshLog) {
                wxLog *oldLog = wxLog::SetActiveTarget(m_pSshLog);
                ::wxLogMessage(msg);
                wxLog::SetActiveTarget(oldLog);
            }
            break;
        case MyIPC::ActionWarning:
            ::wxLogWarning(msg);
            break;
        case MyIPC::ActionError:
            ::wxLogError(msg);
            m_bGotError = true;
            break;
        case MyIPC::ActionPromptYesNo:
            {
                wxMessageDialog d(m_pParent, msg,
                        _("Warning - OpenNX"), wxYES_NO|wxICON_EXCLAMATION);
                if (d.ShowModal() == wxID_YES)
                    printSsh(wxT("yes"));
                else {
                    printSsh(wxT("no"));
                    m_bGotError = true;
                }
            }
            break;
        case MyIPC::ActionKeyChangedYesNo:
            {
                msg << _("\nDo you want to delete the key and retry ?");
                wxMessageDialog d(m_pParent, msg,
                        _("Warning - OpenNX"), wxYES_NO|wxICON_EXCLAMATION);
                if (d.ShowModal() == wxID_YES)
                    m_bRemoveKey = true;
                m_bGotError = true;
            }
            break;
        case MyIPC::ActionOffendingKey:
            m_sOffendingKey = msg;
            break;
        case MyIPC::ActionSendUsername:
            {
                // At this point key-based auth is finished and
                // thus the key-file isn't needed anymore
                wxLogNull logdummy;
                wxFileName fn;
                fn.Assign(m_sTempDir, wxT("keylog"));
                ::wxRemoveFile(fn.GetFullPath());
            }
            m_pDlg->SetStatusText(_("Sending username"));
            printSsh(m_pCfg->sGetSessionUser());
            break;
        case MyIPC::ActionSendPassword:
            m_pDlg->SetStatusText(_("Authenticating"));
            if (m_pCfg->bGetRememberPassword())
                printSsh(m_pCfg->sGetSessionPassword(), false);
            else
                printSsh(m_sClearPassword, false);
            break;
        case MyIPC::ActionWelcome:
            m_pDlg->SetStatusText(_("Authentication successful"));
            m_eConnectState = STATE_LIST_SESSIONS;
            break;
        case MyIPC::ActionSessionPushLength:
            // Session file length: 213
            msg.Mid(21).ToULong(&m_nSessionPushLength);
            break;
        case MyIPC::ActionSessionPushStart:
            m_sConfigBuffer.Empty();
            m_bCollectConfig = true;
            ::wxLogInfo(wxT("receiving %d bytes of session override"),
                    m_nSessionPushLength);
            break;
        case MyIPC::ActionNextCommand:
            m_iProgress += 4;
            m_pDlg->SetProgress(m_iProgress);
            switch (m_eConnectState) {
                case STATE_ABORT:
                    break;
                case STATE_HELLO:
                    m_pDlg->SetStatusText(_("Authenticating"));
                    printSsh(wxT("hello NXCLIENT - Version ") + m_sProtocolVersion);
                    m_eConnectState = STATE_SHELLMODE;
                    break;
                case STATE_SHELLMODE:
                    printSsh(wxT("SET SHELL_MODE SHELL"));
                    m_eConnectState = STATE_AUTHMODE;
                    break;
                case STATE_AUTHMODE:
                    printSsh(wxT("SET AUTH_MODE PASSWORD"));
                    m_eConnectState = STATE_LOGIN;
                    break;
                case STATE_LOGIN:
                    printSsh(wxT("login"));
                    break;
                case STATE_LIST_SESSIONS:
                    m_pDlg->SetStatusText(_("Query server-side sessions"));
                    scmd = wxT("listsession") + m_pCfg->sGetListParams(m_lProtocolVersion);
                    printSsh(scmd);
                    m_eConnectState = STATE_PARSE_SESSIONS;
                    break;
                case STATE_PARSE_SESSIONS:
                    if (m_bIsShadow) {
                        // Server has sent list of attachable sessions
                        m_bCollectSessions = false;
                        ::wxLogInfo(wxT("received end of attachable session list"));
                        parseSessions(false);
                        break;
                    }
                    printSsh(wxEmptyString);
                    break;
                case STATE_START_SESSION:
                    scmd = wxT("startsession");
                    scmd << m_pCfg->sGetSessionParams(m_lProtocolVersion, true, m_sClearPassword);
                    printSsh(scmd);
                    m_eConnectState = STATE_FINISH;
                    break;
                case STATE_ATTACH_SESSION:
                    scmd = wxT("attachsession");
                    scmd << m_pCfg->sGetSessionParams(m_lProtocolVersion, true, m_sClearPassword)
                        << wxT(" --display=\"") << m_sResumePort
                        << wxT("\" --id=\"") << m_sResumeId << wxT("\"");
                    printSsh(scmd);
                    m_eConnectState = STATE_FINISH;
                    break;
                case STATE_RESUME_SESSION:
                    scmd = wxT("restoresession");
                    scmd << m_pCfg->sGetSessionParams(m_lProtocolVersion, false, m_sClearPassword)
                        << wxT(" --session=\"") << m_sResumeName
                        << wxT("\" --type=\"") << m_sResumeType
                        << wxT("\" --id=\"") << m_sResumeId << wxT("\"");
                    printSsh(scmd);
                    m_eConnectState = STATE_FINISH;
                    break;
                case STATE_KILL_SESSION:
                    scmd = wxT("terminate --sessionid=\"");
                    scmd << m_sKillId << wxT("\"");
                    printSsh(scmd);
                    m_eConnectState = STATE_LIST_SESSIONS;
                    break;
                case STATE_FINISH:
                    if (m_bGotError) {
                        m_eConnectState = STATE_ABORT;
                        printSsh(wxT("bye"));
                    }
                    break;
            }
            break;
        case MyIPC::ActionPinDialog:
            printSsh(wxGetPasswordFromUser(
                        _("Enter PIN for Smart Card access."), _("Smart Card PIN"),
                        wxEmptyString, m_pParent), false);
            break;
        case MyIPC::ActionPassphraseDialog:
            scmd = ::wxGetPasswordFromUser(::wxGetTranslation(msg),
                    _("Enter passphrase"), wxEmptyString, m_pParent);
            if (scmd.IsEmpty()) {
                msg = _("Empty passphrase");
                m_bGotError = true;
            }
            printSsh(scmd, false);
            break;
        case MyIPC::ActionSetSessionID:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionID = msg;
            break;
        case MyIPC::ActionSetProxyCookie:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sProxyCookie = msg;
            break;
        case MyIPC::ActionSetProxyIP:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sProxyIP = msg;
            break;
        case MyIPC::ActionSetSessionType:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionType = msg;
            break;
        case MyIPC::ActionSetSessionCache:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionCache = msg;
            break;
        case MyIPC::ActionSetSessionDisplay:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSessionDisplay = msg;
            break;
        case MyIPC::ActionSetAgentCookie:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sAgentCookie = msg;
            break;
        case MyIPC::ActionSetSslTunneling:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_bSslTunneling = (msg == wxT("1"));
            break;
        case MyIPC::ActionSetSubscription:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSubscription = msg;
            break;
        case MyIPC::ActionSetSmbPort:
            m_pDlg->SetStatusText(_("Negotiating session parameter"));
            m_sSmbPort = msg;
            break;
        case MyIPC::ActionExit:
            if (m_eConnectState == STATE_ABORT)
                m_bAbort = true;
            else {
                if (m_eConnectState == STATE_FINISH) {
                    m_pDlg->SetStatusText(_("Starting session"));
                    msg = wxT("NX> 299 Switch connection to: ");
                    if ((m_lProtocolVersion > 0x00020000) && m_bSslTunneling) {
                        msg << wxT("NX mode: encrypted options: nx,options=")
                            << formatOptFilename() << wxT(":") << m_sSessionDisplay;
                    } else {
                        msg << m_sProxyIP << wxT(":") << m_sProxyPort
                            << wxT(" cookie: ") << m_sProxyCookie;
                        m_bSessionRunning = true;
                        wxString slog = m_sSessionDir;
                        slog << wxFileName::GetPathSeparator() << wxT("session");
                        m_pSessionWatch = new SessionWatch(this, slog,
                                wxT("Session: Session started at"));
                    }
                    printSsh(msg);
                } else {
                    m_bSessionRunning = true;
                }
            }
            break;
        case MyIPC::ActionTerminated:
            if ((m_eConnectState <= STATE_PARSE_SESSIONS) && (!m_bGotError) && (!m_bRemoveKey) && (m_sOffendingKey.IsEmpty())) {
                msg = _("Unexpected termination of nxssh");
                ::wxLogError(msg);
                m_bGotError = true;
            }
            break;
        case MyIPC::ActionStartProxy:
            if (m_eConnectState == STATE_FINISH) {
                m_pDlg->SetStatusText(_("Starting session"));
                startSharing();
                startProxy();
            }
            break;
        case MyIPC::ActionSessionRunning:
            m_bSessionRunning = true;
            {
                wxString slog = m_sSessionDir;
                slog << wxFileName::GetPathSeparator() << wxT("session");
                m_pSessionWatch = new SessionWatch(this, slog,
                        wxT("Session: Session started at"));
            }
            break;
        case MyIPC::ActionSessionListStart:
            // Server starts sending session list
            ::wxLogInfo(wxT("receiving session list .."));
            m_aParseBuffer.Empty();
            m_bCollectSessions = true;
            break;
        case MyIPC::ActionSessionListEnd:
            // Server has sent list of running & suspended sessions
            m_bCollectSessions = false;
            ::wxLogInfo(wxT("received end of session list"));
            parseSessions(event.GetExtraLong() == 148);
            break;
    }
}

    void
MySession::initversion()
{
    m_lProtocolVersion = 0;
    if (!::wxGetEnv(wxT("NX_PROTOCOL_VERSION"), &m_sProtocolVersion))
        m_sProtocolVersion = wxT(NX_PROTOCOL_VERSION);
    wxStringTokenizer t(m_sProtocolVersion, wxT("."));
    while (t.HasMoreTokens()) {
        long n;
        t.GetNextToken().ToLong(&n);
        m_lProtocolVersion = (m_lProtocolVersion << 8) + n;
    }
    ::myLogTrace(MYTRACETAG, wxT("protocol version: %08x"), m_lProtocolVersion);
}

    void
MySession::printSsh(const wxString &s, bool doLog /* = true */)
{
    if (m_pNxSsh) {
        ::myLogTrace(MYTRACETAG, wxT("sending '%s'"), (doLog ? s.c_str() : wxT("********")));
        m_pNxSsh->Print(s, doLog);
    }
}

    void
MySession::parseSessions(bool moreAllowed)
{
    size_t n = m_aParseBuffer.GetCount();
    ::myLogTrace(MYTRACETAG, wxT("parseSessions: Got %d lines to parse"), n);
    wxRegEx re(
            wxT("^(\\d+)\\s+([\\w-]+)\\s+([0-9A-F]{32})\\s+([A-Z-]{8})\\s+(\\d+)\\s+(\\d+x\\d+)\\s+(\\w+)\\s+(\\w+)\\s*"),
            wxRE_ADVANCED);
    wxASSERT(re.IsValid());
    wxRegEx re2(
            wxT("^(\\d+)\\s+([\\w-]+)\\s+([0-9A-F]{32})\\s+([A-Z-]{8})\\s+(N/A)\\s+(N/A)\\s+(\\w+)\\s+(\\w+\\s\\w+)\\s*"),
            wxRE_ADVANCED);
    wxASSERT(re2.IsValid());
    ResumeDialog d(NULL);
    bool bFound = false;
    int iSessionCount = 0;
    wxString sName;
    if (m_pCfg->eGetSessionType() == MyXmlConfig::STYPE_SHADOW)
        d.SetAttachMode(true);
    else
        d.SetPreferredSession(m_pCfg->sGetName());
    for (size_t i = 0; i < n; i++) {
        wxString line = m_aParseBuffer[i];
        if (re.Matches(line) && (re.GetMatchCount() == 9)) {
            wxString sPort(re.GetMatch(line, 1));
            wxString sType(re.GetMatch(line, 2));
            wxString sId(re.GetMatch(line, 3));
            wxString sOpts(re.GetMatch(line, 4));
            wxString sColors(re.GetMatch(line, 5));
            wxString sSize(re.GetMatch(line, 6));
            wxString sState(re.GetMatch(line, 7));
            sName = re.GetMatch(line, 8);
            d.AddSession(sName, sState, sType, sSize, sColors, sPort, sOpts, sId);
            bFound = true;
            iSessionCount++;
            ::myLogTrace(MYTRACETAG, wxT("parseSessions: re match"));
            continue;
        }
        if (m_bIsShadow) {
            ::myLogTrace(MYTRACETAG, wxT("parseSessions: re2 probe"));
            if (re2.Matches(line) /* && (re2.GetMatchCount() == 9)*/) {
                ::myLogTrace(MYTRACETAG, wxT("parseSessions: re2 match: %d"), re2.GetMatchCount());
                wxString sPort(re2.GetMatch(line, 1));
                wxString sType(re2.GetMatch(line, 2));
                wxString sId(re2.GetMatch(line, 3));
                wxString sOpts(re2.GetMatch(line, 4));
                wxString sColors(re2.GetMatch(line, 5));
                wxString sSize(re2.GetMatch(line, 6));
                wxString sState(re2.GetMatch(line, 7));
                sName = re2.GetMatch(line, 8);
                d.AddSession(sName, sState, sType, sSize, sColors, sPort, sOpts, sId);
                bFound = true;
                iSessionCount++;
                continue;
            }
        }
        ::myLogTrace(MYTRACETAG, wxT("parseSessions: NO match on '%s'"), line.c_str());
    }
    if (bFound) {
        d.EnableNew(moreAllowed);
        if (m_bIsShadow || (iSessionCount > 1) || (!sName.IsSameAs(m_pCfg->sGetName()))) {
            switch (d.ShowModal()) {
                case wxID_OK:
                    switch (d.GetMode()) {
                        case ResumeDialog::Terminate:
                            ::wxLogInfo(wxT("TERMINATE"));
                            m_sKillId = d.GetSelectedId();
                            m_eConnectState = STATE_KILL_SESSION;
                            break;
                        case ResumeDialog::Resume:
                            ::wxLogInfo(wxT("RESUME"));
                            m_sResumeName = d.GetSelectedName();
                            m_sResumeType = d.GetSelectedType();
                            m_sResumeId = d.GetSelectedId();
                            m_sResumePort = d.GetSelectedPort();
                            m_eConnectState = m_bIsShadow ? STATE_ATTACH_SESSION : STATE_RESUME_SESSION;
                            if (m_bIsShadow)
                                printSsh(wxEmptyString);
                            break;
                        case ResumeDialog::Takeover:
                            ::wxLogInfo(wxT("TAKEOVER"));
                            m_sResumeName = d.GetSelectedName();
                            m_sResumeType = d.GetSelectedType();
                            m_sResumeId = d.GetSelectedId();
                            m_eConnectState = STATE_RESUME_SESSION;
                            break;
                        case ResumeDialog::New:
                            m_eConnectState = STATE_START_SESSION;
                            break;
                    }
                    break;
                case wxID_CANCEL:
                    printSsh(wxT("bye"));
                    if (m_bIsShadow)
                        m_eConnectState = STATE_ABORT;
                    break;
            }
        } else {
            ::wxLogInfo(wxT("RESUME"));
            m_sResumeName = d.GetSelectedName();
            m_sResumeType = d.GetSelectedType();
            m_sResumeId = d.GetSelectedId();
            m_eConnectState = STATE_RESUME_SESSION;
        }
    } else {
        if (m_bIsShadow) {
            printSsh(wxT("bye"));
            wxMessageDialog d(m_pParent,
                    _("There are no sessions which can be attached to."),
                    _("Error - OpenNX"), wxOK);
            d.ShowModal();
            m_bGotError = true;
        } else {
            if (moreAllowed)
                m_eConnectState = STATE_START_SESSION;
            else {
                printSsh(wxT("bye"));
                wxMessageDialog d(m_pParent,
                        _("You have reached your session limit. No more sessions allowed"),
                        _("Error - OpenNX"), wxOK);
                d.ShowModal();
                m_bGotError = true;
            }
        }
    }
}

    void
MySession::startSharing()
{
    ArrayOfShareGroups sg = m_pCfg->aGetShareGroups();
    wxArrayString used = m_pCfg->aGetUsedShareGroups();
    CupsClient cc;
    SmbClient sc;
    ArrayOfShares sa;
    if (m_pCfg->bGetUseCups() && cc.IsAvailable())
        sa = cc.GetShares();
    if (m_pCfg->bGetEnableSmbSharing() && sc.IsAvailable())
        WX_APPEND_ARRAY(sa, sc.GetShares());

    long cupsport;
    long smbport;
    m_sSessionDisplay.ToLong(&cupsport);
    m_sSessionDisplay.ToLong(&smbport);
    cupsport += CUPS_PORT_OFFSET;
    smbport += SMB_PORT_OFFSET;
    wxString cupspw = decodeString(wxConfigBase::Get()->Read(wxT("Config/CupsPasswd"), wxEmptyString));
    for (size_t i = 0; i < sg.GetCount(); i++) {
        if (used.Index(sg[i].m_sGroupName) == wxNOT_FOUND)
            continue;
        bool bAvailable = false;
        wxString sn = sg[i].m_sShareName;
        ::myLogTrace(MYTRACETAG, wxT("startSharing: considering share '%s'"), sn.c_str());
        for (size_t j = 0; j < sa.GetCount(); j++) {
            if ((sa[j].sharetype == sg[i].m_eType) && (sa[j].name == sn)) {
                bAvailable = true;
                break;
            }
        }
        ::myLogTrace(MYTRACETAG, wxT("'%s' is %savailable"), sn.c_str(),
                (bAvailable ? wxEmptyString : wxT("not ")));
        if (!bAvailable)
            continue;
        wxString shcmd;
        switch (sg[i].m_eType) {
            case SharedResource::SHARE_UNKNOWN:
                break;
            case SharedResource::SHARE_SMB_DISK:
                shcmd = wxT("addmount");
                shcmd
                    << wxT(" --port=\"") << smbport << wxT("\"") 
                    << wxT(" --username=\"")
                    << MyXmlConfig::UrlEsc(sg[i].m_sUsername) << wxT("\"") 
                    << wxT(" --password=\"")
                    << MyXmlConfig::UrlEsc(sg[i].m_sPassword) << wxT("\"") 
                    << wxT(" --share=\"") << MyXmlConfig::UrlEsc(sn) << wxT("\"") 
                    << wxT(" --computername=\"")
                    << MyXmlConfig::UrlEsc(::wxGetFullHostName()) << wxT("\"") 
                    << wxT(" --session_id=\"") << m_sSessionID.Right(32) << wxT("\"") 
                    << wxT(" --dir=\"")
                    << MyXmlConfig::UrlEsc(sg[i].m_sAlias) << wxT("\"");
                printSsh(shcmd);
                break;
            case SharedResource::SHARE_SMB_PRINTER:
                shcmd = wxT("addprinter");
                shcmd << wxT(" --type=\"smb\"")
                    << wxT(" --username=\"")
                    << MyXmlConfig::UrlEsc(sg[i].m_sUsername) << wxT("\"") 
                    << wxT(" --password=\"")
                    << MyXmlConfig::UrlEsc(sg[i].m_sPassword) << wxT("\"") 
                    << wxT(" --port=\"") << (int)smbport << wxT("\"") 
                    << wxT(" --share=\"") << MyXmlConfig::UrlEsc(sn) << wxT("\"") 
                    << wxT(" --computername=\"")
                    << MyXmlConfig::UrlEsc(::wxGetHostName()) << wxT("\"") 
                    << wxT(" --session_id=\"") << m_sSessionID.Right(32) << wxT("\"") 
                    << wxT(" --model=\"") << sg[i].m_sDriver << wxT("\"");
                if (sg[i].m_bDefault)
                    shcmd << wxT(" --defaultprinter=\"1\"");
                if (sg[i].m_bPublic)
                    shcmd << wxT(" --public=\"1\"");
                printSsh(shcmd);
                break;
            case SharedResource::SHARE_CUPS_PRINTER:
                shcmd = wxT("addprinter");
                shcmd << wxT(" --type=\"ipp\"")
                    << wxT(" --username=\"") << MyXmlConfig::UrlEsc(sg[i].m_sUsername) << wxT("\"") 
                    << wxT(" --port=\"") << cupsport << wxT("\"") 
                    << wxT(" --session_id=\"") << m_sSessionID.Right(32) << wxT("\"") 
                    << wxT(" --printer=\"") << MyXmlConfig::UrlEsc(sn) << wxT("\"") 
                    << wxT(" --password=\"") << cupspw << wxT("\"")
                    << wxT(" --model=\"cups%20printer\"");
                if (sg[i].m_bDefault)
                    shcmd << wxT(" --defaultprinter=\"1\"");
                if (sg[i].m_bPublic)
                    shcmd << wxT(" --public=\"1\"");
                printSsh(shcmd);
                break;
        }
    }
}

    void
MySession::checkXarch()
{
    m_eXarch = XARCH_NONE;
#ifdef __WXMSW__
    wxFileName fn(m_sSysDir, wxT("nxwin.exe"));
    fn.AppendDir(wxT("bin"));
    if (fn.IsFileExecutable()) {
        m_eXarch = XARCH_CYGWIN;
        return;
    }
    fn.SetFullName(wxT("XMing.exe"));
    if (fn.IsFileExecutable())
        m_eXarch = XARCH_XMING;
#endif
}

#ifdef __WXMSW__
    wxString
MySession::getXfontPath(tXarch Xarch)
{
    wxLogNull l;
    wxFileName fn(m_sSysDir, wxEmptyString);
    fn.AppendDir(wxT("share"));
    fn.AppendDir(wxT("fonts"));
    wxDir d(fn.GetShortPath());
    FontpathTraverser t;
    d.Traverse(t);
    wxString ret = t.GetFontPath(Xarch);
    if (!ret.IsEmpty())
        ret.Prepend(wxT(" -fp "));
    return ret;
}

    void
MySession::unhideNXWin()
{
    if (XARCH_CYGWIN == m_eXarch) {
        // Required only for NXWin - Xming shows up by itself
        HWND h = ::GetTopWindow(0);
        while (h) {
            wxString wclass;
            int r = GetClassName(h, wclass.GetWriteBuf(40), 38);
            wclass.UngetWriteBuf();
            if ((r > 0) && (wxNOT_FOUND != wclass.Find(wxT("cygwin/xfree86")))) {
                DWORD pid;
                GetWindowThreadProcessId(h, &pid);
                if ((int)pid == m_iXserverPID) {
                    // Trigger unhiding of fullscreen NXWin
                    SendMessage(h, WM_USER + 1, 0, 0);
                    break;
                }
            }
            h = ::GetNextWindow(h , GW_HWNDNEXT);
        }
    }
}

    void
MySession::terminateXserver()
{
    // Xming non-fullscreen: keep running
    if ((XARCH_CYGWIN == m_eXarch) ||
            (MyXmlConfig::DPTYPE_FULLSCREEN == m_pCfg->eGetDisplayType())) {
        if ((0 != m_iXserverPID) && wxProcess::Exists(m_iXserverPID))
            wxProcess::Kill(m_iXserverPID, wxSIGKILL, wxKILL_NOCHILDREN);
    }
}

    bool
MySession::startXserver()
{
    int display = getFirstFreePort(X_PORT_OFFSET);
    ::myLogTrace(MYTRACETAG, wxT("startXServer first free port is %d"), display);
    if (0 == display)
        return false;
    display -= X_PORT_OFFSET;
    wxString dpyStr = wxT(":");
    dpyStr << display;

    wxString wxWinCmd;
    wxFileName fn(m_sSysDir, wxT("nxwin.exe"));
    fn.AppendDir(wxT("bin"));

    switch (m_eXarch) {
        case XARCH_CYGWIN:
            m_sXauthCookie = getXauthCookie(display, wxT("/unix"));
            if (m_sXauthCookie.IsEmpty()) {
                ::wxLogError(_("Could not create X11 authentication cookie"));
                return false;
            }
            wxWinCmd = fn.GetShortPath();
            wxWinCmd << wxT(" -nowinkill");
            wxWinCmd << wxT(" -clipboard");
            wxWinCmd << wxT(" -noloadxkb");
            wxWinCmd << wxT(" -agent");
            wxWinCmd << wxT(" -hide");
            wxWinCmd << wxT(" -noreset");
            wxWinCmd << wxT(" -auth ") << getXauthPath(XARCH_CYGWIN);
            wxWinCmd << wxT(" -nolisten tcp");
            wxWinCmd << getXfontPath(m_eXarch);
            wxWinCmd << m_pCfg->sGetXserverParams(true);
            {
                wxString title = m_pCfg->sGetUsername();
                if (m_pCfg->bGetGuestMode()) {
                    title = m_pCfg->sGetGuestUser();
                    if (title.IsEmpty())
                        title << wxT("guest");
                }
                wxWinCmd << wxT(" -name ") << title << wxT("@") << m_pCfg->sGetServerHost();
            }
            wxWinCmd << wxT(" ") << dpyStr;
            wxWinCmd << wxT(" -nokeyhook");
            break;
        case XARCH_XMING:
            if (m_pCfg->eGetDisplayType() != MyXmlConfig::DPTYPE_FULLSCREEN) {
                // If not fullscreen, we use a single instance of Xming, running in
                // multiwindow mode. In this case, we check for a running Xming, by
                // using a win32 named mutex.
                int xdpy = getXmingPort(display + X_PORT_OFFSET);
                if (0 != xdpy) {
                    dpyStr = wxT("127.0.0.1:");
                    dpyStr << xdpy - X_PORT_OFFSET;
                    ::wxSetEnv(wxT("DISPLAY"), dpyStr);
                    ::wxLogInfo(wxT("env: DISPLAY='%s'"), dpyStr.c_str());
                    // Xauth cookie and X<dpy>.hosts are still existing
                    // from initial startup
                    return true;
                }
            }
            fn.SetFullName(wxT("Xming.exe"));
            m_sXauthCookie = getXauthCookie(display, wxEmptyString);
            if (m_sXauthCookie.IsEmpty()) {
                ::wxLogError(_("Could not create X11 authentication cookie"));
                return false;
            }
            wxWinCmd = fn.GetShortPath();
            wxWinCmd << wxT(" ") << dpyStr;
            fn.Assign(m_sUserDir, wxString::Format(wxT("X%d.log"), display));
            wxWinCmd << wxT(" -logfile \"") << fn.GetFullPath() << wxT("\"");
            wxWinCmd << wxT(" -br");
            wxWinCmd << wxT(" -nowinkill");
            wxWinCmd << wxT(" -clipboard");
            wxWinCmd << wxT(" -notrayicon");
            wxWinCmd << getXfontPath(m_eXarch);
            wxWinCmd << wxT(" -silent-dup-error");
            if (::checkMultiMonitors() > 1)
                wxWinCmd << wxT(" -multimonitors");
            wxWinCmd << m_pCfg->sGetXserverParams(false);
            fn.Assign(m_sSysDir, wxEmptyString);
            fn.AppendDir(wxT("share"));
            fn.AppendDir(wxT("Xming"));
            fn.MakeAbsolute();
            ::wxSetEnv(wxT("XMING_BASEDIR"), fn.GetPath());
            ::wxLogInfo(wxT("env: XMING_BASEDIR='%s'"), fn.GetPath().c_str());
            dpyStr.Prepend(wxT("127.0.0.1"));
            break;
        default:
            ::wxLogError(_("No X server found."));
            return false;
            break;
    }

    ::wxLogInfo(wxT("Executing %s"), wxWinCmd.c_str());
    int r = CreateDetachedProcess((const char *)wxWinCmd.mb_str());
    if (r != 0) {
        ::wxLogError(_("Could not execute %s: %s\n"), wxWinCmd.c_str(), wxSysErrorMsg(r));
        return false;
    }
    m_iXserverPID = GetDetachedPID();
    AllowSetForegroundWindow(m_iXserverPID);
    ::wxSetEnv(wxT("DISPLAY"), dpyStr);
    ::wxLogInfo(wxT("env: DISPLAY='%s'"), dpyStr.c_str());
    return true;
}
#endif

    void
MySession::startProxy()
{
    ::myLogTrace(MYTRACETAG, wxT("MySession::startProxy() called"));
    long cupsport = wxConfigBase::Get()->Read(wxT("Config/CupsPort"), -1);
    wxString popts;
    if (m_lProtocolVersion >= 0x00030000)
        popts << wxT("nx/");
    popts << wxT("nx,cookie=") << m_sProxyCookie;
    if (m_lProtocolVersion < 0x00030000)
        popts << wxT(",root=") << cygPath(m_sUserDir);
    popts << m_pCfg->sGetProxyParams(m_lProtocolVersion);
    if (!m_sSubscription.IsEmpty())
        popts << wxT(",product=") << m_sSubscription;
    if (!m_sSmbPort.IsEmpty())
        popts << wxT(",samba=") << m_sSmbPort;
    if ((getActiveCupsPrinters().GetCount() > 0) && (isCupsRunning()))
        popts << wxT(",cups=") << cupsport;
#ifdef SUPPORT_USBIP
    if (m_pCfg->bGetEnableUSBIP())
        popts << wxT(",http=") << wxConfigBase::Get()->Read(wxT("Config/UsbipPort"), 3420);
#endif
    if (m_bEsdRunning && (0 < m_lEsdPort))
        popts << wxT(",media=") << m_lEsdPort;
    popts
        << wxT(",encryption=") << (m_bSslTunneling ? 1 : 0)
        << wxT(",session=session")
#ifdef __WXMAC__
        << wxT(",client=macosx")
#else
# ifdef __UNIX__
        << wxT(",client=linux")
# else
        //<< wxT(",client=winnt")
        << wxT(",client=linux")
# endif
#endif
        << wxT(",id=") << m_sSessionID;
    if (m_bSslTunneling) {
        if (m_lProtocolVersion <= 0x00020000) {
            m_sProxyIP = wxT("127.0.0.1");
            m_sProxyPort = wxString::Format(wxT("%d"), getFirstFreePort(PROXY_PORT_OFFSET));
            popts << wxT(",listen=") << m_sProxyPort;
        }
    } else
        popts << wxT(",connect=") << m_sHost;

    // Undocumented feature of the original:
    // If a file ~/.nx/options exists, it's content is
    // appended to the regular options.
    wxFileName mergeOpts(m_sUserDir, wxT("options"));
    if (mergeOpts.FileExists()) {
        wxLogNull dummy;
        wxTextFile f(mergeOpts.GetFullPath());
        if (f.Open()) {
            wxString mopts = f.GetFirstLine().Strip(wxString::both);
            if (!mopts.IsEmpty()) {
                if ((!popts.IsEmpty()) && (!mopts.StartsWith(wxT(","))))
                    popts << wxT(",");
                popts << mopts;
            }
            f.Close();
        }
    }

    popts << wxT(":") << m_sSessionDisplay;
    m_sSessionDir = m_sUserDir;
    m_sSessionDir << wxFileName::GetPathSeparator()
        << wxT("S-") << m_sSessionID;
    {
        if (!wxFileName::Mkdir(m_sSessionDir, 0700, wxPATH_MKDIR_FULL)) {
            ::wxLogSysError(_("Could not create session directory\n%s\n"),
                    m_sSessionDir.c_str());
            m_bGotError = true;
        }
        m_sOptFilename = m_sSessionDir;
        m_sOptFilename << wxFileName::GetPathSeparator() << wxT("options");
        wxFile f;
        if (f.Open(m_sOptFilename, wxFile::write, wxS_IRUSR|wxS_IWUSR)) {
            f.Write(popts + wxT("\n"));
            f.Close();
            ::wxLogInfo(wxT("Session options='%s'\n"), popts.c_str());
            wxString pcmd;
            wxConfigBase::Get()->Read(wxT("Config/SystemNxDir"), &pcmd);
            pcmd << wxFileName::GetPathSeparator() << wxT("bin")
                << wxFileName::GetPathSeparator() << wxT("nxproxy -S nx,options=")
                << cygPath(m_sOptFilename) << wxT(":") << m_sSessionDisplay;
            printSsh(wxT("bye"));
            if ((m_lProtocolVersion <= 0x00020000) || (!m_bSslTunneling)) {
                ::wxLogInfo(wxT("Executing %s"), pcmd.c_str());
#ifdef __WXMSW__
                CreateDetachedProcess((const char *)pcmd.mb_str());
                if (m_iXserverPID)
                    AllowSetForegroundWindow(m_iXserverPID);
#else
                ::wxExecute(pcmd, wxEXEC_ASYNC);
#endif
            }
        } else {
            ::wxLogSysError(_("Could not write session options\n%s\n"),
                    m_sOptFilename.c_str());
            m_bGotError = true;
        }
    }
}

    ArrayOfShareGroups
MySession::getActiveCupsPrinters()
{
    ArrayOfShareGroups ret;
    if (!m_pCfg->bGetUseCups())
        return ret;
    CupsClient cc;
    if (cc.IsAvailable()) {
        ArrayOfShares sa = cc.GetShares();
        ret = m_pCfg->aGetShareGroups();
        wxArrayString used = m_pCfg->aGetUsedShareGroups();
        for (size_t i = ret.GetCount(); i > 0; i--) {
            size_t idx = i - 1;
            if (used.Index(ret[idx].m_sGroupName) == wxNOT_FOUND) {
                ::myLogTrace(MYTRACETAG, wxT("removing[%d] '%s'"),
                        idx, ret[idx].m_sShareName.c_str());
                ret.RemoveAt(idx);
                continue;
            }
            if (ret[idx].m_eType != SharedResource::SHARE_CUPS_PRINTER) {
                ::myLogTrace(MYTRACETAG, wxT("removing[%d] '%s'"),
                        idx, ret[idx].m_sShareName.c_str());
                ret.RemoveAt(idx);
                continue;
            }
            bool bAvailable = false;
            ::myLogTrace(MYTRACETAG, wxT("Considering CUPS printer '%s' %d"),
                    ret[idx].m_sShareName.c_str(), idx);
            for (size_t j = 0; j < sa.GetCount(); j++) {
                if (sa[j].name == ret[idx].m_sShareName) {
                    bAvailable = true;
                    break;
                }
            }
            ::myLogTrace(MYTRACETAG, wxT("'%s' is %savailable"),
                    ret[idx].m_sShareName.c_str(), (bAvailable ? wxEmptyString : wxT("not ")));
            if (!bAvailable) {
                ::myLogTrace(MYTRACETAG, wxT("removing[%d] '%s'"),
                        idx, ret[idx].m_sShareName.c_str());
                ret.RemoveAt(idx);
            }
        }
    }
    ::myLogTrace(MYTRACETAG, wxT("# of active printers: %d"), ret.GetCount());
    return ret;
}

    bool
MySession::isCupsRunning()
{
    if (m_bCupsRunning)
        return true;
    long cupsport = wxConfigBase::Get()->Read(wxT("Config/CupsPort"), -1);
    bool ret = false;
    if (cupsport > 0) {
        // Try connecting to cupsd
        wxHTTP http;
        wxString cupspw = decodeString(wxConfigBase::Get()->Read(wxT("Config/CupsPasswd"), wxEmptyString));
        if (!cupspw.IsEmpty()) {
            http.SetUser(::wxGetUserId());
            http.SetPassword(cupspw);
        }
        http.Connect(wxT("127.0.0.1"), cupsport);
        wxInputStream *is = http.GetInputStream(wxT("/"));
        int res = http.GetResponse();
        wxString svr = http.GetHeader(wxT("server"));
        ::myLogTrace(MYTRACETAG, wxT("isCupsRunning RC=%d SVR=%s"), res, svr.c_str());
        if ((res == 200) && svr.Contains(wxT("CUPS")))
            ret = true;
        delete is;
    }
    m_bCupsRunning = ret;
    ::myLogTrace(MYTRACETAG, wxT("isCupsRunning returning %s"), (ret ? wxT("true") : wxT("false")));
    return ret;
}

    bool
MySession::prepareCups()
{
    long cupsport = wxConfigBase::Get()->Read(wxT("Config/CupsPort"), -1);
    if (cupsport == -1) {
        cupsport = getFirstFreePort(20000);
        if (0 == cupsport) {
            ::wxLogWarning(_("Could not assign a free port for CUPS printing"));
            return false;
        }
        wxConfigBase::Get()->Write(wxT("Config/CupsPort"), cupsport);
    }
    ::myLogTrace(MYTRACETAG, wxT("Check for cupsd running at port %d"), cupsport);
    if (isCupsRunning())
        return true;

    // If connecting to cupsd failed, we select the port again, because we might
    // have connected to another user's cupsd and thus authentication might
    // have failed. In that case, we must start our own new instance of cupsd.
    cupsport = getFirstFreePort(20000);
    if (0 == cupsport) {
        ::wxLogWarning(_("Could not assign a free port for CUPS printing"));
        return false;
    }
    wxConfigBase::Get()->Write(wxT("Config/CupsPort"), cupsport);

    wxString tmp;
    wxString sCupsDir = m_sUserDir;
    sCupsDir << wxFileName::GetPathSeparator() << wxT("cups");

    wxFileName::Mkdir(sCupsDir, 0700, wxPATH_MKDIR_FULL);
    sCupsDir << wxFileName::GetPathSeparator();
    wxFileName::Mkdir(sCupsDir + wxT("spool"), 0700, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(sCupsDir + wxT("cache"), 0700, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(sCupsDir + wxT("certs"), 0700, wxPATH_MKDIR_FULL);
    wxFileName::Mkdir(sCupsDir + wxT("spool") +
            wxFileName::GetPathSeparator() + wxT("tmp"), 0700, wxPATH_MKDIR_FULL);
    {
        CupsClient cl;
        if (cl.IsAvailable()) {
            ArrayOfShares sa = cl.GetShares();
            if (sa.GetCount() > 0) {
                wxFileOutputStream fos(sCupsDir + wxT("printers.conf"));
                wxTextOutputStream tos(fos);
                for (size_t i = 0; i < sa.GetCount(); i++) {
                    tos << wxT("<Printer ") << sa[i].name << wxT(">") << endl;
                    tos << wxT("  Info ") << sa[i].description << endl;
                    tos << wxT("  Location local computer") << endl;
                    tos << wxT("  DeviceURI http://localhost:") << m_pCfg->iGetCupsPort()
                        << wxT("/printers/") << sa[i].name << endl;
                    tos << wxT("  State Idle") << endl;
                    tos << wxT("  Accepting Yes") << endl;
                    tos << wxT("  JobSheets none none") << endl;
                    tos << wxT("  QuotaPeriod 0") << endl;
                    tos << wxT("  PageLimit 0") << endl;
                    tos << wxT("  KLimit 0") << endl;
                    tos << wxT("</Printer>") << endl;
                }
            }
        } else
            return false;
    }

    tmp = m_sSysDir;
    tmp << wxFileName::GetPathSeparator() << wxT("share")
        << wxFileName::GetPathSeparator() << wxT("cups")
        << wxFileName::GetPathSeparator();
    ::wxCopyFile(tmp + wxT("mime.convs"), sCupsDir + wxT("mime.convs"));
    ::wxCopyFile(tmp + wxT("mime.types"), sCupsDir + wxT("mime.types"));
    {
        wxFileOutputStream fos(sCupsDir + wxT("cupsd.conf"));
        wxTextOutputStream tos(fos);
        tos << wxT("CacheDir ") << sCupsDir << wxT("cache") << endl;
        tos << wxT("TempDir ") << sCupsDir << wxT("spool")
            << wxFileName::GetPathSeparator() << wxT("tmp") << endl;
        tos << wxT("RequestRoot ") << sCupsDir << wxT("spool") << endl;
        tos << wxT("ServerRoot ") << m_sUserDir
            << wxFileName::GetPathSeparator() << wxT("cups") << endl;
        tos << wxT("ErrorLog ") << sCupsDir << wxT("error_log") << endl;
        tos << wxT("PageLog ") << sCupsDir << wxT("page_log") << endl;
        tos << wxT("AccessLog ") << sCupsDir << wxT("access_log") << endl;
        tos << wxT("PidFile ") << sCupsDir << wxT("cupsd.pid") << endl;
#ifdef __WXDEBUG__
        tos << wxT("LogLevel debug") << endl;
#else
        tos << wxT("LogLevel info") << endl;
#endif
        tos << wxT("Port ") << (int)cupsport << endl;
        tos << wxT("Browsing Off") << endl;
        tos << wxT("ServerName localhost") << endl;
        tos << wxT("ServerTokens full") << endl;
        tos << wxT("<Location />") << endl;
        tos << wxT("  Order Deny,Allow") << endl;
        tos << wxT("  Deny From All") << endl;
        tos << wxT("  Require user ") << ::wxGetUserId() << endl;
        tos << wxT("  AuthType BasicDigest") << endl;
        tos << wxT("  Allow from 127.0.0.0/8") << endl;
        tos << wxT("</Location>") << endl;
        tos << wxT("<Location /printers>") << endl;
        tos << wxT("  Order Deny,Allow") << endl;
        tos << wxT("  Deny From All") << endl;
        tos << wxT("  Require user root") << endl;
        tos << wxT("  AuthType none") << endl;
        tos << wxT("  Allow from 127.0.0.0/8") << endl;
        tos << wxT("</Location>") << endl;
    }
    {
        wxFileOutputStream fos(sCupsDir + wxT("passwd.md5"));
        wxTextOutputStream tos(fos);
#ifdef __WXMSW__
        tos << ::wxGetUserId() << wxT(":") << ::wxGetUserId();
#else
        struct group *g = getgrgid(getgid());
        wxString sGroupId = wxConvLocal.cMB2WX(g->gr_name);
        tos << ::wxGetUserId() << wxT(":") << sGroupId;
#endif
        wxDateTime now = wxDateTime::Now();
        wxString cupspw = decodeString(wxConfigBase::Get()->Read(wxT("Config/CupsPasswd"), wxEmptyString));
        if (cupspw.IsEmpty()) {
            cupspw << ::wxGetUserId()
                << wxT("NX")
                << now.FormatISODate()
                << wxT("T")
                << now.FormatISOTime();
            cupspw = md5sum(cupspw).Left(16);
            wxConfigBase::Get()->Write(wxT("Config/CupsPasswd"), encodeString(cupspw));
        }
        tos << wxT(":") << md5sum(::wxGetUserId()+ wxT(":CUPS:") + cupspw) << endl;
    }
    wxString cmd = m_pCfg->sGetCupsPath();
    cmd << wxT(" -c ") << sCupsDir << wxT("cupsd.conf");
    ::myLogTrace(MYTRACETAG, wxT("Starting '%s'"), cmd.c_str());
    if (::wxExecute(cmd, wxEXEC_ASYNC|wxEXEC_MAKE_GROUP_LEADER) <= 0)
        return false;
    wxThread::Sleep(500);
    return isCupsRunning();
}

    void
MySession::cleanupOldSessions()
{
    wxDir ud;
    ::myLogTrace(MYTRACETAG, wxT("Cleaning up old session data"));
    if (ud.Open(m_sUserDir)) {
        SessionCleaner sc(m_sUserDir);
        ud.Traverse(sc);
    }
}

    void
MySession::clearSshKeys(const wxString &keyloc)
{
    ::myLogTrace(MYTRACETAG, wxT("Clearing keys for %s at %s"),
            m_pCfg->sGetServerHost().c_str(), keyloc.c_str());
    wxString keyfile = keyloc.BeforeLast(wxT(':'));
    long n;
    if (keyloc.AfterLast(wxT(':')).ToLong(&n)) {
        n--;
        wxTextFile tf(keyfile);
        if (tf.Exists()) {
            if (tf.Open()) {
                ::myLogTrace(MYTRACETAG, wxT("Removing '%s'"), tf[n].c_str());
                tf.RemoveLine(n);
                wxIPV4address ip;
                if (ip.Hostname(m_pCfg->sGetServerHost())) {
                    wxString ipnum = ip.IPAddress();
                    wxString line;
                    for (line = tf.GetFirstLine(); !tf.Eof(); line = tf.GetNextLine()) {
                        if (line.Contains(ipnum)) {
                            ::myLogTrace(MYTRACETAG, wxT("Removing '%s'"), line.c_str());
                            tf.RemoveLine(tf.GetCurrentLine());
                        }
                    }
                }
                tf.Write();
            }
        }
    }
}

    bool
MySession::Create(MyXmlConfig &cfgpar, const wxString password, wxWindow *parent)
{
    m_sClearPassword = password;
    m_bSessionRunning = false;
    m_bCupsRunning = false;
    m_bEsdRunning = false;
    m_lEsdPort = 0;
    m_bAbort = false;
    m_bSessionEstablished = false;
    m_bCollectSessions = false;
    m_bCollectConfig = false;
    m_bIsShadow = false;
    m_sSessionID = wxEmptyString;
    m_pParent = parent;
    MyXmlConfig cfg(cfgpar.sGetFileName());
    m_pCfg = &cfg;

    if (cfg.IsValid()) {
        // Copy misc values from login dialog
        m_pCfg->bSetUseSmartCard(cfgpar.bGetUseSmartCard());
        m_pCfg->bSetEnableSSL(cfgpar.bGetEnableSSL());
        m_pCfg->bSetGuestMode(cfgpar.bGetGuestMode());
        if (!cfgpar.bGetGuestMode()) {
            m_pCfg->sSetUsername(cfgpar.sGetUsername());
            m_pCfg->sSetPassword(password);
        }

        wxConfigBase::Get()->Read(wxT("Config/SystemNxDir"), &m_sSysDir);
        wxConfigBase::Get()->Read(wxT("Config/UserNxDir"), &m_sUserDir);
        checkXarch();

        wxFileName fn(m_sSysDir, wxEmptyString);
        fn.AppendDir(wxT("bin"));
#ifdef __WXMSW__
        fn.SetName(wxT("nxssh.exe"));
#else
        fn.SetName(wxT("nxssh"));
#endif
        wxString nxsshcmd = fn.GetShortPath();
        nxsshcmd << wxT(" -nx -x -2")
            << wxT(" -p ") << m_pCfg->iGetServerPort()
            << wxT(" -o 'RhostsAuthentication no'")
            << wxT(" -o 'PasswordAuthentication no'")
            << wxT(" -o 'RSAAuthentication no'")
            << wxT(" -o 'RhostsRSAAuthentication no'")
            << wxT(" -o 'PubkeyAuthentication yes'");
        m_sTempDir = m_sUserDir;
        m_sTempDir << wxFileName::GetPathSeparator() << wxT("temp")
            << wxFileName::GetPathSeparator() << ::wxGetProcessId();
        wxFileName::Mkdir(m_sTempDir, 0700, wxPATH_MKDIR_FULL);

        wxString logfn = m_sTempDir +
            wxFileName::GetPathSeparator() + wxT("runlog");
#ifdef __VISUALC__
        ofstream *log = new ofstream();
#else
        std::ofstream *log = new std::ofstream();
#endif
        log->open(logfn.mb_str());
        if (!wxLog::GetTraceMasks().GetCount())
            new RunLog(new wxLogStream(log));

        logfn = m_sTempDir +
            wxFileName::GetPathSeparator() + wxT("sshlog");
#ifdef __VISUALC__
        log = new ofstream();
#else
        log = new std::ofstream();
#endif
        log->open(logfn.mb_str());
        m_pSshLog = new wxLogStream(log);
        wxLog::SetLogLevel(wxLOG_Max);

        m_bIsShadow = (m_pCfg->eGetSessionType() == MyXmlConfig::STYPE_SHADOW);
        if (m_pCfg->bGetRemoveOldSessionFiles())
            cleanupOldSessions();

        if (m_pCfg->bGetUseSmartCard()) {
            ::myLogTrace(MYTRACETAG, wxT("Checking for SmartCard"));
            CardWaiterDialog wd;
            m_iReader = wd.WaitForCard(parent);
            if (m_iReader == -1)
                return false;
            nxsshcmd << wxT(" -I ") << m_iReader;
        } else {
            if (m_pCfg->sGetSshKey().IsEmpty()) {
                fn.Assign(m_sSysDir, wxT("server.id_dsa.key"));
                fn.AppendDir(wxT("share"));
                fn.AppendDir(wxT("keys"));
            } else {
                fn.Assign(m_sTempDir, wxT("keylog"));
                wxFile f;
                ::wxRemoveFile(fn.GetFullPath());
                if (f.Open(fn.GetFullPath(), wxFile::write_excl, wxS_IRUSR|wxS_IWUSR)) {
                    f.Write(m_pCfg->sGetSshKey());
                    f.Close();
                } else {
                    ::wxLogSysError(_("Could not write %s"), fn.GetFullPath().c_str());
                    return false;
                }
            }
            nxsshcmd << wxT(" -i ") << cygPath(fn.GetFullPath());
        }

        if (m_pCfg->bGetUseProxy() && (!m_pCfg->sGetProxyHost().IsEmpty())) {
            // Internal (NoMachine) proxy
            if (m_pCfg->sGetProxyUser().IsEmpty())
                nxsshcmd << wxT(" -P ") << m_pCfg->sGetProxyHost() << wxT(":") << m_pCfg->iGetProxyPort();
            else {
                wxString proxyPass = m_pCfg->sGetProxyPass();
                if (!m_pCfg->bGetProxyPassRemember()) {
                    ProxyPasswordDialog dlg(m_pParent);
                    if (dlg.ShowModal() == wxID_OK)
                        proxyPass = dlg.GetPassword();
                    else
                        return false;
                }
                nxsshcmd << wxT(" -P ") << m_pCfg->sGetProxyUser() << wxT(":") << proxyPass << wxT("@")
                    << m_pCfg->sGetProxyHost() << wxT(":") << m_pCfg->iGetProxyPort();
            }
        }

        if (m_pCfg->bGetExternalProxy() && (!m_pCfg->sGetProxyCommand().IsEmpty()))
            nxsshcmd << wxT(" -o 'ProxyCommand ") << cygPath(m_pCfg->sGetProxyCommand()) << wxT("'");

        if (m_pCfg->bGetEnableSSL())
            nxsshcmd << wxT(" -B");
        nxsshcmd << wxT(" -E") << wxT(" nx@") << m_pCfg->sGetServerHost();
        m_sHost = m_pCfg->sGetServerHost();

        fn.Assign(wxFileName::GetHomeDir());
        ::wxSetEnv(wxT("NX_HOME"), cygPath(fn.GetFullPath()));
        ::wxLogInfo(wxT("env: NX_HOME='%s'"), cygPath(fn.GetFullPath()).c_str());
        ::wxSetEnv(wxT("NX_ROOT"), cygPath(m_sUserDir));
        ::wxLogInfo(wxT("env: NX_ROOT='%s'"), cygPath(m_sUserDir).c_str());
        ::wxSetEnv(wxT("NX_SYSTEM"), cygPath(m_sSysDir));
        ::wxLogInfo(wxT("env: NX_SYSTEM='%s'"), cygPath(m_sSysDir).c_str());
        ::wxSetEnv(wxT("NX_CLIENT"), cygPath(::wxGetApp().GetSelfPath()));
        ::wxLogInfo(wxT("env: NX_CLIENT='%s'"), cygPath(::wxGetApp().GetSelfPath()).c_str());
        ::wxSetEnv(wxT("NX_VERSION"), m_sProtocolVersion);
        ::wxLogInfo(wxT("env: NX_VERSION='%s'"), m_sProtocolVersion.c_str());
        ::wxSetEnv(wxT("XAUTHORITY"), getXauthPath(m_eXarch));
        ::wxLogInfo(wxT("env: XAUTHORITY='%s'"), getXauthPath(m_eXarch).c_str());
#ifdef __UNIX__
        // NX needs TEMP or NX_TEMP to be set to the same dir
        // where .X11-unix resides (typically /tmp)
        wxString stmp = wxConvLocal.cMB2WX(x11_socket_path);
        if (!stmp.IsEmpty()) {
            fn.Assign(stmp);
            fn.RemoveLastDir();
            fn.SetName(wxEmptyString);
            ::wxSetEnv(wxT("NX_TEMP"), cygPath(fn.GetFullPath()));
            ::wxLogInfo(wxT("env: NX_TEMP='%s'"), cygPath(fn.GetShortPath()).c_str());
        } else {
            ::wxSetEnv(wxT("NX_TEMP"), wxT("/tmp"));
            ::wxLogInfo(wxT("env: NX_TEMP='/tmp'"));
        }
#else
        ::wxSetEnv(wxT("NX_TEMP"), cygPath(m_sTempDir));
        ::wxLogInfo(wxT("env: NX_TEMP='%s'"), cygPath(m_sTempDir).c_str());
#endif

#ifdef __WXMSW__
        if (!startXserver()) {
            ::wxLogError(_("Could not start local X server"));
            return false;
        }
        if (XARCH_XMING == m_eXarch) {
            // Now, that the X server has been started, // set XAUTHORITY
            // again, but this time in cygwin notation (for nxssh).
            ::wxSetEnv(wxT("XAUTHORITY"), getXauthPath(XARCH_CYGWIN));
            ::wxLogInfo(wxT("env: XAUTHORITY='%s'"), getXauthPath(XARCH_CYGWIN).c_str());
        }
#endif

        fn.Assign(m_sSysDir, wxT("bin"));
        m_iProgress = 0;
        ConnectDialog dlg(m_pParent);
        m_pDlg = &dlg;
        dlg.Show(true);
        dlg.SetStatusText(wxString::Format(_("Connecting to %s ..."),
                    m_pCfg->sGetServerHost().c_str()));
        if (m_pCfg->bGetEnableMultimedia()) {
            long esdpid = wxConfigBase::Get()->Read(wxT("State/nxesdPID"), -1);
            m_lEsdPort = wxConfigBase::Get()->Read(wxT("State/nxesdPort"), -1);
            if ((-1 != esdpid) && (0 < m_lEsdPort))
                m_bEsdRunning = wxProcess::Exists(esdpid);
            if ((!dlg.bGetAbort()) && (!m_bEsdRunning)) {
                dlg.SetStatusText(_("Preparing multimedia service ..."));
                wxFileName fn(m_sSysDir, wxEmptyString);
                fn.AppendDir(wxT("bin"));
                fn.SetName(wxT("nxesd"));
                wxString esdcmd = fn.GetFullPath();
                m_lEsdPort = getFirstFreePort(6000);
                if (0 < m_lEsdPort) {
                    esdcmd << wxT(" -tcp -nobeeps -bind 127.0.0.1 -spawnfd 1 -port ") << m_lEsdPort;
                    wxProcess *nxesd = wxProcess::Open(esdcmd,
                            wxEXEC_ASYNC|wxEXEC_MAKE_GROUP_LEADER);
                    if (nxesd) {
                        nxesd->CloseOutput();
                        wxStopWatch sw;
                        while (!(dlg.bGetAbort() || nxesd->IsInputAvailable())) {
                            ::wxGetApp().Yield(true);
                            wxLog::FlushActive();
                            // Timeout after 10 sec
                            if (sw.Time() > 10000)
                                break;
                        }
                        char msg = '\0';
                        if (nxesd->IsInputAvailable())
                            nxesd->GetInputStream()->Read(&msg, 1);
                        long esdpid = nxesd->GetPid();
                        nxesd->Detach();
                        if (msg) {
                            m_bEsdRunning = true;
                            wxConfigBase::Get()->Write(wxT("State/nxesdPID"), esdpid);
                            wxConfigBase::Get()->Write(wxT("State/nxesdPort"), m_lEsdPort);
                        }
                    }
                    if (!m_bEsdRunning)
                        ::wxLogWarning(_("Could not start multimedia support"));
                } else
                    ::wxLogWarning(_("Could not assign a free port for multimedia support"));
            }
            dlg.SetStatusText(wxString::Format(_("Connecting to %s ..."),
                        m_pCfg->sGetServerHost().c_str()));
        }

        if (dlg.bGetAbort()) {
#ifdef __WXMSW__
            terminateXserver();
#endif
            return false;
        }
        if (getActiveCupsPrinters().GetCount() > 0) {
            dlg.SetStatusText(_("Preparing CUPS service ..."));
            if (!prepareCups())
                ::wxLogWarning(_("Could not start CUPS printing"));
            dlg.SetStatusText(wxString::Format(_("Connecting to %s ..."),
                        m_pCfg->sGetServerHost().c_str()));
        }

        MyIPC nxssh;
        m_pNxSsh = &nxssh;

        ::wxLogInfo(wxT("Starting %s"), nxsshcmd.c_str());
        do {
            m_bRemoveKey = false;
            m_sOffendingKey = wxEmptyString;
            if (nxssh.SshProcess(nxsshcmd, fn.GetShortPath(), this)) {
                m_bGotError = false;
                m_eConnectState = STATE_HELLO;
                while (!(dlg.bGetAbort() || m_bGotError || m_bAbort ||
                            (m_bSessionRunning && m_bSessionEstablished))) {
                    wxLog::FlushActive();
                    ::wxGetApp().Yield(true);
#ifdef __WXMSW__
                    if (m_iXserverPID)
                        AllowSetForegroundWindow(m_iXserverPID);
#endif
                }
                if (dlg.bGetAbort() || m_bGotError || m_bAbort) {
                    if (m_bRemoveKey) {
                        while (m_sOffendingKey.IsEmpty()) {
                            wxLog::FlushActive();
                            ::wxGetApp().Yield(true);
                        }
                    } else {
                        nxssh.Kill();
#ifdef __WXMSW__
                        terminateXserver();
#endif
                        return false;
                    }
                } else {
                    if (m_bSessionEstablished && (!m_bSslTunneling)) {
                        // Unecrypted session (handled by nxproxy), get rid of nxssh
                        nxssh.Kill();
                    }
                }
                wxThread::Sleep(500);
#ifdef __WXMSW__
                if (m_iXserverPID)
                    AllowSetForegroundWindow(m_iXserverPID);
#endif
            } else {
                ::wxLogError(_("Called command was: ") + nxsshcmd);
                ::wxLogError(_("Could not start nxssh."));
#ifdef __WXMSW__
                terminateXserver();
#endif
                return false;
            }
            if (m_bRemoveKey)
                clearSshKeys(m_sOffendingKey);
        } while (m_bRemoveKey);
#ifdef __WXMSW__
        if (m_iXserverPID)
            AllowSetForegroundWindow(m_iXserverPID);
#endif

        if (m_pCfg->bGetEnableUSBIP()) {
            ::myLogTrace(MYTRACETAG, wxT("Enabling UsbIp"));
            ::wxGetApp().SetNxSshPID(nxssh.GetPID());
            ::wxGetApp().SetSessionCfg(*m_pCfg);
            ::wxGetApp().SetSessionID(m_sSessionID.Right(32));
            ::wxGetApp().SetRequireStartUsbIp(true);
        } else {
            if (m_pCfg->bGetUseSmartCard()) {
                ::myLogTrace(MYTRACETAG, wxT("Enabling WatchReader %d"), m_iReader);
                ::wxGetApp().SetNxSshPID(nxssh.GetPID());
                ::wxGetApp().SetReader(m_iReader);
                ::wxGetApp().SetRequireWatchReader(true);
            }
        }
#ifdef __WXMSW__
        unhideNXWin();
#endif
        return true;
    }
    return false;
}

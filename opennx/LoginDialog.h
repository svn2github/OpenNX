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

#ifndef _LOGINDIALOG_H_
#define _LOGINDIALOG_H_

#if defined(__GNUG__) && !defined(__APPLE__)
#pragma interface "LoginDialog.cpp"
#endif

/*!
 * Includes
 */

////@begin includes
#include "LoginDialog_symbols.h"
#include "wx/xrc/xmlres.h"
#include "wx/valgen.h"
#include "wx/statline.h"
////@end includes

/*!
 * Forward declarations
 */

class MyXmlConfig;
#ifdef SINGLE_SESSION
class NxSshWatcher;
#endif

////@begin forward declarations
////@end forward declarations

/*!
 * Control identifiers
 */

////@begin control identifiers
#define ID_DIALOG_LOGIN 10000
#define SYMBOL_LOGINDIALOG_STYLE wxCAPTION|wxRESIZE_BORDER|wxSYSTEM_MENU|wxCLOSE_BOX
#define SYMBOL_LOGINDIALOG_TITLE _("Login - OpenNX")
#define SYMBOL_LOGINDIALOG_IDNAME ID_DIALOG_LOGIN
#define SYMBOL_LOGINDIALOG_SIZE wxSize(200, 150)
#define SYMBOL_LOGINDIALOG_POSITION wxDefaultPosition
////@end control identifiers

/*!
 * Compatibility
 */

#ifndef wxCLOSE_BOX
#define wxCLOSE_BOX 0x1000
#endif

/*!
 * LoginDialog class declaration
 */

class LoginDialog: public wxDialog
{    
    DECLARE_CLASS( LoginDialog )
    DECLARE_EVENT_TABLE()

public:
    /// Constructors
    LoginDialog( );
    LoginDialog( wxWindow* parent, wxWindowID id = SYMBOL_LOGINDIALOG_IDNAME, const wxString& caption = SYMBOL_LOGINDIALOG_TITLE, const wxPoint& pos = SYMBOL_LOGINDIALOG_POSITION, const wxSize& size = SYMBOL_LOGINDIALOG_SIZE, long style = SYMBOL_LOGINDIALOG_STYLE );

    /// Creation
    bool Create( wxWindow* parent, wxWindowID id = SYMBOL_LOGINDIALOG_IDNAME, const wxString& caption = SYMBOL_LOGINDIALOG_TITLE, const wxPoint& pos = SYMBOL_LOGINDIALOG_POSITION, const wxSize& size = SYMBOL_LOGINDIALOG_SIZE, long style = SYMBOL_LOGINDIALOG_STYLE );

    ~LoginDialog();

private:
    /// Creates the controls and sizers
    void CreateControls();

#ifdef SINGLE_SESSION
    // Event handler for events from NxSshWatcher
    void OnSshCount(wxCommandEvent& event);
#endif

////@begin LoginDialog event handler declarations

    /// wxEVT_COMMAND_COMBOBOX_SELECTED event handler for ID_COMBOBOX_SESSION
    void OnComboboxSessionSelected( wxCommandEvent& event );

    /// wxEVT_COMMAND_CHECKBOX_CLICKED event handler for ID_CHECKBOX_SMARTCARD
    void OnCheckboxSmartcardClick( wxCommandEvent& event );

    /// wxEVT_COMMAND_CHECKBOX_CLICKED event handler for ID_CHECKBOX_GUESTLOGIN
    void OnCheckboxGuestloginClick( wxCommandEvent& event );

    /// wxEVT_COMMAND_BUTTON_CLICKED event handler for ID_BUTTON_CONFIGURE
    void OnButtonConfigureClick( wxCommandEvent& event );

    /// wxEVT_COMMAND_BUTTON_CLICKED event handler for wxID_OK
    void OnOkClick( wxCommandEvent& event );

////@end LoginDialog event handler declarations

public:
////@begin LoginDialog member function declarations

    bool GetUseSmartCard() const { return m_bUseSmartCard ; }
    void SetUseSmartCard(bool value) { m_bUseSmartCard = value ; }

    wxString GetSessionName() const { return m_sSessionName ; }
    void SetSessionName(wxString value) { m_sSessionName = value ; }

    /// Retrieves bitmap resources
    wxBitmap GetBitmapResource( const wxString& name );

    /// Retrieves icon resources
    wxIcon GetIconResource( const wxString& name );
////@end LoginDialog member function declarations

    /// Should we show tooltips?
    static bool ShowToolTips();

    // Return filename of last successfully connected config
    wxString GetLastSessionFilename() const { return m_sLastSessionFilename; }
    void SetLastSessionFilename(wxString value) { m_sLastSessionFilename = value; }
#ifdef __WXMAC__
    void SelectSession(wxString name);
#endif

private:
    void ReadConfigDirectory();

////@begin LoginDialog member variables
    wxTextCtrl* m_pCtrlUsername;
    wxTextCtrl* m_pCtrlPassword;
    wxComboBox* m_pCtrlSessionName;
    wxCheckBox* m_pCtrlUseSmartCard;
    wxCheckBox* m_pCtrlGuestLogin;
    wxButton* m_pCtrlConfigure;
    wxButton* m_pCtrlLoginButton;
private:
    wxString m_sUsername;
    wxString m_sPassword;
    bool m_bUseSmartCard;
    wxString m_sSessionName;
    bool m_bGuestLogin;
    wxString m_sTmpUsername;
    wxString m_sTmpPassword;
////@end LoginDialog member variables

#ifdef SINGLE_SESSION
    NxSshWatcher *m_pNxSshWatcher;
#endif
    MyXmlConfig *m_pCurrentCfg;
    wxArrayString m_aConfigFiles;
    wxString m_sLastSessionFilename;
};

#endif
    // _LOGINDIALOG_H_

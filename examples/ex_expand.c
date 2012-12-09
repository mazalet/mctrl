/*
 * Copyright (c) 2012 Martin Mitas
 *
 * This file contains example code for mCtrl library. Code of this example
 * (but not the library itself) has been placed in the public domain.
 */

/* This sample demonstrates basic usage of EXPAND control. */

#include <windows.h>
#include <commctrl.h>
#include <tchar.h>
#include <stdio.h>

#include <mCtrl/expand.h>
#include <mCtrl/dialog.h>
#include "ex_expand.h"


static HINSTANCE hInst;


/* Main window procedure */
static INT_PTR CALLBACK
DlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch(uMsg) {
        case WM_INITDIALOG:
            SendDlgItemMessage(hwndDlg, IDC_EXPAND, MC_EXM_EXPAND, FALSE, 0);
            return TRUE;

        case WM_CLOSE:
            EndDialog(hwndDlg, 0);
            break;

        default:
            return FALSE;
    }

    return TRUE;
}


int APIENTRY
WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    hInst = hInstance;

    /* Initialize mCtrl control */
    mcExpand_Initialize();

    /* Load and show a dialog. */
    mcDialogBox(hInst, MAKEINTRESOURCE(IDD_DIALOG), NULL, DlgProc, MC_DF_DEFAULTFONT);
    return 0;
}

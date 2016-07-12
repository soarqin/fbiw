#include "stdatl.h"

#include <atlframe.h>
#include <atlgdi.h>
#include <atlctrls.h>
#include <atldlgs.h>
#include <atlcrack.h>

#include "resource.h"
#include "maindlg.h"

CAppModule _Module;

int Run(LPTSTR /*lpCmdLine*/ = NULL, int nCmdShow = SW_SHOWDEFAULT) {
	CMessageLoop theLoop;
	_Module.AddMessageLoop(&theLoop);
	CMainDlg dlgMain;
	if(dlgMain.Create(NULL) == NULL) {
		ATLTRACE(_T("Main dialog creation failed!\n"));
		return 0;
	}
	dlgMain.ShowWindow(nCmdShow);
	int nRet = theLoop.Run();
	_Module.RemoveMessageLoop();
	return nRet;
}

int CALLBACK _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR    lpCmdLine, int       nCmdShow) {
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
#if (_WIN32_IE >= 0x0300)
	INITCOMMONCONTROLSEX iccx;
	iccx.dwSize = sizeof(iccx);
	iccx.dwICC = ICC_BAR_CLASSES;	// change to support other controls
	::InitCommonControlsEx(&iccx);
#else
	::InitCommonControls();
#endif

	_Module.Init(NULL, hInstance);
	int nRet = Run(lpCmdLine, nCmdShow);
	_Module.Term();
	WSACleanup();

	return 0;
}

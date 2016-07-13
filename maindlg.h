#pragma once

#define WM_CUSTOM_NETWORK_MSG (WM_USER + 1001)
#define BUFSIZE (256 * 1024)

#if _MSC_VER < 1500
UINT64 htonll(UINT64 value) {
	int num = 42;
	if (*(char *)&num == 42) {
		UINT32 high_part = htonl((UINT32)(value >> 32));
		UINT32 low_part = htonl((UINT32)(value & 0xFFFFFFFFi64));
		return (((UINT64)low_part) << 32) | (UINT64)high_part;
	} else {
		return value;
	}
}
#endif

BOOL GetFileVersion(LPCTSTR strFile, UINT64& ver) {
    TCHAR szVersionBuffer[8192] = _T("");
    DWORD dwVerSize;
    DWORD dwHandle;
    dwVerSize = GetFileVersionInfoSize(strFile, &dwHandle);
    if (dwVerSize == 0)
        return FALSE;
    if (GetFileVersionInfo(strFile, 0, dwVerSize, szVersionBuffer)) {
        VS_FIXEDFILEINFO * pInfo;
        unsigned int nInfoLen;
        if (VerQueryValue(szVersionBuffer, _T("\\"), (void**)&pInfo, &nInfoLen)) {
			ver = ((UINT64)pInfo->dwFileVersionMS << 32) | (UINT64)pInfo->dwFileVersionLS;
            return TRUE;
        }
    }
    return FALSE;   
}  

class CMainDlg : public CDialogImpl<CMainDlg>, public CMessageFilter
{
public:
	enum { IDD = IDD_FBIW_DIALOG };

	CMainDlg() {
		sock = 0;
		status = 0;
		hFile_ = INVALID_HANDLE_VALUE;
		sendbuf_ = (char*)malloc(BUFSIZE);
		sendPos_ = 0;
		sendSize_ = 0;
		totalRead_ = 0;
		totalSize_ = 0;
		frequency_ = 0;
		lastTick_ = 0.;
		lastBytes_ = 0;
	}

	~CMainDlg() {
		free(sendbuf_);
	}

	virtual BOOL PreTranslateMessage(MSG* pMsg)
	{
		return ::IsDialogMessage(m_hWnd, pMsg);
	}

	BEGIN_MSG_MAP_EX(CMainDlg)
		MESSAGE_HANDLER(WM_CUSTOM_NETWORK_MSG, OnNetwork)
		MSG_WM_INITDIALOG(OnInitDialog)
		MSG_WM_CLOSE(OnCloseDialog)
		MSG_WM_DROPFILES(OnDrop)
		COMMAND_ID_HANDLER(IDC_BTN_SELFILE, OnSelFile)
		COMMAND_ID_HANDLER(IDC_BTN_DELFILE, OnDelFile)
		COMMAND_ID_HANDLER(IDC_BTN_MOVEUP, OnMoveUp)
		COMMAND_ID_HANDLER(IDC_BTN_MOVEDOWN, OnMoveDown)
		COMMAND_ID_HANDLER(IDC_BTN_CLEAR, OnClear)
		COMMAND_ID_HANDLER(IDC_BTN_GO, OnStart)
		COMMAND_HANDLER_EX(IDC_FILELIST, LBN_SELCHANGE, OnListSelChanged)
	END_MSG_MAP()

	LRESULT OnNetwork(UINT /*uMsg*/, WPARAM wParam, LPARAM lParam, BOOL& /*bHandled*/) {
		SOCKET sock = (SOCKET)wParam;
		long event = WSAGETSELECTEVENT(lParam);
		int error = WSAGETSELECTERROR(lParam);
		if (error) {
			SetMsg(_T("Error occured!"));
			Cleanup();
			return 0;
		}
		switch (event) {
		case FD_CONNECT: {
			int nbuf = 32 * 1024;
			setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char *)&nbuf, sizeof(nbuf));
			SetMsg(_T("Connected to remote"));
			UINT fcount = GetPendingCount();
			UINT32 cnt = htonl(fcount);
			if (old_) {
				status = 2;
				WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_WRITE | FD_CLOSE);
			} else {
				status = 1;
				WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_READ | FD_CLOSE);
				if (send(sock, (const char*)&cnt, 4, 0) < 4) {
					SetMsg(_T("Send error"));
					Cleanup();
					break;
				}
			}
			break;
		}
		case FD_READ: {
			if (status == 1) {
				status = 2;
				char n;
				if (recv(sock, &n, 1, 0) <= 0 || n == 0) {
					SetMsg(_T("Sending interrupted by remote"));
					Cleanup();
					break;
				}
				WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_WRITE | FD_CLOSE);
			}
			break;
		}
		case FD_WRITE: {
			if (status == 2) {
				UINT64 sz = htonll(totalSize_);
				const char* p = (const char*)&sz;
				memcpy(sendbuf_ + sendSize_, p, 8);
				sendSize_ += 8;
				status = 3;
				int n = send(sock, sendbuf_ + sendPos_, sendSize_ - sendPos_, 0);
				WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_WRITE | FD_CLOSE);
				if (n <= 0) {
					int err = WSAGetLastError();
					if (err == WSAEWOULDBLOCK || err == WSATRY_AGAIN)
						break;
					SetMsg(_T("Sending error"));
					Cleanup();
					break;
				}
				sendPos_ += n;
			} else if (status == 3 || status == 4) {
				if (sendPos_ >= sendSize_) {
					sendPos_ = sendSize_ = 0;
					DWORD dwRead = 0;
					if (!ReadFile(hFile_, sendbuf_, BUFSIZE, &dwRead, NULL)) {
						SetMsg(_T("Sending error"));
						Cleanup();
						break;
					}
					sendSize_ += dwRead;
					totalRead_ += (UINT64)dwRead;
					if (totalRead_ >= totalSize_) {
						status = 4;
						WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_READ | FD_CLOSE);
					}
				}
				if (sendSize_ > sendPos_) {
					int n = send(sock, sendbuf_ + sendPos_, sendSize_ - sendPos_, 0);
					lastBytes_ += (UINT64)n;
					SetProgress();
					if (n <= 0) {
						int err = WSAGetLastError();
						if (err == WSAEWOULDBLOCK || err == WSATRY_AGAIN)
							break;
						SetMsg(_T("Sending error"));
						Cleanup();
						break;
					}
					sendPos_ += n;
				}
				if (sendPos_ >= sendSize_) {
					sendPos_ = sendSize_ = 0;
					if (status == 4) {
						SetProgress(TRUE);
						SetPendingFinished();
						if (GetPendingCount() == 0) {
							SetMsg(_T("Finished!"));
							Cleanup();
							break;
						}
						if (!OpenNextFile()) {
							Cleanup();
							break;
						}
						if (old_) {
							status = 0;
							WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, 0);
							closesocket(sock);
							sock = 0;
							StartConnect();
						} else {
							status = 1;
							WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_READ | FD_CLOSE);
						}
					} else
						WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_WRITE | FD_CLOSE);
				} else {
					WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_WRITE | FD_CLOSE);
				}
			}
			break;
		}
		case FD_CLOSE: {
			SetMsg(_T("Connection closed by remote"));
			Cleanup();
			break;
		}
		}
		return 0;
	}


	LRESULT OnInitDialog(HWND /*wParam*/, LPARAM /*lParam*/) {
		// center the dialog on the screen
		CenterWindow();

		TCHAR path[256];
		GetModuleFileName(NULL, path, 256);
		UINT64 ver;
		GetFileVersion(path, ver);
		{
			TCHAR title[256];
			GetWindowText(title, 256);
			wsprintf(title, _T("%s v%u.%u"), title, (UINT32)(ver >> 48), (UINT32)(ver >> 32) & 0xFFFF);
			SetWindowText(title);
		}

		// set icons
		HICON hIcon = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDI_ICON), 
			IMAGE_ICON, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
		SetIcon(hIcon, TRUE);
		HICON hIconSmall = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDI_ICON), 
			IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
		SetIcon(hIconSmall, FALSE);
		sBar_.Create(m_hWnd, rcDefault, NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, IDC_STATUSBAR);
		RECT rect;
		GetClientRect(&rect);
		int w = rect.right - rect.left;
		int ind[5] = {w / 2, w * 2 / 3, w * 3 / 4, w - 1};
		sBar_.SetParts(4, ind);
		sBar_.GetRect(3, &rect);
		prog_.Create(sBar_.m_hWnd, &rect, NULL, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, IDC_PROGRESS);
		prog_.SetRange(0, 100);

		// register object for message filtering
		CMessageLoop* pLoop = _Module.GetMessageLoop();
		pLoop->AddMessageFilter(this);

		flist_ = GetDlgItem(IDC_FILELIST);
		go_ = GetDlgItem(IDC_BTN_GO);

		PathRemoveFileSpec(path);
		PathAppend(path, _T("FBIW.ini"));
		TCHAR addr[256];
		if (GetPrivateProfileString(_T("main"), _T("address"), _T(""), addr, 256, path))
			SetDlgItemText(IDC_EDIT_IP, addr);
		UINT r = GetPrivateProfileInt(_T("main"), _T("oldver"), 0, path);
		CheckDlgButton(IDC_CHECK_OLD, r);
		r = GetPrivateProfileInt(_T("main"), _T("purge"), 0, path);
		CheckDlgButton(IDC_CHECK_PURGE, r);
		TCHAR files[4096];
		if (GetPrivateProfileString(_T("main"), _T("files"), _T(""), files, 4096, path)) {
			TCHAR* pf = files;
			while (1) {
				TCHAR* r = _tcschr(pf, '|');
				if (r == NULL) {
					int i = flist_.AddString(pf);
					if (pf[0] == _T('<'))
						flist_.SetItemData(i, 1);
					break;
				} else {
					*r = 0;
					int i = flist_.AddString(pf);
					if (pf[0] == _T('<'))
						flist_.SetItemData(i, 1);
					pf = r + 1;
				}
			}
		}
		UpdateFileListButtons();

		return TRUE;
	}

	void OnCloseDialog() {
		TCHAR addr[256];
		GetDlgItemText(IDC_EDIT_IP, addr, 256);
		TCHAR path[256];
		GetModuleFileName(NULL, path, 256);
		PathRemoveFileSpec(path);
		PathAppend(path, _T("FBIW.ini"));
		WritePrivateProfileString(_T("main"), _T("address"), addr, path);
		WritePrivateProfileString(_T("main"), _T("oldver"), IsDlgButtonChecked(IDC_CHECK_OLD) ? _T("1") : _T("0"), path);
		WritePrivateProfileString(_T("main"), _T("purge"), IsDlgButtonChecked(IDC_CHECK_PURGE) ? _T("1") : _T("0"), path);
		TCHAR files[4096] = {0};
		int count = flist_.GetCount();
		for (int i = 0; i < count; ++i) {
			TCHAR fname[256];
			flist_.GetText(i, fname);
			if (lstrlen(files) + 1 + lstrlen(fname) >= 4096) break;
			if (i > 0)
				lstrcat(files, _T("|"));
			if (fname[0] == _T(' ') && fname[1] == _T('-') && fname[2] == _T('>') && fname[3] == _T(' '))
				lstrcat(files, fname + 4);
			else
				lstrcat(files, fname);
		}
		WritePrivateProfileString(_T("main"), _T("files"), files, path);
		DestroyWindow();
		::PostQuitMessage(0);
	}

	LRESULT OnSelFile(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		if (hFile_ != INVALID_HANDLE_VALUE) return 0;
		CFileDialog dialog(TRUE, _T("cia"), NULL, 0, _T("CIA files\0*.cia\0"), m_hWnd);
		if (dialog.DoModal() == IDOK) {
			flist_.AddString(dialog.m_szFileName);
			UpdateFileListButtons();
		}
		return 0;
	}

	LRESULT OnDelFile(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		if (hFile_ != INVALID_HANDLE_VALUE) return 0;
		int cur = flist_.GetCurSel();
		if (cur < 0) return 0;
		flist_.DeleteString(cur);
		UpdateFileListButtons();
		return 0;
	}
	
	LRESULT OnMoveUp(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		if (hFile_ != INVALID_HANDLE_VALUE) return 0;
		int cur = flist_.GetCurSel();
		if (cur <= 0) return 0;
		TCHAR n[256];
		flist_.GetText(cur, n);
		DWORD_PTR data = flist_.GetItemData(cur);
		flist_.DeleteString(cur);
		flist_.InsertString(cur - 1, n);
		flist_.SetItemData(cur - 1, data);
		flist_.SetCurSel(cur - 1);
		UpdateFileListButtons();
		return 0;
	}
	
	LRESULT OnMoveDown(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		if (hFile_ != INVALID_HANDLE_VALUE) return 0;
		int cur = flist_.GetCurSel();
		if (cur < 0) return 0;
		int count = flist_.GetCount();
		if (cur + 1 >= count) return 0;
		TCHAR n[256];
		flist_.GetText(cur, n);
		DWORD_PTR data = flist_.GetItemData(cur);
		flist_.DeleteString(cur);
		flist_.InsertString(cur + 1, n);
		flist_.SetItemData(cur + 1, data);
		flist_.SetCurSel(cur + 1);
		UpdateFileListButtons();
		return 0;
	}

	LRESULT OnClear(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		if (hFile_ != INVALID_HANDLE_VALUE) return 0;
		flist_.ResetContent();
		UpdateFileListButtons();
		return 0;
	}

	LRESULT OnStart(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		if (GetPendingCount() == 0) return 0;
		old_ = IsDlgButtonChecked(IDC_CHECK_OLD) != 0;
		OpenNextFile();
		UpdateFileListButtons();
		StartTiming();
		StartConnect();
		return 0;
	}

	LRESULT OnListSelChanged(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/) {
		UpdateFileListButtons();
		return 0;
	}

	void OnDrop(HDROP drop) {
		TCHAR fn[256];
		UINT count = DragQueryFile(drop, UINT_MAX, fn, 256);
		for (UINT i = 0; i < count; ++i) {
			if (DragQueryFile(drop, i, fn, 256)) {
				if (lstrcmpi(PathFindExtension(fn), _T(".cia")) == 0) {
					flist_.AddString(fn);
				}
			}
		}
		UpdateFileListButtons();
	}

private:
	void StartConnect() {
		char addr[256];
		::GetWindowTextA(GetDlgItem(IDC_EDIT_IP), addr, 256);
		struct hostent* he;
		sock = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in rec_addr;
		rec_addr.sin_family = AF_INET;
		rec_addr.sin_port = htons(5000);
		if((he = gethostbyname(addr)) != NULL) {
			memcpy(&rec_addr.sin_addr, he->h_addr_list[0], he->h_length);
		} else
			rec_addr.sin_addr.s_addr = inet_addr(addr);
		WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_CONNECT | FD_CLOSE);
		connect(sock, (struct sockaddr *)&rec_addr, sizeof(rec_addr));
	}

	bool OpenNextFile() {
		TCHAR fname[256];
		int n = flist_.GetCount();
		bool found = false;
		for (int i = 0; i < n; ++i) {
			if (flist_.GetItemData(i) == 0) {
				flist_.GetText(i, fname);
				TCHAR nname[260] = _T(" -> ");
				lstrcat(nname, fname);
				flist_.DeleteString(i);
				flist_.InsertString(i, nname);
				flist_.SetItemData(i, 2);
				found = true;
				break;
			}
		}
		if (!found) return false;
		hFile_ = CreateFile(fname, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile_ == NULL || hFile_ == INVALID_HANDLE_VALUE) {
			SetMsg(_T("Unable to open file for reading"));
			return false;
		}
		totalRead_ = 0;
		unsigned long hi;
		totalSize_ = GetFileSize(hFile_, &hi);
		totalSize_ |= (UINT64)hi << 32;
		SetMsg2(_T(""));
		SetMsg3(_T(""));
		return true;
	}

	inline void SetProgress(BOOL force = FALSE) {
		double now = CurrTime();
		if (!force && now - lastTick_ < 0.5) return;
		UINT64 spd = (UINT64)((double)(INT64)lastBytes_ / (now - lastTick_));
		TCHAR spdtxt[32];
		if (spd >= 1024 * 1024 * 10)
			wsprintf(spdtxt, _T("%u.%03uMB/s"), (UINT32)(spd >> 20), (UINT32)((spd >> 10) & 0x3FF));
		else if (spd >= 1024 * 10)
			wsprintf(spdtxt, _T("%u.%03uKB/s"), (UINT32)(spd >> 10), (UINT32)(spd & 0x3FF));
		else
			wsprintf(spdtxt, _T("%uB/s"), (UINT32)spd);
		SetMsg2(spdtxt);
		UINT32 per = (UINT32)(totalRead_ * 100ui64 / totalSize_);
		prog_.SetPos(per);
		TCHAR ntxt[64];
		wsprintf (ntxt, _T("%I64u/%I64u"), totalRead_, totalSize_);
		SetMsg(ntxt);
		wsprintf (ntxt, _T("%u%%"), per);
		SetMsg3(ntxt);

		lastBytes_ = 0;
		lastTick_ = now;
	}

	inline void SetMsg(LPCTSTR txt) {
		sBar_.SetText(0, txt);
	}

	inline void SetMsg2(LPCTSTR txt) {
		sBar_.SetText(1, txt);
	}

	inline void SetMsg3(LPCTSTR txt) {
		sBar_.SetText(2, txt);
	}

	inline void StartTiming() {
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		frequency_ = freq.QuadPart;
		lastTick_ = CurrTime();
	}

	double CurrTime() {
		LARGE_INTEGER tm;
		QueryPerformanceCounter(&tm);
		return tm.QuadPart / (double)(INT64)frequency_;
	}

	void Cleanup() {
		if (sock != 0) {
			WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, 0);
			closesocket(sock);
			sock = 0;
		}
		if (hFile_ != INVALID_HANDLE_VALUE) {
			CloseHandle(hFile_);
			hFile_ = INVALID_HANDLE_VALUE;
		}
		sendPos_ = 0;
		sendSize_ = 0;
		totalRead_ = 0;
		totalSize_ = 0;
		frequency_ = 0;
		lastTick_ = 0.;
		lastBytes_ = 0;

		int n = flist_.GetCount();
		for (int i = 0; i < n; ++i) {
			if (flist_.GetItemData(i) == 2) {
				TCHAR n[260];
				flist_.GetText(i, n);
				flist_.DeleteString(i);
				flist_.InsertString(i, n + 4);
				flist_.SetItemData(i, 0);
			}
		}
		if (IsDlgButtonChecked(IDC_CHECK_PURGE)) {
			int n = flist_.GetCount();
			for (int i = n - 1; i >= 0; --i) {
				if (flist_.GetItemData(i) == 1)
					flist_.DeleteString(i);
			}
		}

		UpdateFileListButtons();
	}

	UINT32 GetPendingCount() {
		UINT32 result = 0;
		int n = flist_.GetCount();
		for (int i = 0; i < n; ++i) {
			if (flist_.GetItemData(i) != 1)
				++result;
		}
		return result;
	}

	inline void SetPendingFinished() {
		int n = flist_.GetCount();
		for (int i = 0; i < n; ++i) {
			if (flist_.GetItemData(i) == 2) {
				TCHAR fn[260];
				flist_.GetText(i, fn);
				memcpy(fn, _T("<O> "), 4 * sizeof(TCHAR));
				flist_.DeleteString(i);
				flist_.InsertString(i, fn);
				flist_.SetItemData(i, 1);
				break;
			}
		}
	}

	void UpdateFileListButtons() {
		if (hFile_ != INVALID_HANDLE_VALUE) {
			::EnableWindow(GetDlgItem(IDC_BTN_GO), FALSE);
			::EnableWindow(GetDlgItem(IDC_BTN_SELFILE), FALSE);
			::EnableWindow(GetDlgItem(IDC_BTN_DELFILE), FALSE);
			::EnableWindow(GetDlgItem(IDC_BTN_MOVEUP), FALSE);
			::EnableWindow(GetDlgItem(IDC_BTN_MOVEDOWN), FALSE);
			return;
		}
		BOOL f = FALSE;
		int n = flist_.GetCount();
		for (int i = 0; i < n; ++i) {
			if (flist_.GetItemData(i) == 0) {
				f = TRUE;
				break;
			}
		}
		::EnableWindow(GetDlgItem(IDC_BTN_GO), f);
		int sel = flist_.GetCurSel();
		int count = flist_.GetCount();
		::EnableWindow(GetDlgItem(IDC_BTN_SELFILE), TRUE);
		::EnableWindow(GetDlgItem(IDC_BTN_DELFILE), sel >= 0);
		::EnableWindow(GetDlgItem(IDC_BTN_MOVEUP), sel > 0);
		::EnableWindow(GetDlgItem(IDC_BTN_MOVEDOWN), sel >= 0 && sel + 1 < count);
	}

private:
	int sock;
	int status;
	CListBox flist_;
	CProgressBarCtrl prog_;
	CButton go_;
	CStatusBarCtrl sBar_;
	HANDLE hFile_;
	char* sendbuf_;
	UINT32 sendPos_;
	UINT32 sendSize_;
	UINT64 totalRead_;
	UINT64 totalSize_;
	UINT64 frequency_;
	double lastTick_;
	UINT64 lastBytes_;
	bool old_;
};

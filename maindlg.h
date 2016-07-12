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

class CMainDlg : public CDialogImpl<CMainDlg>, public CMessageFilter
{
private:
	UINT32 fcount_;
	TCHAR** files_;
	int sock;
	int status;
	CStatic msg_, msg2_, msg3_;
	CProgressBarCtrl prog_;
	CButton go_;
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

	void StartTiming() {
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
		if (fcount_ > 0) {
			for (UINT32 i = 0; i < fcount_; ++i)
				free(files_[i]);
			free(files_);
			files_ = NULL;
			fcount_ = 0;
		}
		sendPos_ = 0;
		sendSize_ = 0;
		totalRead_ = 0;
		totalSize_ = 0;
		frequency_ = 0;
		lastTick_ = 0.;
		lastBytes_ = 0;
	}

public:
	enum { IDD = IDD_FBIW_DIALOG };

	CMainDlg() {
		fcount_ = 0;
		files_ = NULL;
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

	void UpdateData() {
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
		wsprintf (ntxt, _T("%u%% (%I64u/%I64u)"), per, totalRead_, totalSize_);
		SetMsg3(ntxt);

		lastBytes_ = 0;
		lastTick_ = now;
	}

	void SetMsg(LPCTSTR txt) {
		SetDlgItemText(IDC_INFO, txt);
	}

	void SetMsg2(LPCTSTR txt) {
		SetDlgItemText(IDC_INFO2, txt);
	}

	void SetMsg3(LPCTSTR txt) {
		SetDlgItemText(IDC_INFO3, txt);
	}

	BEGIN_MSG_MAP_EX(CMainDlg)
		MESSAGE_HANDLER(WM_CUSTOM_NETWORK_MSG, OnNetwork)
		MSG_WM_INITDIALOG(OnInitDialog)
		MSG_WM_CLOSE(OnCloseDialog)
		MSG_WM_DROPFILES(OnDrop)
		COMMAND_ID_HANDLER(IDC_BTN_SELFILE, OnSelFile)
		COMMAND_ID_HANDLER(IDC_BTN_GO, OnStart)
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
			UINT32 cnt = htonl(fcount_);
			StartTiming();
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
						free(files_[0]);
						memmove(files_, files_ + 1, sizeof(TCHAR*)*(fcount_ - 1));
						--fcount_;
						if (fcount_ == 0) {
							free(files_);
							files_ = NULL;
							SetMsg(_T("Finished!"));
							Cleanup();
							break;
						}
						if (!OpenNextFile()) {
							Cleanup();
							break;
						}
						status = 1;
						WSAAsyncSelect(sock, m_hWnd, WM_CUSTOM_NETWORK_MSG, FD_READ | FD_CLOSE);
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

		// set icons
		HICON hIcon = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDI_ICON), 
			IMAGE_ICON, ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
		SetIcon(hIcon, TRUE);
		HICON hIconSmall = (HICON)::LoadImage(_Module.GetResourceInstance(), MAKEINTRESOURCE(IDI_ICON), 
			IMAGE_ICON, ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
		SetIcon(hIconSmall, FALSE);

		// register object for message filtering
		CMessageLoop* pLoop = _Module.GetMessageLoop();
		pLoop->AddMessageFilter(this);

		msg_ = GetDlgItem(IDC_INFO);
		msg2_ = GetDlgItem(IDC_INFO);
		prog_ = GetDlgItem(IDC_PROGRESS);
		go_ = GetDlgItem(IDC_BTN_GO);
		prog_.SetRange(0, 100);

		TCHAR path[256];
		GetModuleFileName(NULL, path, 256);
		PathRemoveFileSpec(path);
		PathAppend(path, _T("FBIW.ini"));
		TCHAR addr[256];
		if (GetPrivateProfileString(_T("main"), _T("address"), _T(""), addr, 256, path))
			SetDlgItemText(IDC_EDIT_IP, addr);
		UINT r = GetPrivateProfileInt(_T("main"), _T("oldver"), 0, path);
		CheckDlgButton(IDC_CHECK_OLD, r);

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
		DestroyWindow();
		::PostQuitMessage(0);
	}

	LRESULT OnSelFile(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		CFileDialog dialog(TRUE, _T("cia"), NULL, 0, _T("CIA files\0*.cia\0"), m_hWnd);
		if (dialog.DoModal() == IDOK)
			::SetWindowText(GetDlgItem(IDC_EDIT_FILE), dialog.m_szFileName);
		return 0;
	}

	LRESULT OnStart(WORD /*wNotifyCode*/, WORD /*wID*/, HWND /*hWndCtl*/, BOOL& /*bHandled*/) {
		Cleanup();
		{
			TCHAR fname[256];
			::GetWindowText(GetDlgItem(IDC_EDIT_FILE), fname, 256);
			files_ = (TCHAR**)malloc(sizeof(TCHAR*) * 1);
			files_[0] = _tcsdup(fname);
			fcount_ = 1;
		}
		old_ = IsDlgButtonChecked(IDC_CHECK_OLD) != 0;
		OpenNextFile();
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
		return 0;
	}

	void OnDrop(HDROP drop) {
		TCHAR fn[256];
		UINT count = DragQueryFile(drop, UINT_MAX, fn, 256);
		if (DragQueryFile(drop, 0, fn, 256)) {
			if (lstrcmpi(PathFindExtension(fn), _T(".cia")) == 0)
				::SetWindowText(GetDlgItem(IDC_EDIT_FILE), fn);
		}
	}

	bool OpenNextFile() {
		if (fcount_ == 0) return false;
		hFile_ = CreateFile(files_[0], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		if (hFile_ == NULL || hFile_ == INVALID_HANDLE_VALUE) {
			SetMsg(_T("Unable to open file for reading"));
			return false;
		}
		unsigned long hi;
		totalSize_ = GetFileSize(hFile_, &hi);
		totalSize_ |= (UINT64)hi << 32;
		SetMsg2(_T(""));
		SetMsg3(_T(""));
		return true;
	}
};

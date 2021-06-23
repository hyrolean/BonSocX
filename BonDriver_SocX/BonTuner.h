//===========================================================================
#pragma once
#ifndef _BONTUNER_20210405181022414_H_INCLUDED_
#define _BONTUNER_20210405181022414_H_INCLUDED_

#include <winsock2.h>
#include <ws2tcpip.h>
#include "IBonDriver2.h"
//---------------------------------------------------------------------------

  // CBonTuner

class CBonTuner : public IBonDriver2
{
protected:
	enum SOCXTYPE {
		SOCX_UDP,
		SOCX_TCP
	};
	struct TSIO {
		WSAOVERLAPPED Ovl;
		BUFFER<BYTE> Buff;
		int Stat ;
		DWORD RxSz ;
	};
	using TSIOQUEUE = std::vector<TSIO> ;
	using TSIOEVENTS = BUFFER<HANDLE> ;
    using PORTS = std::vector<std::pair<DWORD,std::wstring>> ;
	struct SPACE {
		std::wstring Name;
		SOCXTYPE Type;
		PORTS Ports ;
		SPACE() : Type(SOCX_UDP) {}
		SPACE(std::wstring Name_, SOCXTYPE Type_, const PORTS &Ports_)
		 : Name(Name_), Type(Type_), Ports(Ports_) {}
		SPACE(const SPACE &S) : Name(S.Name), Type(S.Type), Ports(S.Ports) {}
	};
	using SPACES = std::vector<SPACE> ;

private:
	TSIOQUEUE TSIOQueue;
	TSIOEVENTS TSIOEvents;
	CAsyncFifo *AsyncTsFifo;
	event_object TsStreamEvent;
	std::wstring TunerName;
	SPACES Spaces;
    int SocType; // SOCK_STREAM / SOCK_DGRAM
	SOCKET Soc,TcpSoc;
	DWORD CurSpace;
	DWORD CurChannel;
	BOOL TunerOpened;
	HANDLE AsyncTsThread;
	BOOL AsyncTsTerm;
	DWORD AsyncTsCurStart;

protected: // settings
	BOOL IPV6;
	BOOL UDP;
	BOOL TCP;

	// TSIO
	DWORD TSIOPACKETSIZE ;
	DWORD TSIOQUEUENUM ;
	DWORD TSIOPOLLTIMEOUT ;

	// ”ñ“¯ŠúTS
	DWORD ASYNCTSPACKETSIZE;
	DWORD ASYNCTSQUEUENUM;
	DWORD ASYNCTSQUEUEMAX;
	DWORD ASYNCTSQUEUESTART;
	DWORD ASYNCTSEMPTYBORDER;
	DWORD ASYNCTSEMPTYLIMIT;
	//DWORD ASYNCTSRECVTHREADWAIT;
	int   ASYNCTSRECVTHREADPRIORITY;
	BOOL  ASYNCTSFIFOALLOCWAITING;
	DWORD ASYNCTSFIFOTHREADWAIT;
	int   ASYNCTSFIFOTHREADPRIORITY;

protected:
	void Initialize();
	void Finalize();
	void LoadIni(std::string iniFileName);
	BOOL SocOpen(int port);
	void SocClose();

protected: // AsyncTsThread
	void StartAsyncTsThread();
	void StopAsyncTsThread();
	unsigned int AsyncTsThreadProcMain();
	static unsigned int __stdcall AsyncTsThreadProc (PVOID pv);

public: // IBonDriver
	virtual const BOOL OpenTuner(void);
	virtual void CloseTuner(void);

	virtual const BOOL SetChannel(const BYTE bCh);
	virtual const float GetSignalLevel(void);

	virtual const DWORD WaitTsStream(const DWORD dwTimeOut = 0);
	virtual const DWORD GetReadyCount(void);

	virtual const BOOL GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain);
	virtual const BOOL GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain);

	virtual void PurgeTsStream(void);

public: // IBonDriver2
	virtual LPCTSTR GetTunerName(void);

	virtual const BOOL IsTunerOpening(void);

	virtual LPCTSTR EnumTuningSpace(const DWORD dwSpace);
	virtual LPCTSTR EnumChannelName(const DWORD dwSpace, const DWORD dwChannel);

	virtual const BOOL SetChannel(const DWORD dwSpace, const DWORD dwChannel);

	virtual const DWORD GetCurSpace(void);
	virtual const DWORD GetCurChannel(void);

public: // constructor / destructor
	CBonTuner();
	virtual ~CBonTuner();
	virtual void Release(void);

};


extern HMODULE DllModule ;
extern CBonTuner *BonTuner ;

//===========================================================================
#endif // _BONTUNER_20210405181022414_H_INCLUDED_

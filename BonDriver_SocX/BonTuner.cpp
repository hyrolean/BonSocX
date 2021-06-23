//===========================================================================
#include "stdafx.h"
#include <process.h>

#include "BonTuner.h"
//---------------------------------------------------------------------------
#pragma comment(lib, "ws2_32.lib")

using namespace std;

HMODULE DllModule = NULL ;
CBonTuner *BonTuner = NULL ;

//#define STRICTLY_CHECK_EVENT_SIGNALS
//===========================================================================
// Static Functions
//---------------------------------------------------------------------------
static bool WinSockInitialized = false ;
static bool WinSockInitialize()
{
	if(!WinSockInitialized) {
		WSADATA data;
		if (WSAStartup(MAKEWORD(2,0),&data) != 0)
			return false;
		WinSockInitialized=true;
	}
	return true;
}
//---------------------------------------------------------------------------
static void WinSockFinalize()
{
	if(WinSockInitialized) {
		WSACleanup();
		WinSockInitialized=false;
	}
}
//---------------------------------------------------------------------------
static string ModuleFileName()
{
	char path[MAX_PATH] = {0};
	GetModuleFileNameA(DllModule,path,MAX_PATH);
	return path ;
}
//===========================================================================
// CBonTuner
//---------------------------------------------------------------------------
CBonTuner::CBonTuner()
{
	AsyncTsFifo = NULL ;
	Type = SOCK_DGRAM ;
	IPV6 = FALSE ;
	TcpSoc=Soc=INVALID_SOCKET;
	CurChannel=0xFFFFFFFF;
	TunerOpened=FALSE;
	//WinSockInitialize();
	Initialize();
}
//---------------------------------------------------------------------------
CBonTuner::~CBonTuner()
{
	Finalize();
	//WinSockFinalize();

	if(BonTuner==this)
		BonTuner=NULL;
}
//---------------------------------------------------------------------------
void CBonTuner::Release(void)
{
	delete this;
}
//---------------------------------------------------------------------------
void CBonTuner::Initialize()
{
	const DWORD def_packet_size =  48128UL ;
	string mfname = ModuleFileName();
	string prefix = upper_case(file_prefix_of(mfname));

	// initialize variables
	if(prefix=="BONDRIVER_TCP") {
		Type = SOCK_STREAM ;
	}else {
		Type = SOCK_DGRAM ;
	}
	AsyncTsThread = INVALID_HANDLE_VALUE ;
	AsyncTsTerm = FALSE ;

	// TSIO
	TSIOPACKETSIZE   = def_packet_size ;
    TSIOQUEUENUM     = 10UL ;
	TSIOPOLLTIMEOUT  = 100UL ;

	// 非同期TS
	ASYNCTSPACKETSIZE         = def_packet_size         ; // 非同期TSデータのパケットサイズ
	ASYNCTSQUEUENUM           = 66UL                    ; // 非同期TSデータの環状ストック数(初期値)
	ASYNCTSQUEUEMAX           = 660UL                   ; // 非同期TSデータの環状ストック最大数
	ASYNCTSQUEUESTART         = 10UL                    ; // 非同期TSデータの初期バッファ充填数
	ASYNCTSEMPTYBORDER        = 22UL                    ; // 非同期TSデータの空きストック数底値閾値(アロケーション開始閾値)
	ASYNCTSEMPTYLIMIT         = 11UL                    ; // 非同期TSデータの最低限確保する空きストック数(オーバーラップからの保障)
	//ASYNCTSRECVTHREADWAIT     = 250UL                   ; // 非同期TSスレッドキュー毎に待つ最大時間
	ASYNCTSRECVTHREADPRIORITY = THREAD_PRIORITY_HIGHEST ; // 非同期TSスレッドの優先度
	ASYNCTSFIFOALLOCWAITING   = FALSE                   ; // 非同期TSデータのアロケーションの完了を待つかどうか
	ASYNCTSFIFOTHREADWAIT     = 1000UL                  ; // 非同期TSデータのアロケーションの監視毎時間
	ASYNCTSFIFOTHREADPRIORITY = THREAD_PRIORITY_HIGHEST ; // 非同期TSアロケーションスレッドの優先度


	#define ACALCI_ENTRY_CONST(name) do { \
		acalci_entry_const(#name,(int)name); \
		acalci64_entry_const(#name,(__int64)name); \
		}while(0)

	ACALCI_ENTRY_CONST(SOCK_STREAM);
	ACALCI_ENTRY_CONST(SOCK_DGRAM);

	#undef ACALCI_ENTRY_CONST

	string iniFileName = file_path_of(mfname)+file_prefix_of(mfname)+".ini" ;
	LoadIni(iniFileName);

	AsyncTsCurStart = ASYNCTSQUEUESTART ;

	// Ports
	for(DWORD i=0;i<10;i++) {
		DWORD dwPort = ( Type==SOCK_DGRAM ? 1234 : 2230 ) + i ;
		wstring strName = Type==SOCK_DGRAM ? L"UDP Port:" : L"TCP Port:" ;
		strName += itows(dwPort) ;
		Ports.push_back(pair<DWORD,wstring>(dwPort, strName));
	}

	// TSIOキュー
	TSIOQueue.resize(TSIOQUEUENUM);
	for(auto &v : TSIOQueue) {
		ZeroMemory(&v.Ovl,sizeof v.Ovl) ;
		v.Ovl.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL) ;
		v.Buff.resize(TSIOPACKETSIZE) ;
		v.Stat = 0 ;
	}

	// TSIOイベント
	TSIOEvents.resize(TSIOQUEUENUM*2-1);
	for(size_t i=0;i<TSIOQUEUENUM*2-1;i++) {
		TSIOEvents[i] = TSIOQueue[i%TSIOQUEUENUM].Ovl.hEvent ;
	}

	// 非同期FIFOバッファオブジェクト作成
	AsyncTsFifo = new CAsyncFifo(
		ASYNCTSQUEUENUM,ASYNCTSQUEUEMAX,ASYNCTSEMPTYBORDER,
		ASYNCTSPACKETSIZE,ASYNCTSFIFOTHREADWAIT,ASYNCTSFIFOTHREADPRIORITY ) ;
	AsyncTsFifo->SetEmptyLimit(ASYNCTSEMPTYLIMIT) ;
}
//---------------------------------------------------------------------------
void CBonTuner::Finalize()
{
	CloseTuner();

	if(AsyncTsFifo) {
		delete AsyncTsFifo;
		AsyncTsFifo = NULL;
	}

	for(size_t i=0;i<TSIOQueue.size();i++)
		CloseHandle(TSIOEvents[i]);

	TSIOEvents.clear();
	TSIOQueue.clear();
}
//---------------------------------------------------------------------------
void CBonTuner::LoadIni(string iniFileName)
{
  if(GetFileAttributesA(iniFileName.c_str())==-1) return ;
  const DWORD BUFFER_SIZE = 1024;
  char buffer[BUFFER_SIZE];
  ZeroMemory(buffer, BUFFER_SIZE);
  string Section;

  #define LOADSTR2(val,key) do { \
	  GetPrivateProfileStringA(Section.c_str(),key,val.c_str(),buffer,BUFFER_SIZE,iniFileName.c_str()) ; \
	  val = buffer ; \
	}while(0)
  #define LOADSTR(val) LOADSTR2(val,#val)
  #define LOADWSTR(val) do { \
	  string temp = wcs2mbcs(val) ; \
	  LOADSTR2(temp,#val) ; val = mbcs2wcs(temp) ; \
	}while(0)

  #define LOADINT2(val,key,a2i) do { \
	  GetPrivateProfileStringA(Section.c_str(),key,"",buffer,BUFFER_SIZE,iniFileName.c_str()) ; \
	  val = a2i(buffer,val) ; \
	}while(0)
  #define LOADINT(val) LOADINT2(val,#val,acalci)
  #define LOADINT64(val) LOADINT2(val,#val,acalci64)

  #define LOADSTR_SEC(sec,val) do {\
	  Section = #sec ; \
	  LOADSTR2(sec##val,#val); \
	}while(0)
  #define LOADINT_SEC(sec,val) do {\
	  Section = #sec ; \
	  LOADINT2(sec##val,#val,acalci); \
	}while(0)
  #define LOADINT64_SEC(sec,val) do {\
	  Section = #sec ; \
	  LOADINT2(sec##val,#val,acalci64); \
	}while(0)

  Section = "SET" ;
  LOADINT(Type);
  LOADINT(IPV6);

  LOADINT_SEC(TSIO,PACKETSIZE);
  LOADINT_SEC(TSIO,QUEUENUM);
  LOADINT_SEC(TSIO,POLLTIMEOUT);

  LOADINT_SEC(ASYNCTS,PACKETSIZE);
  LOADINT_SEC(ASYNCTS,QUEUENUM);
  LOADINT_SEC(ASYNCTS,QUEUEMAX);
  LOADINT_SEC(ASYNCTS,EMPTYBORDER);
  LOADINT_SEC(ASYNCTS,EMPTYLIMIT);
  //LOADINT_SEC(ASYNCTS,RECVTHREADWAIT);
  LOADINT_SEC(ASYNCTS,RECVTHREADPRIORITY);
  LOADINT_SEC(ASYNCTS,FIFOALLOCWAITING);
  LOADINT_SEC(ASYNCTS,FIFOTHREADWAIT);
  LOADINT_SEC(ASYNCTS,FIFOTHREADPRIORITY);

  #undef LOADINT64_SEC
  #undef LOADINT_SEC
  #undef LOADSTR_SEC

  #undef LOADINT64
  #undef LOADINT
  #undef LOADINT2

  #undef LOADSTR
  #undef LOADWSTR
  #undef LOADSTR2
}
//---------------------------------------------------------------------------
BOOL CBonTuner::SocOpen(int port)
{
  addrinfo hints;
  ZeroMemory(&hints, sizeof(hints)) ;

  hints.ai_flags = AI_PASSIVE;
  hints.ai_family = IPV6 ? AF_INET6 : AF_INET ;
  hints.ai_socktype = Type;
  hints.ai_protocol = Type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP ;

  string strPort = str_printf("%d", port) ;

  addrinfo *ai;
  if (getaddrinfo(NULL, strPort.c_str(), &hints, &ai) != 0) {
    DBGOUT("getaddrinfo Failed: code=%d\n", WSAGetLastError());
    return FALSE;
  }

  Soc = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
  if (Soc == INVALID_SOCKET) {
    freeaddrinfo(ai);
    DBGOUT("Sock Open Failed: code=%d\n", WSAGetLastError()) ;
    return FALSE;
  }

  if (Type == SOCK_STREAM) {
    BOOL bReuseAddr = TRUE;
    setsockopt(Soc, SOL_SOCKET, SO_REUSEADDR, (const char *)&bReuseAddr, sizeof(bReuseAddr)) ;
  }

  if (IPV6) {
    BOOL bV6Only = FALSE;
    setsockopt(Soc, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&bV6Only, sizeof(bV6Only)) ;
  }

  if (::bind(Soc, ai->ai_addr, (int)ai->ai_addrlen) == SOCKET_ERROR
      || (Type == SOCK_STREAM && listen(Soc, 1) == SOCKET_ERROR)) {
    DBGOUT("Bind Error: code=%d\n", WSAGetLastError()) ;
    return FALSE;
  }
  freeaddrinfo(ai);

  return TRUE;
}
//---------------------------------------------------------------------------
void CBonTuner::SocClose()
{
	if (TcpSoc != INVALID_SOCKET) {
		closesocket(TcpSoc);
		TcpSoc = INVALID_SOCKET;
	}
	if (Soc != INVALID_SOCKET) {
		closesocket(Soc);
		Soc = INVALID_SOCKET;
	}
}
//---------------------------------------------------------------------------
void CBonTuner::StartAsyncTsThread()
{
	if(!AsyncTsFifo||Soc==INVALID_SOCKET) return;
	auto &Thread = AsyncTsThread;
	if(Thread != INVALID_HANDLE_VALUE) return /*active*/;
	Thread = (HANDLE)_beginthreadex(NULL, 0, AsyncTsThreadProc, this,
		CREATE_SUSPENDED, NULL) ;
	if(Thread != INVALID_HANDLE_VALUE) {
		SetThreadPriority(Thread,ASYNCTSRECVTHREADPRIORITY);
		AsyncTsTerm=FALSE;
		::ResumeThread(Thread) ;
	}
}
//---------------------------------------------------------------------------
void CBonTuner::StopAsyncTsThread()
{
	if(!AsyncTsFifo||Soc==INVALID_SOCKET) return;
	auto &Thread = AsyncTsThread;
	if(Thread == INVALID_HANDLE_VALUE) return /*inactive*/;
	AsyncTsTerm=TRUE;
	if(::WaitForSingleObject(Thread,30000) != WAIT_OBJECT_0) {
		::TerminateThread(Thread, 0);
	}
	CloseHandle(Thread);
	Thread = INVALID_HANDLE_VALUE ;
}
//---------------------------------------------------------------------------
unsigned int CBonTuner::AsyncTsThreadProcMain()
{
	BOOL &terminated = AsyncTsTerm ;
	const bool alloc_waiting = ASYNCTSFIFOALLOCWAITING ? true : false ;

	event_object evAccept ;
	if (Type == SOCK_STREAM)
		WSAEventSelect(Soc, evAccept.handle(), FD_ACCEPT);

	int si = 0, ri = 0 ; // submitting index, reaping index
	int num_submit = 0 ; // number of submitting

	int tcp_stride = 0 ;  // tcp header stride
    union {
		struct {
			DWORD dummy ;
			DWORD sz ;
		};
		BYTE bin[8] ;
	} tcp_header ;

	const int STAT_BUSY = 1 ;
	const int STAT_EMPTY = 0 ;

	for (auto &v : TSIOQueue)
		v.Stat = STAT_EMPTY ;

	auto reset_queue = [&]() {
		for(auto &q : TSIOQueue) {
			if(q.Stat == STAT_BUSY) {
				WaitForSingleObject(q.Ovl.hEvent, INFINITE);
				q.Stat = STAT_EMPTY ;
			}
		}
		AsyncTsFifo->Purge();
		TsStreamEvent.set() ;
	};


	while (!terminated) {

		// waiting tcp connection
		if ( Type == SOCK_STREAM && evAccept.wait(0) == WAIT_OBJECT_0 ) {
			WSANETWORKEVENTS events;
			if (WSAEnumNetworkEvents(Soc, evAccept.handle(), &events) != SOCKET_ERROR &&
			        (events.lNetworkEvents & FD_ACCEPT)) {
				SOCKET clisoc = accept(Soc, NULL, NULL);
				if (clisoc != INVALID_SOCKET) {
					WSAEventSelect(clisoc, NULL, 0);
					if (TcpSoc != INVALID_SOCKET) {
						closesocket(clisoc);
					} else {
						TcpSoc = clisoc;
						tcp_stride = -8 ;
					}
				}
			}
		}

		auto soc = Type == SOCK_STREAM ? TcpSoc : Soc ;

		if ( soc == INVALID_SOCKET ) {
			Sleep(TSIOPOLLTIMEOUT);
			continue;
		}

		// polling
		int next_wait_index=-1 ;
		if(num_submit>0) {
			int max_wait_count = num_submit<MAXIMUM_WAIT_OBJECTS ? num_submit : MAXIMUM_WAIT_OBJECTS ;
			DWORD dRet = WaitForMultipleObjects(max_wait_count, &TSIOEvents[ri] , FALSE, TSIOPOLLTIMEOUT );
			if(WAIT_OBJECT_0 <= dRet&&dRet < WAIT_OBJECT_0+max_wait_count) {
				next_wait_index = ((dRet - WAIT_OBJECT_0)+1 + ri) % TSIOQUEUENUM ;
#ifdef STRICTLY_CHECK_EVENT_SIGNALS
				int end_index=(ri+num_submit)%TSIOQUEUENUM ;
				while( next_wait_index != end_index ) {
					if(WaitForSingleObject(TSIOEvents[next_wait_index],0)!=WAIT_OBJECT_0)
						break ;
					if (++next_wait_index >= (int)TSIOQUEUENUM)
						next_wait_index ^= next_wait_index ;
				}
#endif
			}else if(WAIT_TIMEOUT!=dRet) {
				soc=INVALID_SOCKET;
			}
		}

		// reaping
		if ( soc != INVALID_SOCKET) {
			if(next_wait_index>=0) do {
				auto &q = TSIOQueue[ri] ;
				if(q.Stat==STAT_EMPTY) break;
				DWORD rx_sz = q.RxSz ;
				if(!rx_sz) {
					DWORD Flags = 0;
					BOOL bRet = ::WSAGetOverlappedResult(soc, &q.Ovl, &rx_sz, FALSE, &Flags);
				    if(!bRet) {
						int sock_err = WSAGetLastError();
						if(sock_err == ERROR_IO_INCOMPLETE)
							break;
						else {
							soc = INVALID_SOCKET ;
							DBGOUT("WSAGetOverlappedResult failed (%d)\n",sock_err) ;
							break ;
						}
					}
					q.RxSz = rx_sz ;
				}
				if(rx_sz>0) {
					if(Type==SOCK_STREAM) {
						for(DWORD i=0;i<rx_sz;) {
							if(tcp_stride<0) {
								tcp_header.bin[tcp_stride++ + 8] = q.Buff[i++] ;
								if(!tcp_stride)
									tcp_stride = tcp_header.sz ;
							}else {
								DWORD sz = min<DWORD>(tcp_stride,rx_sz-i);
								if(AsyncTsFifo->Push(&q.Buff[i],sz,false,alloc_waiting))
									TsStreamEvent.set();
								tcp_stride-=sz;
								if(!tcp_stride)
									tcp_stride = -8 ;
								i+=sz ;
							}
						}
					}else {
						if(AsyncTsFifo->Push(q.Buff.data(),rx_sz,false,alloc_waiting))
							TsStreamEvent.set();
					}
				}
				q.Stat=STAT_EMPTY;
				if(++ri>=(int)TSIOQUEUENUM) ri^=ri ;
				if(--num_submit<=0) break;
			}while(ri!=next_wait_index);
		}

		// submitting
		if ( soc != INVALID_SOCKET) {
			while ( num_submit < (int)TSIOQUEUENUM ) {
				auto &q = TSIOQueue[si] ;
				if (q.Stat != STAT_EMPTY)
					break;
				ZeroMemory(&q.Ovl, sizeof q.Ovl) ;
				q.Ovl.hEvent = TSIOEvents[si] ;
				ResetEvent(q.Ovl.hEvent);
				DWORD Flags = 0;
				WSABUF wsaBuf;
				wsaBuf.buf = (char*)q.Buff.data() ;
				wsaBuf.len = (ULONG)q.Buff.size() ;
				q.RxSz = 0 ;
				if (SOCKET_ERROR == WSARecv(soc, &wsaBuf, 1, &q.RxSz, &Flags, &q.Ovl, NULL)) {
					int sock_err = WSAGetLastError();
					if (sock_err != ERROR_IO_PENDING) {
						soc = INVALID_SOCKET ;
						DBGOUT("WSARecv failed (%d)\n",sock_err) ;
						break ;
					}
				}
				q.Stat = STAT_BUSY ;
				si = ( si + 1 >= (int)TSIOQUEUENUM ) ? 0 : si + 1 ;
				num_submit++;
			}
		}

		// socket error
		if ( soc == INVALID_SOCKET) {
			if (Type == SOCK_STREAM) {
				closesocket(TcpSoc);
				TcpSoc = INVALID_SOCKET;
				reset_queue();
			} else
				break ;
		}

	}

	SocClose();
	reset_queue();

	return 0 ;
}
//---------------------------------------------------------------------------
unsigned int __stdcall CBonTuner::AsyncTsThreadProc (PVOID pv)
{
	unsigned int res = static_cast<CBonTuner*>(pv)->AsyncTsThreadProcMain() ;
	_endthreadex(res) ;
	return res;
}
//---------------------------------------------------------------------------
const BOOL CBonTuner::OpenTuner(void)
{
	CloseTuner();
	if(!TunerOpened) {
		if(!WinSockInitialize())
			return FALSE ;
		TunerOpened=TRUE;
		return TRUE;
	}
	return FALSE;
}
//---------------------------------------------------------------------------
void CBonTuner::CloseTuner(void)
{
	if(TunerOpened) {
		StopAsyncTsThread();
		SocClose();
		WinSockFinalize();
		TunerOpened=FALSE;
	}
}
//---------------------------------------------------------------------------
const BOOL CBonTuner::SetChannel(const BYTE bCh)
{
	return bCh<13 ? FALSE : SetChannel(0, bCh-13) ;
}
//---------------------------------------------------------------------------
const float CBonTuner::GetSignalLevel(void)
{
	return 0.f ;
}
//---------------------------------------------------------------------------
const DWORD CBonTuner::WaitTsStream(const DWORD dwTimeOut)
{
	if(!AsyncTsFifo) return WAIT_ABANDONED;

	if(!AsyncTsFifo->Empty()) return WAIT_OBJECT_0;
    if(AsyncTsThread==INVALID_HANDLE_VALUE) return WAIT_ABANDONED;

	const DWORD dwRet = TsStreamEvent.wait(dwTimeOut);

	switch(dwRet){
	case WAIT_ABANDONED:
		return WAIT_ABANDONED;

	case WAIT_OBJECT_0:
	case WAIT_TIMEOUT:
		return dwRet;
	}

	return WAIT_FAILED;
}
//---------------------------------------------------------------------------
const DWORD CBonTuner::GetReadyCount(void)
{
	if(!AsyncTsFifo) return 0;
	if(AsyncTsFifo->Size()<=AsyncTsCurStart) return 0;

	return (DWORD)(AsyncTsFifo->Size()-AsyncTsCurStart);
}
//---------------------------------------------------------------------------
const BOOL CBonTuner::GetTsStream(BYTE *pDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if(!AsyncTsFifo) return FALSE;

	BYTE *pSrc = NULL;

	if(GetTsStream(&pSrc, pdwSize, pdwRemain)){
		if(*pdwSize&&pSrc)
			CopyMemory(pDst, pSrc, *pdwSize);
		return TRUE;
	}

	return FALSE;
}
//---------------------------------------------------------------------------
const BOOL CBonTuner::GetTsStream(BYTE **ppDst, DWORD *pdwSize, DWORD *pdwRemain)
{
	if (!AsyncTsFifo)
		return FALSE;

	if (AsyncTsFifo->Size() > AsyncTsCurStart) {
		if (AsyncTsFifo->Pop(ppDst, pdwSize, pdwRemain)) {
			if (AsyncTsCurStart > 0) {
				if (pdwRemain) {
					if (AsyncTsFifo->Size() > AsyncTsCurStart)
						*pdwRemain = (DWORD)AsyncTsFifo->Size() - AsyncTsCurStart;
					else
						*pdwRemain = 0 ;
				}
				AsyncTsCurStart--;
			}
			return TRUE ;
		}
	}

	if (ppDst) *ppDst = NULL ;
	if (pdwSize) *pdwSize = 0 ;
	if (pdwRemain) *pdwRemain = 0 ;
	return FALSE;
}
//---------------------------------------------------------------------------
void CBonTuner::PurgeTsStream(void)
{
	if(AsyncTsFifo) AsyncTsFifo->Purge() ;
	AsyncTsCurStart = ASYNCTSQUEUESTART ;
}
//---------------------------------------------------------------------------
LPCTSTR CBonTuner::GetTunerName(void)
{
	if(Type == SOCK_STREAM) return L"TCP";
	else if(Type == SOCK_DGRAM) return L"UDP";
	return L"(none)" ;
}
//---------------------------------------------------------------------------
const BOOL CBonTuner::IsTunerOpening(void)
{
	return TunerOpened;
}
//---------------------------------------------------------------------------
LPCTSTR CBonTuner::EnumTuningSpace(const DWORD dwSpace)
{
	if(dwSpace==0) return GetTunerName() ;
	return NULL;
}
//---------------------------------------------------------------------------
LPCTSTR CBonTuner::EnumChannelName(const DWORD dwSpace, const DWORD dwChannel)
{
	if(dwSpace==0) {
		if(dwChannel<DWORD(Ports.size())) {
			return Ports[dwChannel].second.c_str();
		}
	}
	return NULL;
}
//---------------------------------------------------------------------------
const BOOL CBonTuner::SetChannel(const DWORD dwSpace, const DWORD dwChannel)
{
	if(dwSpace!=0) return FALSE ;
	if(dwChannel>=Ports.size()) return FALSE ;
	if(CurChannel==dwChannel) return TRUE ;

	StopAsyncTsThread();
	SocClose() ;
	CurChannel = 0xFFFFFFFF ;

	if(!SocOpen(Ports[dwChannel].first)) {
		SocClose();
		return FALSE ;
	}

	PurgeTsStream();
	CurChannel = dwChannel ;
	StartAsyncTsThread();

	return TRUE ;
}
//---------------------------------------------------------------------------
const DWORD CBonTuner::GetCurSpace(void)
{
	return 0;
}
//---------------------------------------------------------------------------
const DWORD CBonTuner::GetCurChannel(void)
{
	return CurChannel;
}
//===========================================================================

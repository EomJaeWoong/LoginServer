#include "stdafx.h"

//-----------------------------------------------------------------------------------------
// ������, �Ҹ���
//-----------------------------------------------------------------------------------------
CLanServer::CLanServer()
{
	///////////////////////////////////////////////////////////////////////////////////////
	// ���� �ʱ�ȭ
	///////////////////////////////////////////////////////////////////////////////////////
	CCrashDump::CCrashDump();

	///////////////////////////////////////////////////////////////////////////////////////
	// �������Ϸ� �ʱ�ȭ
	///////////////////////////////////////////////////////////////////////////////////////
	ProfileInit();

	///////////////////////////////////////////////////////////////////////////////////////
	// ��Ʈ��ũ ��Ŷ ���� üũ
	///////////////////////////////////////////////////////////////////////////////////////
	if (!CNPacket::_ValueSizeCheck())
		CCrashDump::Crash();

	///////////////////////////////////////////////////////////////////////////////////////
	// �� ���� �ε��� ���� ����
	///////////////////////////////////////////////////////////////////////////////////////
	_pBlankStack = new CLockfreeStack<int>();

	///////////////////////////////////////////////////////////////////////////////////////
	// �� ���� ����
	///////////////////////////////////////////////////////////////////////////////////////
	for (int iCnt = eMAX_SESSION - 1; iCnt >= 0; iCnt--)
	{
		_Session[iCnt] = new SESSION;
		///////////////////////////////////////////////////////////////////////////////////
		// ���� ���� ����ü �ʱ�ȭ
		///////////////////////////////////////////////////////////////////////////////////
		_Session[iCnt]->_SessionInfo._Socket = INVALID_SOCKET;
		memset(&_Session[iCnt]->_SessionInfo._wIP, 0, sizeof(_Session[iCnt]->_SessionInfo._wIP));
		_Session[iCnt]->_SessionInfo._iPort = 0;

		///////////////////////////////////////////////////////////////////////////////////
		// ���� �ʱ�ȭ
		///////////////////////////////////////////////////////////////////////////////////
		_Session[iCnt]->_iSessionID = -1;

		memset(&_Session[iCnt]->_SendOverlapped, 0, sizeof(OVERLAPPED));
		memset(&_Session[iCnt]->_RecvOverlapped, 0, sizeof(OVERLAPPED));

		_Session[iCnt]->_RecvQ.ClearBuffer();
		_Session[iCnt]->_SendQ.ClearBuffer();

		_Session[iCnt]->_bSendFlag = false;
		_Session[iCnt]->_bSendFlagWorker = false;

		_Session[iCnt]->_IOBlock = (IOBlock *)_aligned_malloc(sizeof(IOBlock), 16);

		_Session[iCnt]->_IOBlock->_iIOCount = 0;
		_Session[iCnt]->_IOBlock->_iReleaseFlag = false;

		memset(_Session[iCnt]->_pSentPacket, 0, sizeof(_Session[iCnt]->_pSentPacket));
		_Session[iCnt]->_lSentPacketCnt = 0;

		InsertBlankSessionIndex(iCnt);
	}

	
	///////////////////////////////////////////////////////////////////////////////////////
	// LanServer ���� ����
	///////////////////////////////////////////////////////////////////////////////////////
	_iSessionID = 0;


	///////////////////////////////////////////////////////////////////////////////////////
	// ���� �ʱ�ȭ
	///////////////////////////////////////////////////////////////////////////////////////
	WSADATA wsa;
	if (0 != WSAStartup(MAKEWORD(2, 2), &wsa))
		return;


	///////////////////////////////////////////////////////////////////////////////////////
	// ����͸� ������
	///////////////////////////////////////////////////////////////////////////////////////
	DWORD dwThreadID;
	_hMonitorThread = (HANDLE)_beginthreadex(
		NULL,
		0,
		MonitorThread,
		this,
		0,
		(unsigned int *)&dwThreadID
		);
}

CLanServer::~CLanServer()
{
	CloseHandle(_hIOCP);

	for (int iCnt = 0; iCnt < eMAX_SESSION; iCnt++)
		delete _Session[iCnt];

	delete _pBlankStack;
}


//-----------------------------------------------------------------------------------------
// ���� ����
//-----------------------------------------------------------------------------------------
bool				CLanServer::Start(WCHAR* wOpenIP, int iPort, int iWorkerThreadNum, bool bNagle, int iMaxConnect)
{
	int				result;;

	///////////////////////////////////////////////////////////////////////////////////////
	// �������� ����
	///////////////////////////////////////////////////////////////////////////////////////
	_ListenSocket = socket(AF_INET, SOCK_STREAM, 0);
	if (INVALID_SOCKET == _ListenSocket)
		return false;

	///////////////////////////////////////////////////////////////////////////////////////
	// bind
	///////////////////////////////////////////////////////////////////////////////////////
	SOCKADDR_IN serverAddr;
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons(iPort);
	InetPton(AF_INET, wOpenIP, &serverAddr.sin_addr);
	result = bind(_ListenSocket, (SOCKADDR *)&serverAddr, sizeof(SOCKADDR_IN));
	if (SOCKET_ERROR == result)
		return false;

	///////////////////////////////////////////////////////////////////////////////////////
	// ���� Send Buffer ������ 0���� ����
	///////////////////////////////////////////////////////////////////////////////////////
	int optval;
	setsockopt(_ListenSocket, SOL_SOCKET, SO_SNDBUF, (char*)&optval, 0);
		
	///////////////////////////////////////////////////////////////////////////////////////
	// listen
	///////////////////////////////////////////////////////////////////////////////////////
	result = listen(_ListenSocket, SOMAXCONN);
	if (SOCKET_ERROR == result)
		return false;

	///////////////////////////////////////////////////////////////////////////////////////
	// nagle option
	///////////////////////////////////////////////////////////////////////////////////////
	_bNagle = bNagle;

	///////////////////////////////////////////////////////////////////////////////////////
	// IO Completion Port ����
	///////////////////////////////////////////////////////////////////////////////////////
	_hIOCP = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (NULL == _hIOCP)
		return false;

	///////////////////////////////////////////////////////////////////////////////////////
	// Thread ����
	///////////////////////////////////////////////////////////////////////////////////////
	DWORD dwThreadID;

	_hAcceptThread = (HANDLE)_beginthreadex(
		NULL,
		0,
		AcceptThread,
		this,
		0,
		(unsigned int *)&dwThreadID
		);

	if (iWorkerThreadNum > eMAX_THREAD)
		return false;

	_iWorkerThreadNum = iWorkerThreadNum;

	for (int iCnt = 0; iCnt < iWorkerThreadNum; iCnt++)
	{
		_hWorkerThread[iCnt] = (HANDLE)_beginthreadex(
			NULL,
			0,
			WorkerThread,
			this,
			0,
			(unsigned int *)&dwThreadID
			);
	}

	return true;
}


///////////////////////////////////////////////////////////////////////////////////////////
// ���� ����
///////////////////////////////////////////////////////////////////////////////////////////
void				CLanServer::Stop()
{
	///////////////////////////////////////////////////////////////////////////////////////
	// ���� ���� �ݱ�
	///////////////////////////////////////////////////////////////////////////////////////
	closesocket(_ListenSocket);

	///////////////////////////////////////////////////////////////////////////////////////
	// accpet Thread�� ����Ǹ�
	// ��� ������ �����Ű�� �ٸ� �����嵵 �����Ѵ�
	///////////////////////////////////////////////////////////////////////////////////////
	if (WAIT_OBJECT_0 == WaitForSingleObject(_hAcceptThread, INFINITE))
	{
		CloseHandle(_hAcceptThread);
		for (int iCnt = 0; iCnt < eMAX_SESSION; iCnt++)
			DisconnectSession(_Session[iCnt]);
	}

	///////////////////////////////////////////////////////////////////////////////////////
	// session�� 0�� �ɶ����� ��ٸ�
	///////////////////////////////////////////////////////////////////////////////////////
	while (GetSessionCount());

	///////////////////////////////////////////////////////////////////////////////////////
	// Worker Thread�鿡�� ���� status ����
	///////////////////////////////////////////////////////////////////////////////////////
	PostQueuedCompletionStatus(_hIOCP, 0, NULL, NULL);

	CloseHandle(_hIOCP);
	for (int iCnt = 0; iCnt < _iWorkerThreadNum; iCnt++)
		CloseHandle(_hWorkerThread[iCnt]);
}


///////////////////////////////////////////////////////////////////////////////////////////
// ���� ����
///////////////////////////////////////////////////////////////////////////////////////////
bool				CLanServer::SendPacket(__int64 iSessionID, CNPacket *pPacket)
{
	SESSION		*pSession = SessionGetLock(iSessionID);

	if (nullptr != pSession)
	{
		pPacket->SetCustomShortHeader(pPacket->GetDataSize());

		PRO_BEGIN(L"Packet addref");
		pPacket->addRef();
		PRO_END(L"Packet addref");

		while (true == InterlockedCompareExchange((LONG *)&pSession->_bSendFlagWorker, true, true));
		
		PRO_BEGIN(L"PacketQueue Put");
		pSession->_SendQ.Put(pPacket);
		PRO_END(L"PacketQueue Put");

		PRO_BEGIN(L"SendPost");
		SendPost(pSession);
		PRO_END(L"SendPost");
		
		InterlockedIncrement((LONG *)&_lSendPacketCounter);
	}

	SessionGetUnlock(pSession);

	return true;
}


///////////////////////////////////////////////////////////////////////////////////////////
// ���� ����
///////////////////////////////////////////////////////////////////////////////////////////
bool				CLanServer::Disconnect(__int64 iSessionID)
{
	int			iSessionIndex = GET_SESSIONINDEX(iSessionID);
	
	DisconnectSession(_Session[iSessionIndex]);
}

///////////////////////////////////////////////////////////////////////////////////////////
// ���� �����ϴ� ������ �κ�
///////////////////////////////////////////////////////////////////////////////////////////
int					CLanServer::AccpetThread_update()
{
	HANDLE			hResult;
	int				iErrorCode;

	SOCKET			ClientSocket;
	SOCKADDR_IN		ClientAddr;
	int				iAddrLen = sizeof(SOCKADDR_IN);
	

	SESSIONINFO		SessionInfo;

	int				iBlankIndex;

	while (1)
	{
		///////////////////////////////////////////////////////////////////////////////////
		// accpet
		///////////////////////////////////////////////////////////////////////////////////
		ClientSocket = accept(_ListenSocket, (SOCKADDR *)&ClientAddr, &iAddrLen);
		if (INVALID_SOCKET == ClientSocket)
		{
			iErrorCode = WSAGetLastError();
			if ((WSAENOTSOCK == iErrorCode) ||
				(WSAEINTR == iErrorCode))
				break;
		}

		InterlockedIncrement((LONG *)&_lAcceptCounter);
		InterlockedIncrement((LONG *)&_lAcceptTotalCounter);

		///////////////////////////////////////////////////////////////////////////////////
		// ���� ���� ���� ����
		///////////////////////////////////////////////////////////////////////////////////
		SessionInfo._Socket = ClientSocket;
		InetNtop(AF_INET, &ClientAddr.sin_addr, SessionInfo._wIP, 16);
		SessionInfo._iPort = ntohs(ClientAddr.sin_port);

		iBlankIndex = GetBlankSessionIndex();

		///////////////////////////////////////////////////////////////////////////////////
		// �ִ� ���� �ʰ���
		///////////////////////////////////////////////////////////////////////////////////
		if (iBlankIndex < 0)
		{
			closesocket(ClientSocket);
			continue;
		}

		///////////////////////////////////////////////////////////////////////////////////
		// ���� ��û(White IP�� �����ϰ� �ϱ� ��)
		///////////////////////////////////////////////////////////////////////////////////
		if (!OnConnectionRequest(&SessionInfo))
			continue;
		
		///////////////////////////////////////////////////////////////////////////////////
		// keepalive �ɼ�
		//
		// onoff			 -> keepalive ���� ����(0�� �ƴϸ� ����)
		// keepalivetime	 -> ù keepalive ��Ŷ�� ���� �������� �ð�
		// keepaliveinterval -> ������ ������� ���� keepalive ��Ŷ�� ���۵Ǵ� ���� 
		///////////////////////////////////////////////////////////////////////////////////
		tcp_keepalive tcpkl;

		tcpkl.onoff = 1;
		tcpkl.keepalivetime = 30000;
		tcpkl.keepaliveinterval = 2000;

		DWORD dwReturnByte;
		WSAIoctl(SessionInfo._Socket, SIO_KEEPALIVE_VALS, &tcpkl, sizeof(tcp_keepalive),
			0, 0, &dwReturnByte, NULL, NULL);

		///////////////////////////////////////////////////////////////////////////////////
		// nagle �ɼ�
		///////////////////////////////////////////////////////////////////////////////////
		if (_bNagle)
		{
			char opt_val = true;
			setsockopt(SessionInfo._Socket, IPPROTO_TCP, TCP_NODELAY, &opt_val, sizeof(opt_val));
		}

		///////////////////////////////////////////////////////////////////////////////////
		// �� ���� ã�� ���� ����
		///////////////////////////////////////////////////////////////////////////////////
		_Session[iBlankIndex]->_SessionInfo = SessionInfo;

		///////////////////////////////////////////////////////////////////////////////////
		// ���� ID �����ؼ� �����
		///////////////////////////////////////////////////////////////////////////////////
		__int64 iSessionID = InterlockedIncrement64((LONG64 *)&_iSessionID);
		_Session[iBlankIndex]->_iSessionID = COMBINE_ID_WITH_INDEX(iSessionID, iBlankIndex);

		/////////////////////////////////////////////////////////////////////
		// IOCP ���
		/////////////////////////////////////////////////////////////////////
		hResult = CreateIoCompletionPort((HANDLE)_Session[iBlankIndex]->_SessionInfo._Socket,
			_hIOCP,
			(ULONG_PTR)_Session[iBlankIndex],
			0);
		if (!hResult)
			PostQueuedCompletionStatus(_hIOCP, 0, 0, 0);

		///////////////////////////////////////////////////////////////////////////////////
		// OnClientJoin
		// �������ʿ� ������ �������� �˸�
		// �α��� ��Ŷ ������ �߿� ���� �� ������ IOCount�� �̸� �÷��ش�
		///////////////////////////////////////////////////////////////////////////////////
		InterlockedIncrement64((LONG64 *)&_Session[iBlankIndex]->_IOBlock->_iIOCount);
		OnClientJoin(&_Session[iBlankIndex]->_SessionInfo, _Session[iBlankIndex]->_iSessionID);

		InterlockedIncrement((long *)&_lSessionCount);

		PRO_BEGIN(L"RecvPost - AccpetTH");
		RecvPost(_Session[iBlankIndex], true);
		PRO_END(L"RecvPost - AccpetTH");
	}

	return 0;
}

int					CLanServer::WorkerThread_update()
{
	int				result;

	OVERLAPPED		*pOverlapped;
	SESSION			*pSession;
	DWORD			dwTransferred;

	while (1)
	{
		pOverlapped		= NULL;
		pSession		= NULL;
		dwTransferred	= 0;

		PRO_BEGIN(L"GQCS IOComplete");
		result = GetQueuedCompletionStatus(
			_hIOCP,
			&dwTransferred,
			(PULONG_PTR)&pSession,
			&pOverlapped,
			INFINITE);
		PRO_END(L"GQCS IOComplete");

		OnWorkerThreadBegin();

		///////////////////////////////////////////////////////////////////////////////////
		// Error, ���� ó��
		///////////////////////////////////////////////////////////////////////////////////
		// IOCP ���� ���� ����
		if (result == FALSE && (pOverlapped == NULL || pSession == NULL))
		{
			int iErrorCode = WSAGetLastError();
			OnError(iErrorCode, L"IOCP HANDLE Error\n");

			break;
		}

		// ��Ŀ������ ���� ����
		else if (dwTransferred == 0 && pSession == NULL && pOverlapped == NULL)
		{
			OnError(0, L"Worker Thread Done.\n");
			PostQueuedCompletionStatus(_hIOCP, 0, NULL, NULL);
			return 0;
		}

		//----------------------------------------------------------------------------
		// ��������
		// Ŭ���̾�Ʈ ���� closesocket() Ȥ�� shutdown() �Լ��� ȣ���� ����
		//----------------------------------------------------------------------------
		else if (dwTransferred == 0)
		{
			DisconnectSession(pSession);
		}
		//----------------------------------------------------------------------------

		if (pOverlapped == &(pSession->_RecvOverlapped))
		{
			PRO_BEGIN(L"CompleteRecv");
			CompleteRecv(pSession, dwTransferred);
			PRO_END(L"CompleteRecv");
		}

		if (pOverlapped == &(pSession->_SendOverlapped))
		{
			PRO_BEGIN(L"CompleteSend");
			CompleteSend(pSession, dwTransferred);
			PRO_END(L"CompleteSend");
		}

		if (0 == InterlockedDecrement64((LONG64 *)&pSession->_IOBlock->_iIOCount))
			ReleaseSession(pSession);

		OnWorkerThreadEnd();
	}

	return true;
}

int					CLanServer::MonitorThread_update()
{
	timeBeginPeriod(1);

	while (1)
	{
		_lAcceptTPS			= _lAcceptCounter;
		_lAcceptTotalTPS	+= _lAcceptTotalCounter;
		_lRecvPacketTPS		= _lRecvPacketCounter;
		_lSendPacketTPS		= _lSendPacketCounter;
		_lPacketPoolTPS		= CNPacket::GetPacketCount();

		_lAcceptCounter		= 0;
		_lAcceptTotalCounter = 0;
		_lRecvPacketCounter = 0;
		_lSendPacketCounter = 0;

		Sleep(999);
	}

	timeEndPeriod(1);

	return 0;
}



///////////////////////////////////////////////////////////////////////////////////////////
// Recv, Send ���
///////////////////////////////////////////////////////////////////////////////////////////
void				CLanServer::RecvPost(SESSION *pSession, bool bAcceptRecv)
{
	int result, iCount = 1;
	DWORD dwRecvSize, dwFlag = 0;
	WSABUF wBuf[2];

	///////////////////////////////////////////////////////////////////////////////////////
	// WSABUF ���
	///////////////////////////////////////////////////////////////////////////////////////
	wBuf[0].buf = pSession->_RecvQ.GetWriteBufferPtr();
	wBuf[0].len = pSession->_RecvQ.GetNotBrokenPutSize();

	///////////////////////////////////////////////////////////////////////////////////////
	// ������ �������� ��� ���� ���� ���
	///////////////////////////////////////////////////////////////////////////////////////
	if (pSession->_RecvQ.GetFreeSize() > pSession->_RecvQ.GetNotBrokenPutSize())
	{
		wBuf[1].buf = pSession->_RecvQ.GetBufferPtr();
		wBuf[1].len = pSession->_RecvQ.GetFreeSize() -
						pSession->_RecvQ.GetNotBrokenPutSize();

		iCount++;
	}
	
	memset(&pSession->_RecvOverlapped, 0, sizeof(OVERLAPPED));

	///////////////////////////////////////////////////////////////////////////////////////
	// ù ���ӽ��� Recv�� IOCount�� �ø��� ����(�α��� ��Ŷ ����)
	///////////////////////////////////////////////////////////////////////////////////////
	if (!bAcceptRecv)
		InterlockedIncrement64((LONG64 *)&pSession->_IOBlock->_iIOCount);

	PRO_BEGIN(L"WSARecv");
	result = WSARecv(
		pSession->_SessionInfo._Socket, 
		wBuf,
		iCount, 
		&dwRecvSize,
		&dwFlag,
		&pSession->_RecvOverlapped, 
		NULL
		);

	if (result == SOCKET_ERROR)
	{
		int iErrorCode = GetLastError();
		///////////////////////////////////////////////////////////////////////////////////
		// WSA_IO_PENDING -> Overlapped ������ �غ�Ǿ����� �Ϸ���� ���� ���
		// �� �ܿ��� ������ ��
		///////////////////////////////////////////////////////////////////////////////////
		if (iErrorCode != WSA_IO_PENDING)
		{
			///////////////////////////////////////////////////////////////////////////////
			// WSAENOBUFS(10055)
			// �ý��ۿ� ���� ������ �����ϰų� ť�� ���� ���� ���� �۾��� ������ �� ����
			///////////////////////////////////////////////////////////////////////////////
			// �ý��� �α� �����
			if (WSAENOBUFS == iErrorCode)
				CCrashDump::Crash();

			///////////////////////////////////////////////////////////////////////////////
			// WSAECONNABORTED(10053) : Ÿ�Ӿƿ��̳� �ٸ� ���� ��Ȳ���� ���� ������ �����Ǿ���
			// WSAECONNRESET(10054) : Ŭ���̾�Ʈ �ʿ��� ������ ������ ���
			// WSAESHUTDOWN(10058) : �ش� ������ shutdown�� ���
			///////////////////////////////////////////////////////////////////////////////
			if ((WSAECONNABORTED != iErrorCode) &&
				(WSAECONNRESET != iErrorCode) &&
				(WSAESHUTDOWN != iErrorCode))
				CCrashDump::Crash();

			if (0 == InterlockedDecrement64((LONG64 *)&pSession->_IOBlock->_iIOCount))
				ReleaseSession(pSession);
		}
	}
	PRO_END(L"WSARecv");
}

bool				CLanServer::SendPost(SESSION *pSession, bool bWorker)
{
	int		result, iCount = 0;
	DWORD	dwSendSize, dwFlag = 0;
	WSABUF	wBuf[eMAX_WSABUF];

	do
	{
		///////////////////////////////////////////////////////////////////////////////////
		// SendFlag Ȯ�� �� ����
		///////////////////////////////////////////////////////////////////////////////////	
		if (true == InterlockedCompareExchange((LONG *)&pSession->_bSendFlag, true, false))
			break;

		if (bWorker)
			InterlockedExchange((LONG *)&pSession->_bSendFlagWorker, true);

		///////////////////////////////////////////////////////////////////////////////////
		// SendQ ������ ����
		///////////////////////////////////////////////////////////////////////////////////
		int iSendQUseSize = pSession->_SendQ.GetUseSize();

		///////////////////////////////////////////////////////////////////////////////////
		// SendQ������ �ٽ� Ȯ��
		///////////////////////////////////////////////////////////////////////////////////
		if (0 == iSendQUseSize)
		{
			InterlockedExchange((LONG *)&pSession->_bSendFlag, false);
			if (bWorker)
				InterlockedExchange((LONG *)&pSession->_bSendFlagWorker, false);
			
			///////////////////////////////////////////////////////////////////////////////
			// SendQ�� ����� ������(���� ������ �ٸ� ��)
			///////////////////////////////////////////////////////////////////////////////
			if (!pSession->_SendQ.isEmpty())
				continue;

			break;
		}

		if (eMAX_WSABUF <= iCount)
			iSendQUseSize = eMAX_WSABUF;
		
		///////////////////////////////////////////////////////////////////////////////////
		// WSABUF�� ��Ŷ �ֱ�
		///////////////////////////////////////////////////////////////////////////////////
		CNPacket *pPacket = nullptr;
		while (0 != pSession->_SendQ.GetUseSize())
		{
			if (!pSession->_SendQ.Get(&pPacket))
				break;

			wBuf[iCount].buf = (char *)pPacket->GetBufferHeaderPtr();
			wBuf[iCount].len = pPacket->GetDataSize();

			pSession->_pSentPacket[iCount] = (char *)pPacket;

			iCount++;
		}

		pSession->_lSentPacketCnt += iCount;

		memset(&pSession->_SendOverlapped, 0, sizeof(OVERLAPPED));

		InterlockedIncrement64((LONG64 *)&pSession->_IOBlock->_iIOCount);

		PRO_BEGIN(L"WSASend");
		result = WSASend(
			pSession->_SessionInfo._Socket,
			wBuf,
			iCount,
			&dwSendSize,
			dwFlag,
			&pSession->_SendOverlapped,
			NULL
			);

		if (result == SOCKET_ERROR)
		{
			int iErrorCode = WSAGetLastError();
			///////////////////////////////////////////////////////////////////////////////////
			// WSA_IO_PENDING -> Overlapped ������ �غ�Ǿ����� �Ϸ���� ���� ���
			// �� �ܿ��� ������ ��
			///////////////////////////////////////////////////////////////////////////////////
			if (iErrorCode != WSA_IO_PENDING)
			{
				///////////////////////////////////////////////////////////////////////////////
				// WSAENOBUFS(10055)
				// �ý��ۿ� ���� ������ �����ϰų� ť�� ���� ���� ���� �۾��� ������ �� ����
				///////////////////////////////////////////////////////////////////////////////
				// �ý��� �α� �����
				if (WSAENOBUFS == iErrorCode)
					CCrashDump::Crash();
				
				///////////////////////////////////////////////////////////////////////////////
				// WSAECONNABORTED(10053) : Ÿ�Ӿƿ��̳� �ٸ� ���� ��Ȳ���� ���� ������ �����Ǿ���
				// WSAECONNRESET(10054) : Ŭ���̾�Ʈ �ʿ��� ������ ������ ���
				// WSAESHUTDOWN(10058) : �ش� ������ shutdown�� ���
				///////////////////////////////////////////////////////////////////////////////
				if ((WSAECONNABORTED != iErrorCode) &&
					(WSAECONNRESET != iErrorCode) &&
					(WSAESHUTDOWN != iErrorCode))
					CCrashDump::Crash();

				if (0 == InterlockedDecrement64((LONG64 *)&pSession->_IOBlock->_iIOCount))
					ReleaseSession(pSession);
			}
		}
		PRO_END(L"WSASend");
	} while (0);

	return true;
}



///////////////////////////////////////////////////////////////////////////////////////////
// Recv, Send ó��
///////////////////////////////////////////////////////////////////////////////////////////
bool				CLanServer::CompleteRecv(SESSION *pSession, DWORD dwTransferred)
{
	short header;

	//////////////////////////////////////////////////////////////////////////////
	// RecvQ WritePos �̵�(���� ��ŭ)
	//////////////////////////////////////////////////////////////////////////////
	if (dwTransferred != pSession->_RecvQ.MoveWritePos(dwTransferred))
		CCrashDump::Crash();

	PRO_BEGIN(L"Packet Alloc");
	CNPacket *pPacket = CNPacket::Alloc();
	PRO_END(L"Packet Alloc");

	while (pSession->_RecvQ.GetUseSize() > 0)
	{
		PRO_BEGIN(L"Recv BufferDeque");
		//////////////////////////////////////////////////////////////////////////
		// RecvQ�� ��� ���̸�ŭ �ִ��� �˻� �� ������ Peek
		//////////////////////////////////////////////////////////////////////////
		if (pSession->_RecvQ.GetUseSize() <= sizeof(header))
			break;
		pSession->_RecvQ.Peek((char *)&header, sizeof(header));

		//////////////////////////////////////////////////////////////////////////
		// RecvQ�� ��� ���� + Payload ��ŭ �ִ��� �˻� �� ��� ����
		//////////////////////////////////////////////////////////////////////////
		if (pSession->_RecvQ.GetUseSize() < sizeof(header) + header)
			break;;
		pSession->_RecvQ.RemoveData(sizeof(header));

		//////////////////////////////////////////////////////////////////////////
		// Payload�� ���� �� ��Ŷ Ŭ������ ����
		//////////////////////////////////////////////////////////////////////////
		pPacket->PutData((unsigned char *)pSession->_RecvQ.GetReadBufferPtr(), header);
		pSession->_RecvQ.RemoveData(header);
		PRO_END(L"Recv BufferDeque");

		//////////////////////////////////////////////////////////////////////////
		// OnRecv ȣ��
		//////////////////////////////////////////////////////////////////////////
		PRO_BEGIN(L"OnRecv");
		OnRecv(pSession->_iSessionID, pPacket);
		PRO_END(L"OnRecv");

		InterlockedIncrement((LONG *)&_lRecvPacketCounter);
	}

	PRO_BEGIN(L"Packet Free");
	pPacket->Free();
	PRO_END(L"Packet Free");

	PRO_BEGIN(L"RecvPost");
	RecvPost(pSession);
	PRO_END(L"RecvPost");

	return true;
}

bool				CLanServer::CompleteSend(SESSION *pSession, DWORD dwTransferred)
{
	int			iSentCnt;

	//////////////////////////////////////////////////////////////////////////////
	// ������ �Ϸ�� ������ ����
	//////////////////////////////////////////////////////////////////////////////
	PRO_BEGIN(L"SentPacket Remove");
	for (iSentCnt = 0; iSentCnt < pSession->_lSentPacketCnt; iSentCnt++)
		((CNPacket *)pSession->_pSentPacket[iSentCnt])->Free();

	pSession->_lSentPacketCnt -= iSentCnt;
	PRO_END(L"SentPacket Remove");

	//////////////////////////////////////////////////////////////////////////////
	// �� ���´ٰ� Flag ��ȯ
	//////////////////////////////////////////////////////////////////////////////	
	InterlockedExchange((LONG *)&pSession->_bSendFlag, false);
	
	PRO_BEGIN(L"SendPost - WorkerTh");
	//////////////////////////////////////////////////////////////////////////////
	// ������ ���������� �ٽ� ���
	//////////////////////////////////////////////////////////////////////////////
	if (!pSession->_SendQ.isEmpty())
	{
		SendPost(pSession, true);
		InterlockedExchange((LONG *)&pSession->_bSendFlagWorker, false);
	}
	PRO_END(L"SendPost - WorkerTh");

	return true;
}


///////////////////////////////////////////////////////////////////////////////////////////
// ���� ����ȭ		 ->		Disconnect, SendPacket
///////////////////////////////////////////////////////////////////////////////////////////
SESSION*			CLanServer::SessionGetLock(__int64 iSessionID)
{
	int iSessionIndex = GET_SESSIONINDEX(iSessionID);

	///////////////////////////////////////////////////////////////////////////////////////
	// ������ ���� �ʰ� Ȯ��
	///////////////////////////////////////////////////////////////////////////////////////
	if (1 == InterlockedIncrement64((LONG64 *)&_Session[iSessionIndex]->_IOBlock->_iIOCount))
	{
		if (0 == InterlockedDecrement64((LONG64 *)&_Session[iSessionIndex]->_IOBlock->_iIOCount))
			ReleaseSession(_Session[iSessionIndex]);

		return nullptr;
	}

	//////////////////////////////////////////////////////////////////////////////////////
	// ������ �ٲ������ �ٽ� Ȯ��
	//////////////////////////////////////////////////////////////////////////////////////
	if (iSessionID != _Session[iSessionIndex]->_iSessionID)
	{
		if (0 == InterlockedDecrement64((LONG64 *)&_Session[iSessionIndex]->_IOBlock->_iIOCount))
			ReleaseSession(_Session[iSessionIndex]);

		return nullptr;
	}

	return _Session[iSessionIndex];
}

void				CLanServer::SessionGetUnlock(SESSION *pSession)
{
	///////////////////////////////////////////////////////////////////////////////////////
	// �ٽ� �۾� ī��Ʈ ������
	///////////////////////////////////////////////////////////////////////////////////////
	if (0 == InterlockedDecrement64((LONG64 *)&pSession->_IOBlock->_iIOCount))
		ReleaseSession(pSession);
}



///////////////////////////////////////////////////////////////////////////////////////////
// �� ���� ���
///////////////////////////////////////////////////////////////////////////////////////////
int					CLanServer::GetBlankSessionIndex()
{
	int iBlankIndex;

	if (_pBlankStack->isEmpty())
		iBlankIndex = -1;
	else
		_pBlankStack->Pop(&iBlankIndex);

	return iBlankIndex;
}

///////////////////////////////////////////////////////////////////////////////////////////
// ���� �ݳ�
///////////////////////////////////////////////////////////////////////////////////////////
void				CLanServer::InsertBlankSessionIndex(int iSessionIndex)
{
	_pBlankStack->Push(iSessionIndex);
}


///////////////////////////////////////////////////////////////////////////////////////////
// Disconnection
///////////////////////////////////////////////////////////////////////////////////////////
void				CLanServer::DisconnectSession(SESSION *pSession)
{
	CloseSocket(pSession->_SessionInfo._Socket);
}

///////////////////////////////////////////////////////////////////////////////////////////
// ���� ���� ����
///////////////////////////////////////////////////////////////////////////////////////////
void				CLanServer::CloseSocket(SOCKET socket)
{
	shutdown(socket, SD_BOTH);
}

///////////////////////////////////////////////////////////////////////////////////////////
// Release
///////////////////////////////////////////////////////////////////////////////////////////
void				CLanServer::ReleaseSession(SESSION *pSession)
{
	IOBlock stCompareBlock;

	stCompareBlock._iIOCount = 0;
	stCompareBlock._iReleaseFlag = 0;

	///////////////////////////////////////////////////////////////////////////////////////
	// ��¥ ������ �ؾ��ϴ� ������� �˻�
	///////////////////////////////////////////////////////////////////////////////////////
	if (!InterlockedCompareExchange128(
		(LONG64 *)pSession->_IOBlock,
		(LONG64)true,
		(LONG64)0,
		(LONG64 *)&stCompareBlock
		))
		return;

	closesocket(pSession->_SessionInfo._Socket);
	pSession->_SessionInfo._Socket = INVALID_SOCKET;

	pSession->_SessionInfo._iPort = 0;
	memset(&pSession->_SessionInfo._wIP, 0, sizeof(pSession->_SessionInfo._wIP));

	pSession->_RecvQ.ClearBuffer();
	pSession->_SendQ.ClearBuffer();

	memset(&pSession->_RecvOverlapped, 0, sizeof(OVERLAPPED));
	memset(&pSession->_SendOverlapped, 0, sizeof(OVERLAPPED));

	for (int iSentCnt = 0; iSentCnt < pSession->_lSentPacketCnt; iSentCnt++)
	{
		CNPacket *pPacket = (CNPacket *)pSession->_pSentPacket[iSentCnt];
		pPacket->Free();
		pSession->_pSentPacket[iSentCnt] = nullptr;
	}

	pSession->_lSentPacketCnt = 0;

	InterlockedExchange((LONG *)&pSession->_bSendFlag, false);
	InterlockedExchange((LONG *)&pSession->_bSendFlagWorker, false);

	OnClientLeave(pSession->_iSessionID);

	pSession->_IOBlock->_iReleaseFlag = false;
	InsertBlankSessionIndex(GET_SESSIONINDEX(pSession->_iSessionID));

	InterlockedDecrement((LONG *)&_lSessionCount);
}



///////////////////////////////////////////////////////////////////////////////////////////
// ���� ������ �κ�
///////////////////////////////////////////////////////////////////////////////////////////
unsigned __stdcall	CLanServer::AcceptThread(LPVOID AcceptParam)
{
	return ((CLanServer *)AcceptParam)->AccpetThread_update();
}

unsigned __stdcall	CLanServer::WorkerThread(LPVOID WorkerParam)
{
	return ((CLanServer *)WorkerParam)->WorkerThread_update();
}

unsigned __stdcall	CLanServer::MonitorThread(LPVOID MonitorParam)
{
	return ((CLanServer *)MonitorParam)->MonitorThread_update();
}
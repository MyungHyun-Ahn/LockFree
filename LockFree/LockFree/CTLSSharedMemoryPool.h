#pragma once

template<typename DATA>
struct TLSMemoryPoolNode
{
	TLSMemoryPoolNode() = default;

	DATA data;
	TLSMemoryPoolNode<DATA> *next;
};

template<typename DATA>
struct Bucket
{
	static constexpr int BUCKET_SIZE = 64;
	static constexpr int TLS_BUCKET_COUNT = 2;

	~Bucket()
	{
		// 누수 감지 코드 등 넣어도 됨
	}

	static Bucket *GetTLSBucket() noexcept
	{
		thread_local Bucket buckets[TLS_BUCKET_COUNT];
		Bucket *pPtr = buckets;
		return pPtr;
	}

	static Bucket *GetFreeBucket() noexcept
	{
		thread_local Bucket freeBucket;
		return &freeBucket;
	}



	void Clear() noexcept
	{
		m_pTop = nullptr;
		m_iSize = 0;
	}

	int Push(TLSMemoryPoolNode<DATA> *freeNode) noexcept
	{
		// Top 갱신
		freeNode->next = m_pTop;
		m_pTop = freeNode;
		m_iSize++;
		return m_iSize;
	}

	TLSMemoryPoolNode<DATA> *Pop() noexcept
	{
		if (m_iSize == 0)
			return nullptr;

		TLSMemoryPoolNode<DATA> *ret = m_pTop;
		m_pTop = m_pTop->next;
		m_iSize--;
		return ret;
	}

	// 사실상 스택 역할
	TLSMemoryPoolNode<DATA> *m_pTop = nullptr;
	int m_iSize = 0;
};

template<typename DATA, bool useQueue = TRUE>
class CTLSSharedMemoryPool
{

};

// 버킷 풀, 사실 상 스택
// Capacity 를 공용 풀에서만 측정해도 될 듯
template<typename DATA>
class CTLSSharedMemoryPool<DATA, FALSE>
{
public:
	using Node = MemoryPoolNode<Bucket<DATA>>;

	// BUCKET_SIZE 만큼 연결된 버킷의 m_pTop을 반환
	TLSMemoryPoolNode<DATA> *Alloc() noexcept
	{
		// Free 상태인 Bucket이 있는지 먼저 체크
		if (InterlockedDecrement(&m_iUseCount) < 0)
		{
			InterlockedIncrement(&m_iUseCount);

			// 없으면 찐 할당
			TLSMemoryPoolNode<DATA> *retTop = CreateNodeList();
			InterlockedAdd(&m_iCapacity, Bucket<DATA>::BUCKET_SIZE);
			return retTop;

		}

		ULONG_PTR readTop;
		ULONG_PTR newTop;
		Node *readTopAddr;

		do 
		{
			readTop = m_pTop;
			readTopAddr = (Node *)GetAddress(readTop);
			newTop = readTopAddr->next;
		} while (InterlockedCompareExchange(&m_pTop, newTop, readTop) != readTop);
		
		// Pop 성공
		TLSMemoryPoolNode<DATA> *retTop = readTopAddr->data.m_pTop;
		readTopAddr->data.Clear();
		s_BucketPool.Free(readTopAddr);
		return retTop;
	}

	// BUCKET_SIZE 만큼 연결된 버킷의 m_pTop을 반납
	void Free(TLSMemoryPoolNode<DATA> *freePtr) noexcept
	{
		// 반납
		ULONG_PTR ident = InterlockedIncrement(&m_ullCurrentIdentifier);
		ULONG_PTR readTop;
		Node *newTop = s_BucketPool.Alloc();
		newTop->data.m_pTop = freePtr;
		newTop->data.m_iSize = Bucket<DATA>::BUCKET_SIZE;
		ULONG_PTR combinedNewTop = CombineIdentAndAddr(ident, (ULONG_PTR)newTop);

		do
		{
			// readTop 또한 ident와 합성된 주소
			readTop = m_pTop;
			newTop->next = readTop;
		} while (InterlockedCompareExchange(&m_pTop, combinedNewTop, readTop) != readTop);


		InterlockedIncrement(&m_iUseCount);
	}

public:
	LONG GetCapacity()
	{
		return m_iCapacity;
	}

private:
	// 버킷 크기만큼의 노드 리스트를 생성(스택)
	TLSMemoryPoolNode<DATA> *CreateNodeList()
	{
		TLSMemoryPoolNode<DATA> *pTop = nullptr;
		for (int i = 0; i < Bucket<DATA>::BUCKET_SIZE; i++)
		{
			TLSMemoryPoolNode<DATA> *newNode = new TLSMemoryPoolNode<DATA>();
			newNode->next = pTop;
			pTop = newNode;
		}
		return pTop;
	}

private:
	ULONG_PTR		m_pTop = NULL; // Bucket Top
	ULONG_PTR		m_ullCurrentIdentifier = 0; // ABA 문제를 해결하기 위한 식별자
	LONG			m_iUseCount = 0;
	LONG			m_iCapacity = 0;
	inline static CLFMemoryPool<Node> s_BucketPool = CLFMemoryPool<Node>(100, false);
};

template<typename DATA>
class CTLSSharedMemoryPool<DATA, TRUE>
{
public:
	using Node = MemoryPoolNode<Bucket<DATA>>;

	// Dequeue
	TLSMemoryPoolNode<DATA> *Alloc() noexcept
	{
		// Free 상태인 Bucket이 있는지 먼저 체크
		if (InterlockedDecrement(&m_iUseCount) < 0)
		{
			InterlockedIncrement(&m_iUseCount);

			// 없으면 찐 할당
			TLSMemoryPoolNode<DATA> *retTop = CreateNodeList();
			InterlockedAdd(&m_iCapacity, Bucket<DATA>::BUCKET_SIZE);
			return retTop;

		}

		TLSMemoryPoolNode<DATA> *retTop;

		while (true)
		{
			ULONG_PTR readHead = m_pHead;
			ULONG_PTR readTail = m_pTail;
			Node *readHeadAddr = (Node *)GetAddress(readHead);
			ULONG_PTR next = readHeadAddr->next;
			Node *nextAddr = (Node *)GetAddress(next);

			// Head와 Tail이 같다면 한번 밀어주고 Dequeue 수행
			if (readHead == readTail)
			{
				Node *readTailAddr = (Node *)GetAddress(readTail);
				ULONG_PTR readTailNext = readTailAddr->next;
				InterlockedCompareExchange(&m_pTail, readTailNext, readTail);
			}

			// Head->next NULL인 경우는 큐가 비었을 때 뿐
			if (next == NULL)
			{
				continue;
			}
			else
			{
				if (InterlockedCompareExchange(&m_pHead, next, readHead) == readHead)
				{
					retTop = nextAddr->data.m_pTop;

					Node *readHeadAddr = (Node *)GetAddress(readHead);
					s_BucketPool.Free(readHeadAddr);
					break;
				}
			}
		}

		return retTop;
	}

	// Enqueue
	void Free(TLSMemoryPoolNode<DATA> *freePtr) noexcept
	{
		// 반납
		ULONG_PTR ident = InterlockedIncrement(&m_ullCurrentIdentifier);

		Node *newNode = s_BucketPool.Alloc();
		newNode->data.m_pTop = freePtr;
		newNode->data.m_iSize = Bucket<DATA>::BUCKET_SIZE;
		newNode->next = m_NULL;

		ULONG_PTR combinedNode = CombineIdentAndAddr(ident, (ULONG_PTR)newNode);

		while (true)
		{
			ULONG_PTR readTail = m_pTail;
			Node *readTailAddr = (Node *)GetAddress(readTail);
			ULONG_PTR next = readTailAddr->next;

			if (next != m_NULL)
			{
				InterlockedCompareExchange(&m_pTail, next, readTail);
				continue;
			}

			if (InterlockedCompareExchange(&readTailAddr->next, combinedNode, m_NULL) == m_NULL)
			{
				InterlockedCompareExchange(&m_pTail, combinedNode, readTail);
				break; 
			}
		}

		InterlockedIncrement(&m_iUseCount);
	}

public:
	LONG GetCapacity()
	{
		return m_iCapacity;
	}

private:
	// 버킷 크기만큼의 노드 리스트를 생성(스택)
	TLSMemoryPoolNode<DATA> *CreateNodeList()
	{
		TLSMemoryPoolNode<DATA> *pTop = nullptr;
		for (int i = 0; i < Bucket<DATA>::BUCKET_SIZE; i++)
		{
			TLSMemoryPoolNode<DATA> *newNode = new TLSMemoryPoolNode<DATA>();
			newNode->next = pTop;
			pTop = newNode;
		}
		return pTop;
	}

private:
	ULONG_PTR			m_pHead = NULL;
	ULONG_PTR			m_pTail = NULL;
	ULONG_PTR			m_ullCurrentIdentifier = 0; // ABA 문제를 해결하기 위한 식별자
	LONG				m_iUseCount = 0;
	LONG				m_iCapacity = 0;
	ULONG_PTR			m_NULL; // NULL 체크용

	inline static LONG	s_iQueueIdentifier = 0;
	inline static CLFMemoryPool<Node> s_BucketPool = CLFMemoryPool<Node>(100, false);
};
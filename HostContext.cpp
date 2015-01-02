
#include "HostContext.h"

#include "Threading\Task.h"
#include "Threading\TaskMgr.h"
#include "Threading\SyncMgr.h"

#include "CrstLock.h"
#include "Logger.h"

#include <mscoree.h>
#include <corerror.h>


HostContext::HostContext(ICLRRuntimeHost* runtimeHost) {
   this->runtimeHost = runtimeHost;

   m_cRef = 0;

   defaultDomainManager = NULL;
   defaultDomainId = 1; 
   domainMapCrst = new CRITICAL_SECTION;

   numZombieDomains = 0;

   if (!domainMapCrst)
      Logger::Critical("Failed to allocate critical sections");

   InitializeCriticalSection(domainMapCrst);
}

HostContext::~HostContext() {
   if (domainMapCrst) DeleteCriticalSection(domainMapCrst);
}

// IUnknown functions
STDMETHODIMP_(DWORD) HostContext::AddRef() {
   return InterlockedIncrement(&m_cRef);
}

STDMETHODIMP_(DWORD) HostContext::Release() {
   ULONG cRef = InterlockedDecrement(&m_cRef);
   if (cRef == 0) {
      delete this;
   }
   return cRef;
}

STDMETHODIMP HostContext::QueryInterface(const IID &riid, void **ppvObject) {
   if (riid == IID_IUnknown || riid == IID_IHostContext) {
      *ppvObject = this;
      AddRef();
      return S_OK;
   }

   *ppvObject = NULL;
   return E_NOINTERFACE;
}

// IHostContext functions
STDMETHODIMP HostContext::raw_GetThreadCount(
   /*[in]*/ long appDomainId,
   /*[out,retval]*/ long * pRetVal) {
   Logger::Debug("In HostContext::GetThreadCount %d", appDomainId);
   if (pRetVal == NULL)
      return E_INVALIDARG;
   
   auto appDomainInfo = appDomains.find(appDomainId);
   if (appDomainInfo == appDomains.end()) {
      Logger::Error("Cannot find AppDomain %d!", appDomainId);
      return S_FALSE;
   }
   *pRetVal = appDomainInfo->second.threadsInAppDomain;
   return S_OK;
}

STDMETHODIMP HostContext::raw_GetMemoryUsage(
   /*[in]*/ long appDomainId,
   /*[out,retval]*/ long * pRetVal) {
   Logger::Debug("In HostContext::GetThreadCount %d", appDomainId);
   if (pRetVal == NULL)
      return E_INVALIDARG;

   auto appDomainInfo = appDomains.find(appDomainId);
   if (appDomainInfo == appDomains.end()) {
      Logger::Error("Cannot find AppDomain %d!", appDomainId);
      return S_FALSE;
   }
   *pRetVal = appDomainInfo->second.bytesInAppDomain;
   return S_OK;
}

STDMETHODIMP HostContext::raw_GetNumberOfZombies(
   /*[out,retval]*/ long * pRetVal) {
   Logger::Debug("In HostContext::raw_GetNumberOfZombies");
   if (pRetVal == NULL)
      return E_INVALIDARG;
     
   *pRetVal = numZombieDomains;
   return S_OK;
}

STDMETHODIMP HostContext::raw_ResetCountersForAppDomain(/*[in]*/long appDomainId) {
   Logger::Debug("In HostContext::raw_ResetCountersForAppDomain");
   auto appDomainInfo = appDomains.find(appDomainId);
   if (appDomainInfo == appDomains.end()) {
      Logger::Error("Cannot find AppDomain %d!", appDomainId);      
   }
   else {
      appDomainInfo->second.bytesInAppDomain = 0;
      appDomainInfo->second.threadsInAppDomain = 1;
   }
   return S_OK;
}

STDMETHODIMP HostContext::raw_UnloadDomain(/*[in]*/long appDomainId) {
   // Alternative: ask the managed manager to do it (unload its domain)
   //auto appDomainInfo = appDomains.find(appDomainId);
   //if (appDomainInfo == appDomains.end()) {
   //   Logger::Error("Cannot find AppDomain %d!", appDomainId);
   //   return E_INVALIDARG;
   //}
   //else {
   //   ISimpleHostDomainManager* domain = appDomainInfo->second.appDomainManager;
   //   domain->Unload();
   //}
   
   return runtimeHost->UnloadAppDomain(appDomainId, false);
}


void HostContext::OnDomainUnload(DWORD domainId) {

   CrstLock(this->domainMapCrst);
   auto domainIt = appDomains.find(domainId);
   if (domainIt != appDomains.end()) {
      appDomains.erase(domainIt);
   }

}

void HostContext::OnDomainRudeUnload() {
   //CrstLock(this->domainMapCrst);
   InterlockedIncrement(&numZombieDomains);
}

void HostContext::OnDomainCreate(DWORD dwAppDomainID, DWORD dwCurrentThreadId, ISimpleHostDomainManager* domainManager) {

   CrstLock(this->domainMapCrst);
   appDomains.insert(std::make_pair(dwAppDomainID, AppDomainInfo(dwCurrentThreadId, domainManager)));

   // "Migrate" a thread, if it was already assigned to a domain
   auto currentThreadDomain = threadAppDomain.find(dwCurrentThreadId);
   if (currentThreadDomain != threadAppDomain.end()) {
      DWORD currentAppDomainId = currentThreadDomain->second;
      Logger::Debug("Thread %d moving from domain %d to domain %d", dwCurrentThreadId, currentAppDomainId, dwAppDomainID);
      AppDomainInfo& domainInfo = appDomains.at(currentAppDomainId);
      --(domainInfo.threadsInAppDomain);

      currentThreadDomain->second = dwAppDomainID;
   }
   else {
      threadAppDomain.insert(std::make_pair(dwCurrentThreadId, dwAppDomainID));
   }
   if (defaultDomainManager == NULL) {
      defaultDomainId = dwAppDomainID; // It should always be 1, but.. you never know
      defaultDomainManager = domainManager;
   }   
}

ISimpleHostDomainManager* HostContext::GetDomainManagerForDefaultDomain() {
   if (defaultDomainManager)
      defaultDomainManager->AddRef();

   return defaultDomainManager;
}

bool HostContext::OnThreadAcquire(DWORD dwParentThreadId, DWORD dwThreadId) {

   CrstLock(this->domainMapCrst);
   auto parentThreadDomain = threadAppDomain.find(dwParentThreadId);
   if (parentThreadDomain != threadAppDomain.end()) {
      DWORD appDomainId = parentThreadDomain->second;
      Logger::Debug("Thread %d added to domain %d", dwThreadId, appDomainId);
      AppDomainInfo& domainInfo = appDomains.at(appDomainId);
      ++(domainInfo.threadsInAppDomain);
      threadAppDomain.insert(std::make_pair(dwThreadId, parentThreadDomain->second));

#ifdef TRACK_THREAD_RELATIONSHIP
      childThreadToParent.insert(std::make_pair(dwThreadId, dwParentThreadId));
#endif //TRACK_THREAD_RELATIONSHIP
      return true;
   }

   return false;
}

bool HostContext::OnThreadRelease(DWORD dwThreadId) {

   CrstLock(this->domainMapCrst);
   auto parentThreadDomain = threadAppDomain.find(dwThreadId);
   if (parentThreadDomain != threadAppDomain.end()) {
      DWORD appDomainId = parentThreadDomain->second;
      Logger::Debug("Thread %d removed from domain %d", dwThreadId, appDomainId);
      AppDomainInfo& domainInfo = appDomains.at(appDomainId);
      --(domainInfo.threadsInAppDomain);

#ifdef TRACK_THREAD_RELATIONSHIP
      // Remove thread child-parent relationship, where threadId is the parent
      for (auto it = childThreadToParent.begin(); it != childThreadToParent.end(); ++it) {
         DWORD childId = it->first;
         DWORD parentId = it->second;
         if (parentId == dwThreadId) {
            // Get the new parent
            auto parentParent = childThreadToParent.find(parentId);
            // Insert the new child - grandfather relationship (if there is one)
            if (parentParent != childThreadToParent.end()) {
               childThreadToParent.insert(std::make_pair(childId, parentParent->first));
            }

            childThreadToParent.erase(it);
            break;
         }
      }      
      // Remove thread child-parent relationship, where threadId is the child
      childThreadToParent.erase(dwThreadId);
#endif //TRACK_THREAD_RELATIONSHIP

      threadAppDomain.erase(parentThreadDomain);
      if (domainInfo.mainThreadId == dwThreadId) {
         Logger::Debug("Thread %d is the domain main thread. Removing association with %d", dwThreadId, appDomainId);
         defaultDomainManager->OnMainThreadExit(appDomainId, domainInfo.threadsInAppDomain == 0);
         appDomains.erase(appDomainId);
      }
      return true;
   }

   return false;
}

bool HostContext::IsSnippetThread(DWORD dwNativeThreadId) {
   CrstLock(this->domainMapCrst);
   
   auto appDomain = threadAppDomain.find(dwNativeThreadId);
   if (appDomain == threadAppDomain.end())
      return false;

   return (appDomain->second != defaultDomainId);
}

HRESULT HostContext::Sleep(DWORD dwMilliseconds, DWORD option) {

   BOOL alertable = option & WAIT_ALERTABLE;

   // WAIT_MSGPUMP: Notifies the host that it must pump messages on the current OS thread if the thread becomes blocked.The runtime specifies this value only on an STA thread.
   if (option & WAIT_MSGPUMP) {
      // There is a nice, general solution from Raymond Chen: http://blogs.msdn.com/b/oldnewthing/archive/2006/01/26/517849.aspx
      // BUT
      // according to Chris Brumme, we need NOT to pump too much!
      // http://blogs.msdn.com/b/cbrumme/archive/2003/04/17/51361.aspx
      // So, we use a simpler solution, since we also assume to be on XP or later: delegate to CoWaitForMultipleHandles
      // http://blogs.msdn.com/b/cbrumme/archive/2004/02/02/66219.aspx
      // http://msdn.microsoft.com/en-us/library/windows/desktop/ms680732%28v=vs.85%29.aspx
      DWORD dwFlags = 0;
      DWORD dwIndex;

      // if working with windows store, ensure
      // DWORD dwFlags = COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES;

      if (alertable) {
         dwFlags |= COWAIT_ALERTABLE;
         // See http://msdn.microsoft.com/en-us/library/windows/desktop/ms680732%28v=vs.85%29.aspx
         SetLastError(ERROR_SUCCESS);
      }

      // CoWaitForMultipleHandles may call WaitForMultipleObjectEx, which does NOT support 0 array handles.
      // Besides, MSDN is not clear if the handle array can be NULL, so.. we use a trick
      HANDLE dummy = GetCurrentProcess();
      HRESULT hr = CoWaitForMultipleHandles(dwFlags, dwMilliseconds, 1, &dummy, &dwIndex);
      if (dwIndex == WAIT_TIMEOUT) {
         return S_OK;
      }
      return hr;
   }
   else {
      return HRESULTFromWaitResult(SleepEx(dwMilliseconds, alertable));
   }
}

HRESULT HostContext::HRESULTFromWaitResult(DWORD dwWaitResult) {
   switch (dwWaitResult) {
   case WAIT_OBJECT_0:
      return S_OK;
   case WAIT_ABANDONED:
      return HOST_E_ABANDONED;
   case WAIT_IO_COMPLETION:
      return HOST_E_INTERRUPTED;
   case WAIT_TIMEOUT:
      return HOST_E_TIMEOUT;
   case WAIT_FAILED: {
      DWORD error = GetLastError();
      Logger::Error("Wait failed: %d", error);
      return HRESULT_FROM_WIN32(error);
   }
   default:
      Logger::Error("Unexpected wait result: %d", dwWaitResult);
      return E_FAIL;
   }
}

HRESULT HostContext::HostWait(HANDLE hWait, DWORD dwMilliseconds, DWORD dwOption) {

   BOOL alertable = dwOption & WAIT_ALERTABLE;
   if (dwOption & WAIT_MSGPUMP) {
      DWORD dwFlags = 0;
      if (alertable) {
         dwFlags |= COWAIT_ALERTABLE;
         // See http://msdn.microsoft.com/en-us/library/windows/desktop/ms680732%28v=vs.85%29.aspx
         SetLastError(ERROR_SUCCESS);
      }
      return CoWaitForMultipleHandles(dwFlags, dwMilliseconds, 1, &hWait, NULL);
   }
   else {
      return HRESULTFromWaitResult(WaitForSingleObjectEx(hWait, dwMilliseconds, alertable));
   }
}

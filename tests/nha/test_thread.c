/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>
#include <errno.h>

int __real_plat_thread_try(PThreadFn fn, void* arg, size_t stack, PError* error);
void __real_plat_thread_forget(void* handle);
static int reject_start=1, attempts;
static int exits;
static PMutex gate_lock;
static PCond gate_cond;
static int ready, release, finished;
static u32 target;

int __wrap_plat_thread_try(PThreadFn fn, void* arg, size_t stack, PError* error){
  if(stack!=((size_t)64<<20)) return __real_plat_thread_try(fn,arg,stack,error);
  attempts++;
  if(!reject_start) return __real_plat_thread_try(fn,arg,stack,error);
  *error=(PError){PERR_ERRNO,EAGAIN};
  return -1;
}

void __wrap_plat_thread_forget(void* handle){
#if defined(_WIN32)
  DWORD flags;
  assert(handle && GetHandleInformation((HANDLE)handle,&flags));
#endif
  __real_plat_thread_forget(handle);
#if defined(_WIN32)
  assert(!GetHandleInformation((HANDLE)handle,&flags) && GetLastError()==ERROR_INVALID_HANDLE);
#endif
  pm_lock(&gate_lock);
  exits++;
  pc_broadcast(&gate_cond);
  pm_unlock(&gate_lock);
}

static void start_together(void* arg){
  KThread caller={0};
  tls_thread=&caller;
  caller.cpu.thread=&caller;
  pm_lock(&gate_lock);
  ready++;
  pc_broadcast(&gate_cond);
  while(!release) pc_wait(&gate_cond,&gate_lock,-1);
  pm_unlock(&gate_lock);
  caller.cpu.x[0]=target;
  swr_svc(&caller.cpu,9);
  *(u32*)arg=(u32)caller.cpu.x[0];
  pm_lock(&gate_lock);
  finished++;
  pc_broadcast(&gate_cond);
  pm_unlock(&gate_lock);
}

int main(void){
  assert(!nha_init());
  mem_init();
  kernel_init();
  pm_init(&gate_lock);
  pc_init(&gate_cond);
  KThread main_thread={0};
  tls_thread=&main_thread;
  pc_init(&main_thread.cv);
  CPU* c=&main_thread.cpu;
  c->thread=&main_thread;
  c->x[1]=AS_BASE; c->x[3]=AS_BASE+0x1000; c->x[4]=64; c->x[5]=0;
  swr_svc(c,8);
  assert(c->x[0]==0xF001 && !c->x[1]);
  c->x[1]=AS_BASE+2; c->x[4]=63;
  swr_svc(c,8);
  assert(c->x[0]==0xCC01 && !c->x[1]);
  c->x[1]=AS_BASE; c->x[3]=AS_BASE+8;
  swr_svc(c,8);
  assert(c->x[0]==0xCC01 && !c->x[1]);
  c->x[1]=AS_BASE; c->x[3]=AS_BASE+0x1000; c->x[2]=0xCAFE; c->x[5]=3;
  swr_svc(c,8);
  assert(!c->x[0]);
  u32 handle=(u32)c->x[1];
  KThread* thread=(KThread*)h_get_type(handle,KO_THREAD);
  assert(thread && thread->state==0 && thread->cpu.tpidrro==thread->tls);
  assert(thread->prio==63 && thread->core==3 && thread->ideal==3 && thread->affinity==8);
  assert(thread->cpu.x[31]==AS_BASE+0x1000 && thread->cpu.x[0]==0xCAFE);

#if defined(_WIN32)
  DWORD handles_before, handles_after;
  assert(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
  assert(handles_before || GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version"));
#endif
  c->x[0]=handle;
  swr_svc(c,9);
  assert(c->x[0]==0xD001 && attempts==1 && thread->state==0 && !thread->host && thread->o.refs==1);
#if defined(_WIN32)
  assert(GetProcessHandleCount(GetCurrentProcess(),&handles_after) && handles_after==handles_before);
  fprintf(stderr,"NHA_TEST {\"failed_start_handles_before\":%lu,\"failed_start_handles_after\":%lu,"
          "\"resource_count_available\":%d}\n",
          (unsigned long)handles_before,(unsigned long)handles_after,handles_before!=0);
#endif
  reject_start=0;
  /* A zero PC exercises the normal guest trampoline's return/exit path. */
  thread->cpu.pc=0;
  target=handle;
  u32 starts[2];
  assert(!plat_thread(start_together,&starts[0],1<<20));
  assert(!plat_thread(start_together,&starts[1],1<<20));
  pm_lock(&gate_lock);
  while(ready!=2) pc_wait(&gate_cond,&gate_lock,-1);
  release=1;
  pc_broadcast(&gate_cond);
  while(finished!=2 || !exits) pc_wait(&gate_cond,&gate_lock,-1);
  pm_unlock(&gate_lock);
  assert((!starts[0] && starts[1]==0xFA01) || (!starts[1] && starts[0]==0xFA01));
  assert(attempts==2 && exits==1 && !thread->host);

  assert(!mem_map(AS_BASE,0x1000,MS_NORMAL,PERM_RW));
  gw32(AS_BASE,handle);
  c->x[1]=AS_BASE; c->x[2]=1; c->x[3]=1000000000;
  swr_svc(c,0x18);
  assert(!c->x[0] && c->x[1]==0);
  assert(!h_close(handle));
  c->x[0]=handle;
  swr_svc(c,9);
  assert(c->x[0]==0xE401 && attempts==2);

  /* A handle-table failure must roll back the newly mapped TLS and CV. */
  u64 free_tls=mem_find_free(KERN_BASE,KERN_END,0x2000,0x1000);
  assert(free_tls);
  KObj dummy={0};
  u32 handles[4096];
  unsigned n=0;
  for(;n<4096 && (handles[n]=h_try_new(&dummy))!=0;n++){}
  c->x[1]=AS_BASE; c->x[3]=AS_BASE+0x1000; c->x[4]=32; c->x[5]=0;
  swr_svc(c,8);
  assert(c->x[0]==0xD201 && c->x[1]==0);
  assert(mem_find_free(KERN_BASE,KERN_END,0x2000,0x1000)==free_tls);
  while(n) assert(!h_close(handles[--n]));
  assert(!dummy.refs && !thread->o.refs);
  fprintf(stderr,"NHA_TEST {\"host_start_attempts\":%d,\"host_exits\":%d,"
          "\"guest_thread_refs\":%d,\"start_errno\":%d}\n",attempts,exits,thread->o.refs,EAGAIN);
  pc_destroy(&gate_cond);
  pc_destroy(&main_thread.cv);
  return 0;
}

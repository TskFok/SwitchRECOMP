/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>
#include <errno.h>

int __real_plat_thread_try(PThreadFn fn, void* arg, size_t stack, PError* error);
static int reject_start=1, attempts;
int __wrap_plat_thread_try(PThreadFn fn, void* arg, size_t stack, PError* error){
  attempts++;
  if(!reject_start) return __real_plat_thread_try(fn,arg,stack,error);
  *error=(PError){PERR_ERRNO,EAGAIN};
  return -1;
}

static NhaCall dispatch(KThread* caller, u32 imm){
  NhaCall call={0};
  nha_kernel_svc(&caller->cpu,imm,&call);
  return call;
}

int main(void){
  mem_init();
  kernel_init();
  KThread main_thread={0};
  tls_thread=&main_thread;
  pc_init(&main_thread.cv);
  CPU* c=&main_thread.cpu;
  c->thread=&main_thread;
  c->x[1]=AS_BASE; c->x[3]=AS_BASE+0x1000; c->x[4]=64; c->x[5]=0;
  dispatch(&main_thread,8);
  assert(c->x[0]==0xF001);
  c->x[1]=AS_BASE; c->x[4]=32;
  dispatch(&main_thread,8);
  assert(!c->x[0]);
  u32 handle=(u32)c->x[1];
  KThread* thread=(KThread*)h_get_type(handle,KO_THREAD);
  assert(thread && thread->state==0 && thread->cpu.tpidrro==thread->tls);

  c->x[0]=handle;
  NhaCall start=dispatch(&main_thread,9);
  assert(c->x[0]==0xD001 && attempts==1 && thread->state==0);
  assert(start.host_error.domain==PERR_ERRNO && start.host_error.code==EAGAIN);
  reject_start=0;
  /* A zero PC exercises the normal guest trampoline's return/exit path. */
  thread->cpu.pc=0;
  c->x[0]=handle;
  dispatch(&main_thread,9);
  assert(!c->x[0] && attempts==2);
  c->x[0]=handle;
  dispatch(&main_thread,9);
  assert(c->x[0]==0xFA01 && attempts==2);

  assert(!mem_map(AS_BASE,0x1000,MS_NORMAL,PERM_RW));
  gw32(AS_BASE,handle);
  c->x[1]=AS_BASE; c->x[2]=1; c->x[3]=1000000000;
  dispatch(&main_thread,0x18);
  assert(!c->x[0] && c->x[1]==0);
  assert(!h_close(handle));

  /* A handle-table failure must roll back the newly mapped TLS and CV. */
  u64 free_tls=mem_find_free(KERN_BASE,KERN_END,0x2000,0x1000);
  assert(free_tls);
  KObj dummy={0};
  u32 handles[4096];
  unsigned n=0;
  for(;n<4096 && (handles[n]=h_try_new(&dummy))!=0;n++){}
  c->x[1]=AS_BASE; c->x[3]=AS_BASE+0x1000; c->x[4]=32; c->x[5]=0;
  dispatch(&main_thread,8);
  assert(c->x[0]==0xD201 && c->x[1]==0);
  assert(mem_find_free(KERN_BASE,KERN_END,0x2000,0x1000)==free_tls);
  while(n) assert(!h_close(handles[--n]));
  return 0;
}

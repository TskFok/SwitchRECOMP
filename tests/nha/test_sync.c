/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>

static KThread waiter, caller;
static PMutex done_lock;
static PCond done_cond;
static int done;

static u32 svc(KThread* thread, u32 imm, u64 a, u64 b, u64 c, u64 d){
  NhaCall call={0};
  CPU* cpu=&thread->cpu;
  cpu->x[0]=a; cpu->x[1]=b; cpu->x[2]=c; cpu->x[3]=d;
  nha_kernel_svc(cpu,imm,&call);
  return (u32)cpu->x[0];
}

static void finish(void){
  pm_lock(&done_lock);
  done=1;
  pc_signal(&done_cond);
  pm_unlock(&done_lock);
}

static void join(void){
  pm_lock(&done_lock);
  while(!done) pc_wait(&done_cond,&done_lock,-1);
  done=0;
  pm_unlock(&done_lock);
}

static void wait_and_cancel(void* unused){
  (void)unused;
  tls_thread=&waiter;
  assert(svc(&waiter,0x18,0,AS_BASE,1,(u64)-1)==0xEC01);
  finish();
}

static void wait_for_kind(int kind){
  u64 start=plat_ns();
  for(;;){
    pm_lock(&g_klock);
    int found=waiter.wait_kind==kind;
    pm_unlock(&g_klock);
    if(found) return;
    assert(plat_ns()-start<2000000000ULL);
    plat_yield();
  }
}

static void wait_for_mutex(void* arg){
  u32* h=arg;
  tls_thread=&waiter;
  assert(!svc(&waiter,0x1a,h[0],AS_BASE+8,h[1],0));
  assert(gr32(AS_BASE+8)==h[1]);
  finish();
}

int main(void){
  kernel_init(); mem_init();
  assert(!mem_map(AS_BASE,0x1000,MS_NORMAL,PERM_RW));
  pc_init(&waiter.cv); pc_init(&caller.cv); pm_init(&done_lock); pc_init(&done_cond);
  caller.cpu.thread=&caller;
  waiter.cpu.thread=&waiter;
  waiter.o.type=caller.o.type=KO_THREAD;
  tls_thread=&caller;
  u32 handles[]={h_new(&caller.o),h_new(&waiter.o)};

  assert(!svc(&caller,0x45,0,0,0,0));
  u32 writer=(u32)caller.cpu.x[1], reader=(u32)caller.cpu.x[2];
  gw32(AS_BASE,reader);
  assert(svc(&caller,0x18,0,AS_BASE,1,0)==0xEA01);
  u64 start=plat_ns();
  assert(svc(&caller,0x18,0,AS_BASE,1,2000000)==0xEA01);
  assert(plat_ns()-start>=2000000);
  assert(svc(&caller,0x11,reader,0,0,0)==0xE401);
  assert(!svc(&caller,0x11,writer,0,0,0));
  assert(!svc(&caller,0x18,0,AS_BASE,1,0));
  assert(!svc(&caller,0x18,0,AS_BASE,1,0)); /* manual reset */
  assert(!svc(&caller,0x17,reader,0,0,0));
  assert(svc(&caller,0x17,reader,0,0,0)==0xFA01);
  assert(svc(&caller,0x18,0,AS_SIZE,1,0)==0xCC01);

  assert(!plat_thread(wait_and_cancel,NULL,1<<20));
  wait_for_kind(1);
  assert(!h_close(reader) && !h_close(writer));
  assert(!svc(&caller,0x19,handles[1],0,0,0));
  join();

  gw32(AS_BASE+8,handles[0]|0x40000000u);
  assert(!plat_thread(wait_for_mutex,handles,1<<20));
  wait_for_kind(2);
  assert(!svc(&caller,0x1b,AS_BASE+8,0,0,0));
  join();
  assert(!svc(&caller,0x1b,AS_BASE+8,0,0,0) && !gr32(AS_BASE+8));
  assert(svc(&caller,0x1b,AS_SIZE,0,0,0)==0xCC01);
  assert(!h_close(handles[0]));
  gw32(AS_BASE+8,handles[0]|0x40000000u);
  assert(svc(&caller,0x1a,handles[0],AS_BASE+8,handles[1],0)==0xE401);
  assert(!h_close(handles[1]));
  return 0;
}

/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>

static KThread waiter, other, caller;
static PMutex done_lock;
static PCond done_cond;
static int done;
static const u64 mutex=AS_BASE+8, key=AS_BASE+12, payload=AS_BASE+16;

static u32 svc(KThread* thread, u32 imm, u64 a, u64 b, u64 c, u64 d){
  CPU* cpu=&thread->cpu;
  cpu->x[0]=a; cpu->x[1]=b; cpu->x[2]=c; cpu->x[3]=d;
  swr_svc(cpu,imm);
  return (u32)cpu->x[0];
}

static void finish(void){
  pm_lock(&done_lock);
  done++;
  pc_signal(&done_cond);
  pm_unlock(&done_lock);
}

static void join(int count){
  pm_lock(&done_lock);
  while(done<count) pc_wait(&done_cond,&done_lock,-1);
  assert(done==count);
  done=0;
  pm_unlock(&done_lock);
}

static void wait_and_cancel(void* unused){
  (void)unused;
  tls_thread=&waiter;
  assert(svc(&waiter,0x18,0,AS_BASE,1,(u64)-1)==0xEC01);
  finish();
}

static void wait_for_kind(KThread* thread, int kind){
  u64 start=plat_ns();
  for(;;){
    pm_lock(&g_klock);
    int found=thread->wait_kind==kind;
    pm_unlock(&g_klock);
    if(found) return;
    assert(plat_ns()-start<2000000000ULL);
    plat_yield();
  }
}

static void wait_for_mutex(void* arg){
  u32* h=arg;
  tls_thread=&waiter;
  assert(!svc(&waiter,0x1a,h[0],mutex,h[1],0));
  assert(gr32(mutex)==h[1] && gr32(payload)==0xCAFE);
  finish();
}

static void wait_for_event(void* arg){
  KThread* thread=arg;
  tls_thread=thread;
  assert(!svc(thread,0x18,0,AS_BASE,1,(u64)-1));
  assert(!thread->cpu.x[1] && gr32(payload)==0x1234);
  finish();
}

static void wait_for_cv(void* arg){
  u32 tag=*(const u32*)arg;
  tls_thread=&waiter;
  assert(!svc(&waiter,0x1c,mutex,key,tag,(u64)-1));
  assert(gr32(mutex)==tag && gr32(payload)==0x12345678);
  assert(!svc(&waiter,0x1b,mutex,0,0,0));
  finish();
}

typedef struct {
  u32 reader, writer, target, result;
  int ready, release;
} WaitRace;

static void wait_through_race(void* arg){
  WaitRace* race=arg;
  tls_thread=&waiter;
  race->result=svc(&waiter,0x18,0,AS_BASE,1,(u64)-1);
  finish();
}

static void await_race_start(WaitRace* race){
  pm_lock(&done_lock);
  race->ready++;
  pc_broadcast(&done_cond);
  while(!race->release) pc_wait(&done_cond,&done_lock,-1);
  pm_unlock(&done_lock);
}

static void close_racing_reader(void* arg){
  WaitRace* race=arg;
  await_race_start(race);
  assert(!h_close(race->reader));
  finish();
}

static void signal_racing_event(void* arg){
  WaitRace* race=arg;
  KThread signaler={0};
  signaler.cpu.thread=&signaler;
  tls_thread=&signaler;
  await_race_start(race);
  assert(!svc(&signaler,0x11,race->writer,0,0,0));
  finish();
}

static void cancel_racing_wait(void* arg){
  WaitRace* race=arg;
  KThread canceler={0};
  canceler.cpu.thread=&canceler;
  tls_thread=&canceler;
  await_race_start(race);
  assert(!svc(&canceler,0x19,race->target,0,0,0));
  finish();
}

static void check_pending_cancel(int pending){
  assert(waiter.cancel==pending && !waiter.wait_kind);
  tls_thread=&waiter;
  assert(svc(&waiter,0x18,0,0,0,0)==(pending? 0xEC01 : 0xEA01));
  assert(svc(&waiter,0x18,0,0,0,0)==0xEA01 && !waiter.cancel);
  tls_thread=&caller;
}

int main(void){
  assert(!nha_init());
  kernel_init(); mem_init();
  assert(!mem_map(AS_BASE,0x1000,MS_NORMAL,PERM_RW));
  pc_init(&waiter.cv); pc_init(&caller.cv); pm_init(&done_lock); pc_init(&done_cond);
  pc_init(&other.cv);
  caller.cpu.thread=&caller;
  waiter.cpu.thread=&waiter;
  other.cpu.thread=&other;
  waiter.o.type=caller.o.type=KO_THREAD;
  tls_thread=&caller;
  u32 handles[]={h_new(&caller.o),h_new(&waiter.o)};

  assert(!svc(&caller,0x45,0,0,0,0));
  u32 writer=(u32)caller.cpu.x[1], reader=(u32)caller.cpu.x[2];
  gw32(AS_BASE,reader);
  assert(svc(&caller,0x18,0,AS_BASE,1,0)==0xEA01);
  u64 start=plat_ns();
  assert(svc(&caller,0x18,0,AS_BASE,1,200001)==0xEA01);
  assert(plat_ns()-start>=200001);
  assert(svc(&caller,0x11,reader,0,0,0)==0xE401);
  assert(svc(&caller,0x17,writer,0,0,0)==0xE401);
  assert(svc(&caller,0x19,reader,0,0,0)==0xE401);
  gw32(AS_BASE,writer);
  assert(svc(&caller,0x18,0,AS_BASE,1,0)==0xE401);
  gw32(AS_BASE,reader);
  KObj* event=h_get(reader);
  assert(event && event->refs==2 && !event->nwait);

  assert(!plat_thread(wait_for_event,&waiter,1<<20));
  assert(!plat_thread(wait_for_event,&other,1<<20));
  wait_for_kind(&waiter,1);
  wait_for_kind(&other,1);
  gw32(payload,0x1234);
  assert(!svc(&caller,0x11,writer,0,0,0));
  join(2);
  assert(event->refs==2 && !event->nwait);
  /* Signal completed first, so subsequent cancel must survive until the next wait. */
  assert(!svc(&caller,0x19,handles[1],0,0,0));
  check_pending_cancel(1);
  assert(!svc(&caller,0x18,0,AS_BASE,1,0));
  assert(!svc(&caller,0x18,0,AS_BASE,1,0)); /* manual reset */
  assert(!svc(&caller,0x12,reader,0,0,0));
  assert(svc(&caller,0x18,0,AS_BASE,1,0)==0xEA01);
  assert(!svc(&caller,0x11,writer,0,0,0));
  assert(!svc(&caller,0x17,reader,0,0,0));
  assert(svc(&caller,0x17,reader,0,0,0)==0xFA01);
  assert(svc(&caller,0x18,0,AS_SIZE,1,0)==0xCC01);
  assert(svc(&caller,0x18,0,AS_BASE+1,1,0)==0xCC01);
  assert(svc(&caller,0x18,0,AS_BASE,65,0)==0xEE01);
  assert(svc(&caller,0x12,0,0,0,0)==0xE401);

  KEvent* automatic=kevent_new("test automatic event",1);
  assert(automatic);
  u32 automatic_handle=h_new(&automatic->o);
  kevent_signal(automatic);
  gw32(AS_BASE,automatic_handle);
  gw32(AS_BASE+4,reader);
  assert(svc(&caller,0x18,0,AS_BASE,2,0)==0xF601);
  assert(svc(&caller,0x12,automatic_handle,0,0,0)==0xF601 && automatic->signaled);
  assert(automatic->o.refs==1 && !h_close(automatic_handle));
  /* First signaled slot is returned and provisional references are released. */
  assert(!svc(&caller,0x11,writer,0,0,0));
  KEvent* idle=kevent_new("test unsignaled event",0);
  assert(idle);
  u32 idle_handle=h_new(&idle->o);
  gw32(AS_BASE,idle_handle);
  assert(svc(&caller,0x18,0,AS_BASE,2,0)==0 && caller.cpu.x[1]==1);
  assert(idle->o.refs==1 && !h_close(idle_handle));
  gw32(AS_BASE,reader);
  assert(svc(&caller,0x18,0,AS_BASE,2,0)==0 && !caller.cpu.x[1]);
  gw32(AS_BASE+4,0);
  assert(svc(&caller,0x18,0,AS_BASE,2,0)==0xE401 && event->refs==2);
  assert(!svc(&caller,0x12,writer,0,0,0));

  assert(!plat_thread(wait_and_cancel,NULL,1<<20));
  wait_for_kind(&waiter,1);
  assert(!h_close(reader) && !h_close(writer));
  assert(!svc(&caller,0x19,handles[1],0,0,0));
  join(1);
  check_pending_cancel(0);

  assert(!svc(&caller,0x45,0,0,0,0));
  writer=(u32)caller.cpu.x[1]; reader=(u32)caller.cpu.x[2];
  gw32(AS_BASE,reader);
  WaitRace race={.reader=reader,.writer=writer,.target=handles[1]};
  assert(!plat_thread(wait_through_race,&race,1<<20));
  wait_for_kind(&waiter,1);
  assert(!plat_thread(close_racing_reader,&race,1<<20));
  assert(!plat_thread(signal_racing_event,&race,1<<20));
  assert(!plat_thread(cancel_racing_wait,&race,1<<20));
  pm_lock(&done_lock);
  while(race.ready!=3) pc_wait(&done_cond,&done_lock,-1);
  race.release=1;
  pc_broadcast(&done_cond);
  pm_unlock(&done_lock);
  join(4);
  assert(!race.result || race.result==0xEC01);
  check_pending_cancel(!race.result);
  event=h_get(writer);
  assert(event && event->refs==1 && !event->nwait && !h_close(writer));

  gw32(mutex,handles[0]|0x40000000u);
  assert(!plat_thread(wait_for_mutex,handles,1<<20));
  wait_for_kind(&waiter,2);
  gw32(payload,0xCAFE);
  assert(!svc(&caller,0x1b,mutex,0,0,0));
  join(1);
  assert(!svc(&caller,0x1b,mutex,0,0,0) && !gr32(mutex));
  assert(svc(&caller,0x1b,mutex+1,0,0,0)==0xCC01);
  assert(svc(&caller,0x1a,handles[0],AS_SIZE,handles[1],0)==0xCC01);

  gw32(mutex,handles[1]);
  assert(!plat_thread(wait_for_cv,&handles[1],1<<20));
  wait_for_kind(&waiter,3);
  assert(!gr32(mutex) && gr32(key)==1);
  gw32(mutex,handles[0]);
  assert(!svc(&caller,0x1d,key,1,0,0));
  wait_for_kind(&waiter,2);
  assert(gr32(mutex)==(handles[0]|0x40000000u) && !gr32(key));
  gw32(payload,0x12345678);
  assert(!svc(&caller,0x1b,mutex,0,0,0));
  join(1);
  assert(!gr32(mutex));

  assert(!h_close(handles[0]));
  gw32(mutex,handles[0]|0x40000000u);
  assert(svc(&caller,0x1a,handles[0],mutex,handles[1],0)==0xE401);
  assert(!h_close(handles[1]));

  KObj dummy={0};
  u32 full[4096];
  unsigned n=0;
  while(n<4096 && (full[n]=h_try_new(&dummy))) n++;
  assert(n==4095 && !h_close(full[--n]));
  assert(svc(&caller,0x45,0,0,0,0)==0xD201 && !caller.cpu.x[1] && !caller.cpu.x[2]);
  u32 last=h_try_new(&dummy);
  assert(last && !h_try_new(&dummy) && !h_close(last));
  while(n) assert(!h_close(full[--n]));
  assert(!dummy.refs && !waiter.o.refs && !caller.o.refs);
  fprintf(stderr,"NHA_TEST {\"event_waiters_woken\":2,\"race_rc\":%u,\"released_handle_slots\":4095}\n",race.result);
  pc_destroy(&done_cond);
  pc_destroy(&waiter.cv);
  pc_destroy(&other.cv);
  pc_destroy(&caller.cv);
  return 0;
}

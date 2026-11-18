/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>

static const u64 msg=AS_BASE, path=AS_BASE+0x1000, buffer=AS_BASE+0x1020;
static KThread caller;

void svc_fs_init(void);

static void header(u32 type, u32 command, u32 raw, u32 words){
  memset(GP(msg),0,0x100);
  gw32(msg,type);
  gw32(msg+4,words);
  gw32(msg+raw,0x49434653);
  gw32(msg+raw+8,command);
}

static u32 request(u32 handle, int domain){
  ipc_handle(&caller,handle,msg,0x100);
  assert(caller.cpu.x[0]==0);
  return gr32(msg+(domain? 40 : 24));
}

static void open_path(void){
  header(0x10004,8,16,12);
  gw32(msg+8,8<<16);
  gw32(msg+12,(u32)path);
  gw32(msg+32,1);
}

static void file_read(u32 addr, u64 count){
  header(0x01000004,0,32,16);
  gw32(msg+8,8);
  gw32(msg+12,addr);
  gw64(msg+64,count);
}

int main(void){
  assert(!nha_init());
  mem_init(); kernel_init(); svc_fs_init();
  caller.cpu.thread=&caller;
  tls_thread=&caller;
  assert(!mem_map(AS_BASE,0x2000,MS_NORMAL,PERM_RW));

  strcpy(g_datadir,"data/");
  assert(!plat_mkdir("data"));
  u32 fsp=session_new(svc_open("fsp-srv"));
  header(4,18,16,8);
  assert(!request(fsp,0));
  u32 fs=gr32(msg+12);
  assert(!h_close(fsp));

  FILE* example=fopen("data/sdmc/one.txt","wb");
  assert(example && fwrite("hello\n",1,6,example)==6 && !fclose(example));
  memcpy(GP(path),"one.txt",8);

  u32 handles[4096];
  KObj dummy={0};
  unsigned n=0;
  for(;n<4096 && (handles[n]=h_try_new(&dummy))!=0;n++){}
  assert(n>1 && n<4096);
  open_path();
  assert(request(fs,0)==RC(2,6709));
  assert(!h_close(handles[--n]));
  open_path();
  assert(!request(fs,0));
  u32 file=gr32(msg+12);
  while(n) assert(!h_close(handles[--n]));
  assert(dummy.refs==0);

  /* Out buffer validation runs before accessing the host file. */
  file_read((u32)(AS_BASE+0x2000),8);
  assert(request(file,0)==RC(2,6001));
  file_read((u32)buffer,9);
  assert(request(file,0)==RC(2,6001));
  file_read((u32)buffer,8);
  assert(!request(file,0) && gr64(msg+32)==6 && !memcmp(GP(buffer),"hello\n",6));
  file_read((u32)buffer,8);
  gw64(msg+56,6);
  assert(!request(file,0) && gr64(msg+32)==0);
  file_read((u32)buffer,8);
  gw64(msg+56,(u64)-1);
  assert(request(file,0)==RC(2,6061));
  header(4,4,16,8);
  assert(!request(file,0) && gr64(msg+32)==6);
  assert(!h_close(file));

  header(5,0,16,8);
  assert(!request(fs,0));
  header(4,8,32,16);
  gw32(msg+16,(16<<16)|1);
  gw32(msg+20,1);
  gw32(msg+48,1); /* trailing bytes beyond domain data are not OpenFile args */
  assert(request(fs,1)==RC(2,6001));
  ipc_handle(&caller,fs,AS_SIZE,0x100);
  assert(caller.cpu.x[0]==0xCC01);
  header(0xfff00004,8,16,0x3ff);
  ipc_handle(&caller,fs,msg,0x100);
  assert(caller.cpu.x[0]==0xEE01);
  assert(!h_close(fs));
  return 0;
}

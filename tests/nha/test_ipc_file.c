/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
#include "native_hos.h"
#include <assert.h>
#include <errno.h>
#if !defined(_WIN32)
#include <dirent.h>
#endif

static const u64 msg=AS_BASE, path=AS_BASE+0x1000, buffer=AS_BASE+0x1020;
static KThread caller;
static int open_attempts, opened, reads, sizes, closed, legacy_opens;
static int fail_open, fail_read, fail_size, fail_close;

int __real_plat_file_open_read(PFile* file, const char* root, const char* path, PError* error);
int __real_plat_file_read_at(PFile* file, void* buf, u64 size, u64 off, u64* count, PError* error);
int __real_plat_file_size64(PFile* file, u64* size, PError* error);
int __real_plat_file_close(PFile* file, PError* error);
FILE* __real_fopen(const char* path, const char* mode);

static PError denied_error(void){
#if defined(_WIN32)
  return (PError){PERR_WIN32,ERROR_ACCESS_DENIED};
#else
  return (PError){PERR_ERRNO,EACCES};
#endif
}

static PError io_error(void){
#if defined(_WIN32)
  return (PError){PERR_WIN32,ERROR_READ_FAULT};
#else
  return (PError){PERR_ERRNO,EIO};
#endif
}

FILE* __wrap_fopen(const char* path, const char* mode){
  if(!strcmp(mode,"rb")) legacy_opens++;
  return __real_fopen(path,mode);
}

int __wrap_plat_file_open_read(PFile* file, const char* root, const char* path, PError* error){
  open_attempts++;
  if(fail_open){ *error=denied_error(); return PFILE_DENIED; }
  int rc=__real_plat_file_open_read(file,root,path,error);
  if(rc==PFILE_OK) opened++;
  return rc;
}

int __wrap_plat_file_read_at(PFile* file, void* buf, u64 size, u64 off, u64* count, PError* error){
  reads++;
  if(fail_read){ *error=io_error(); *count=0; return PFILE_IO; }
  return __real_plat_file_read_at(file,buf,size,off,count,error);
}

int __wrap_plat_file_size64(PFile* file, u64* size, PError* error){
  sizes++;
  if(fail_size){ *error=io_error(); return PFILE_IO; }
  return __real_plat_file_size64(file,size,error);
}

int __wrap_plat_file_close(PFile* file, PError* error){
#if defined(_WIN32)
  DWORD flags;
  assert(GetHandleInformation(file->handle,&flags));
#endif
  assert(!__real_plat_file_close(file,error));
#if defined(_WIN32)
  assert(!GetHandleInformation(file->handle,&flags) && GetLastError()==ERROR_INVALID_HANDLE);
#endif
  closed++;
  /* Ownership is consumed even when close reports a diagnostic error. */
  if(fail_close){ *error=io_error(); return PFILE_IO; }
  return PFILE_OK;
}

static unsigned resource_count(void){
#if defined(_WIN32)
  DWORD handles;
  assert(GetProcessHandleCount(GetCurrentProcess(),&handles));
  /* Wine 8 returns success with zero even while a file handle is live. */
  assert(handles || GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version"));
  return (unsigned)handles;
#else
  DIR* dir=opendir("/proc/self/fd");
  assert(dir);
  unsigned descriptors=0;
  for(struct dirent* entry; (entry=readdir(dir)); )
    if(entry->d_name[0]!='.') descriptors++;
  assert(!closedir(dir));
  return descriptors-1; /* exclude the directory opened for the count */
#endif
}

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

static void file_read(u64 addr, u64 count){
  header(0x01000004,0,32,16);
  gw32(msg+8,8);
  gw32(msg+12,(u32)addr);
  gw32(msg+16,(u32)((addr>>32)&15)<<28 | (u32)((addr>>36)&7)<<2);
  gw64(msg+64,count);
}

int main(void){
  assert(!nha_init());
  mem_init(); kernel_init(); svc_fs_init();
  caller.cpu.thread=&caller;
  tls_thread=&caller;
  assert(!mem_map(AS_BASE,0x2000,MS_NORMAL,PERM_RW));
  assert(!mem_map(AS_BASE+0x2000,0x1000,MS_NORMAL,PERM_R));

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
  unsigned before=resource_count();

  open_path();
  gw32(msg+12,(u32)(AS_BASE+0x3000));
  assert(request(fs,0)==RC(2,6002) && !open_attempts);
  open_path();
  memcpy(GP(path),"one.txt!",8);
  assert(request(fs,0)==RC(2,6002) && !open_attempts);
  memcpy(GP(path),"one.txt",8);
  open_path();
  fail_open=1;
  assert(request(fs,0)==RC(2,6400) && open_attempts==1 && !opened && !legacy_opens);
  fail_open=0;

  u32 handles[4096];
  KObj dummy={0};
  unsigned n=0;
  for(;n<4096 && (handles[n]=h_try_new(&dummy))!=0;n++){}
  assert(n>1 && n<4096);
  open_path();
  assert(request(fs,0)==RC(2,6709));
  assert(opened==1 && closed==1 && resource_count()==before);
  assert(!h_close(handles[--n]));
  open_path();
  assert(!request(fs,0));
  u32 file=gr32(msg+12);
  assert(opened==2 && resource_count()==before+(before!=0));
  while(n) assert(!h_close(handles[--n]));
  assert(dummy.refs==0);

  /* Out buffer validation runs before accessing the host file. */
  file_read(AS_SIZE,8);
  assert(request(file,0)==RC(2,6001));
  file_read(AS_BASE+0x2000,8);
  assert(request(file,0)==RC(2,6001));
  file_read(AS_BASE+0x1ffc,8);
  assert(request(file,0)==RC(2,6001));
  file_read(buffer,9);
  assert(request(file,0)==RC(2,6001));
  file_read(AS_SIZE,0);
  assert(!request(file,0) && !gr64(msg+32) && !reads);

  memset(GP(buffer),0xa5,8);
  file_read(buffer,8);
  fail_read=1;
  assert(request(file,0)==RC(2,5000) && reads==1);
  assert(gr64(buffer)==0xa5a5a5a5a5a5a5a5ULL);
  fail_read=0;
  file_read(buffer,8);
  assert(!request(file,0) && gr64(msg+32)==6 && !memcmp(GP(buffer),"hello\n",6));
  assert(gr64(buffer)>>48==0xa5a5);
  file_read(buffer,8);
  gw64(msg+56,6);
  assert(!request(file,0) && gr64(msg+32)==0);
  file_read(buffer,8);
  gw64(msg+56,0x100000003ULL);
  assert(!request(file,0) && !gr64(msg+32));
  file_read(buffer,8);
  gw64(msg+56,(u64)-1);
  assert(request(file,0)==RC(2,6061) && reads==4);
  file_read(buffer,8);
  gw64(msg+56,INT64_MAX);
  assert(request(file,0)==RC(2,3005) && reads==4);
  header(4,4,16,8);
  fail_size=1;
  assert(request(file,0)==RC(2,5000) && sizes==1);
  fail_size=0;
  header(4,4,16,8);
  assert(!request(file,0) && gr64(msg+32)==6);
  fail_close=1;
  assert(!h_close(file));
  assert(h_close(file)==0xE401 && closed==opened && !legacy_opens && resource_count()==before);

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
  fprintf(stderr,"NHA_TEST {\"host_open_attempts\":%d,\"host_opened\":%d,\"host_closed\":%d,"
          "\"legacy_opens\":%d,\"resources_before\":%u,\"resources_after\":%u,"
          "\"resource_count_available\":%d,\"access_error\":%u,\"io_error\":%u}\n",
          open_attempts,opened,closed,legacy_opens,before,resource_count(),before!=0,denied_error().code,io_error().code);
  return 0;
}

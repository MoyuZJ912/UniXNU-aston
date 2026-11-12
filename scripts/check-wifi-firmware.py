#!/usr/bin/env python3
"""Run the production firmware reader (package/WifiFirmware.c) against explicit mocks: the Data addon VFS path, and (aston) the root
image object "Wifi" through a mocked native_ufs_object_read with the object layout scripts/make-aston-wifi-object.py writes."""
from pathlib import Path
import hashlib,json,re,subprocess
root=Path(__file__).resolve().parents[1];source=(root/'package/WifiFirmware.c').read_text()
body=source[source.index('extern int native_ufs_object_read('):]
out=root/'build/wifi-firmware';out.mkdir(parents=True,exist_ok=True)
prefix=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#define OPENSSL_SUPPRESS_DEPRECATED
#include <openssl/sha.h>
#include "WifiFirmware.h"
#define weak_import weak
typedef void *vfs_context_t;typedef void *vnode_t;typedef char *caddr_t;
struct vnode_attr {uint64_t va_data_size;};
#define VATTR_INIT(p) memset(p,0,sizeof(*(p)))
#define VATTR_WANTED(p,f) ((void)0)
enum {FREAD=1,VREG=1,UIO_READ=1,UIO_SYSSPACE=2,IO_NOAUTH=4};
static unsigned opens,closes,allocations,frees,reads;static int present=1,regular=1,short_read,read_error,allocation_error,attribute_error;
static volatile int native_gpu_addon_stage;
static uint64_t size=64;static unsigned char file[128];
static vfs_context_t vfs_context_kernel(void){return (void *)1;}
static void *vfs_context_ucred(void *c){return c;}static void *vfs_context_proc(void *c){return c;}
static int vnode_open(const char *p,int flags,int a,int b,vnode_t *v,void *ctx){(void)a;(void)b;(void)ctx;assert(!strcmp(p,SHENG_WIFI_FIRMWARE_ROOT "/amss.bin")&&flags==FREAD);opens++;if(!present)return ENOENT;*v=(void *)1;return 0;}
static int vnode_getattr(vnode_t v,struct vnode_attr *a,void *ctx){(void)v;(void)ctx;a->va_data_size=size;return attribute_error;}
static int vnode_vtype(vnode_t v){(void)v;return regular?VREG:2;}
static void vnode_close(vnode_t v,int flags,void *ctx){(void)ctx;assert(v&&flags==FREAD);closes++;}
static void *IOMallocData(size_t n){if(allocation_error)return NULL;allocations++;return malloc(n);}
static void IOFreeData(void *p,size_t n){(void)n;assert(p);frees++;free(p);}
static int vn_rdwr(int op,vnode_t v,char *buf,int length,long offset,int space,int flags,void *cred,int *resid,void *proc){(void)v;(void)cred;(void)proc;assert(op==UIO_READ&&!offset&&space==UIO_SYSSPACE&&flags==IO_NOAUTH);reads++;if(read_error)return EIO;memcpy(buf,file,length);*resid=short_read;return 0;}
/* The root image object: absent unless a test builds one. */
static unsigned char *object;static uint64_t object_size;static unsigned object_reads;
int native_ufs_object_read(const char *name,uint64_t offset,void *dst,uint64_t bytes,uint64_t *osize){
 if(!object||strcmp(name,"Wifi"))return ENOENT;if(osize)*osize=object_size;if(!dst)return 0;
 assert(!(offset&4095));if(offset>object_size||bytes>object_size-offset)return EINVAL;object_reads++;memcpy(dst,object+offset,bytes);return 0;}
'''
vfs_tests=r'''
int main(void){
 for(unsigned i=0;i<sizeof file;i++)file[i]=(unsigned char)i;
 uint8_t *data;size_t length;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EAGAIN&&!data&&!length&&!opens);native_gpu_addon_stage=1;
 assert(aston_wifi_object_stats[0]==-1);
 assert(!sheng_wifi_firmware_read("amss.bin",&data,&length)&&length==64&&!memcmp(data,file,64));IOFreeData(data,length);
 unsigned before=opens;assert(sheng_wifi_firmware_read("../amss.bin",&data,&length)==ENOENT&&!data&&!length&&opens==before);
 assert(sheng_wifi_firmware_read("join.txt",&data,&length)==ENOENT&&opens==before);
 present=0;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==ENOENT&&!data&&!length);present=1;
 size=0;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EINVAL);size=32u*1024*1024+1u;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EINVAL);size=64;
 regular=0;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EINVAL);regular=1;
 attribute_error=EIO;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EIO);attribute_error=0;
 allocation_error=1;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==ENOMEM);allocation_error=0;
 short_read=1;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EIO&&!data&&!length);short_read=0;
 read_error=1;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EIO&&!data&&!length);
 assert(opens==closes+1&&allocations==frees);
 puts("PASS: Data firmware path without the root object: content/ownership, whitelist, absent/empty/oversized/nonregular input, VFS/allocation/short-read failures and cleanup");
}
'''
object_tests=r'''
static void put_file(unsigned char *h,unsigned index,const char *name,uint32_t offset,const unsigned char *bytes,uint32_t length){
 unsigned char *e=h+16+64*index;memset(e,0,64);strncpy((char *)e,name,24);memcpy(e+24,&offset,4);memcpy(e+28,&length,4);SHA256(bytes,length,e+32);}
static void seal(unsigned char *h){SHA256(h,4064,h+4064);}
int main(void){
 enum {A=5000,B=300};object_size=4096+8192+4096;object=calloc(1,object_size);unsigned char *h=object;
 memcpy(h,"AWF1",4);uint32_t n=2;memcpy(h+4,&n,4);
 for(unsigned i=0;i<A;i++)object[4096+i]=(unsigned char)(i*7);for(unsigned i=0;i<B;i++)object[4096+8192+i]=(unsigned char)(255-i);
 put_file(h,0,"amss.bin",4096,object+4096,A);put_file(h,1,"board.bin",4096+8192,object+4096+8192,B);seal(h);
 uint8_t *data;size_t length;
 /* Served from the object before the Data addon exists, and checked against its digest. */
 assert(!sheng_wifi_firmware_read("amss.bin",&data,&length)&&length==A&&!memcmp(data,object+4096,A)&&!opens);IOFreeData(data,length);
 assert(aston_wifi_object_stats[0]==1&&aston_wifi_object_stats[1]==2&&aston_wifi_object_stats[2]==1);
 assert(!sheng_wifi_firmware_read("board.bin",&data,&length)&&length==B&&!memcmp(data,object+4096+8192,B));IOFreeData(data,length);
 /* A whitelisted name the object lacks is absent: no Data fallback once the object is valid. */
 native_gpu_addon_stage=1;assert(sheng_wifi_firmware_read("m3.bin",&data,&length)==ENOENT&&!data&&!opens);
 /* A changed byte fails its digest and is not handed out. */
 object[4096+17]^=1;unsigned before=allocations-frees;
 assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==EIO&&!data&&!length&&aston_wifi_object_stats[3]==1&&allocations-frees==before);
 object[4096+17]^=1;
 allocation_error=1;assert(sheng_wifi_firmware_read("amss.bin",&data,&length)==ENOMEM);allocation_error=0;
 puts("PASS: root image object: digest-checked files, no Data fallback for absent names, corrupted file refused, allocation failure");
 return 0;
}
'''
bad_header_tests=r'''
int main(void){
 object_size=8192;object=calloc(1,object_size);memcpy(object,"AWF1",4);uint32_t n=1;memcpy(object+4,&n,4);/* header digest left zero */
 for(unsigned i=0;i<sizeof file;i++)file[i]=(unsigned char)i;native_gpu_addon_stage=1;
 uint8_t *data;size_t length;assert(!sheng_wifi_firmware_read("amss.bin",&data,&length)&&length==64&&opens==1);IOFreeData(data,length);
 assert(aston_wifi_object_stats[0]==-4);
 puts("PASS: root image object with a bad header digest is ignored and the Data path serves");
 return 0;
}
'''
results=[]
for name,tests in (('vfs',vfs_tests),('object',object_tests),('bad-header',bad_header_tests)):
 (out/f'{name}.c').write_text(prefix+body+tests)
 subprocess.run(['clang','-std=c11','-O1','-g','-Wno-unknown-attributes','-fsanitize=address,undefined','-I',str(root/'package'),
                 str(out/f'{name}.c'),'-o',str(out/name),'-lcrypto'],check=True)
 r=subprocess.run([str(out/name)],capture_output=True,text=True,check=True);print(r.stdout,end='');results.append(r.stdout)
# The reference board pins its Data addon firmware here; aston has none (its firmware is the root image object).
manifest_path=root/'firmware/wcn7850/manifest.json'
manifest=json.loads(manifest_path.read_text())['files'] if manifest_path.is_file() else {}
for name,info in manifest.items():
 data=(root/'firmware/wcn7850'/name).read_bytes();assert len(data)==info['bytes'] and hashlib.sha256(data).hexdigest()==info['sha256']
assert '"oops"' not in (root/'package/WifiProbe.cpp').read_text()
(out/'report.json').write_text(json.dumps(dict(passed=True,vfs_simulated=True,object_simulated=True,hardware_verified=False,source_sha256=hashlib.sha256(source.encode()).hexdigest(),firmware_manifest=manifest,output=''.join(results)),indent=2)+'\n')

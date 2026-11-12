/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <IOKit/IOLib.h>
#include <libkern/crypto/sha2.h>
#include "WifiFirmware.h"

/* Set by the first overlay mount, after every Data addon file is installed. */
extern volatile int native_gpu_addon_stage;

/* aston: the firmware comes from the root image object "Wifi" (scripts/make-aston-wifi-object.py, packed into the unixnu image with
 * `unixnu build userdata --object Wifi=...`), so neither the WCN7850 firmware nor this phone's board data passes through the public
 * build. Block 0 of the object: "AWF1", u32 count, then from byte 16 one 64-byte entry per file (name[24] NUL padded, u32 offset, u32
 * size, sha256[32]), and the SHA-256 of bytes 0..4063 in bytes 4064..4095; files at 4096-byte aligned offsets. Every file read is
 * checked against its digest. Without the object the Data addon path below is used, as on the reference board. */
extern int native_ufs_object_read(const char *name, uint64_t offset, void *dst, uint64_t bytes, uint64_t *size) __attribute__((weak_import));

#define AWF_OBJECT "Wifi"
#define AWF_HEADER 4096u
#define AWF_MAX_FILE (32u * 1024 * 1024)

/* [0] object state (1 header valid; negative: -1 no object, -2 read, -3 magic or count, -4 header digest), [1] files in it,
 * [2] files served, [3] digest mismatches, [4] the last error */
volatile int32_t aston_wifi_object_stats[5];

static uint8_t *awf_header;

static int awf_open(void) {
  if (awf_header) return 0;
  if (aston_wifi_object_stats[0] < 0) return ENOENT;
  uint64_t size = 0;
  if (!native_ufs_object_read || native_ufs_object_read(AWF_OBJECT, 0, NULL, 0, &size) || size < AWF_HEADER) {
    aston_wifi_object_stats[0] = -1; return ENOENT;
  }
  uint8_t *header = IOMallocData(AWF_HEADER);
  if (!header) return ENOMEM;
  int state = 1;
  if (native_ufs_object_read(AWF_OBJECT, 0, header, AWF_HEADER, NULL)) state = -2;
  uint32_t count = 0;
  if (state == 1) {
    memcpy(&count, header + 4, 4);
    if (memcmp(header, "AWF1", 4) || !count || count > 8) state = -3;
  }
  if (state == 1) {
    uint8_t digest[SHA256_DIGEST_LENGTH];
    SHA256_CTX context;
    SHA256_Init(&context); SHA256_Update(&context, header, AWF_HEADER - 32); SHA256_Final(digest, &context);
    if (memcmp(digest, header + AWF_HEADER - 32, 32)) state = -4;
  }
  aston_wifi_object_stats[0] = state;
  if (state != 1) { IOFreeData(header, AWF_HEADER); return ENOENT; }
  aston_wifi_object_stats[1] = (int32_t)count;
  awf_header = header;
  return 0;
}

/* 0 and a new buffer the caller owns, or an errno; ENOENT when the object has no such file. */
static int awf_read(const char *name, uint8_t **data, size_t *size) {
  int error = awf_open();
  if (error) return error;
  uint32_t count; memcpy(&count, awf_header + 4, 4);
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *entry = awf_header + 16 + 64 * i;
    char entry_name[25]; memcpy(entry_name, entry, 24); entry_name[24] = 0;
    if (strcmp(entry_name, name)) continue;
    uint32_t offset, length; memcpy(&offset, entry + 24, 4); memcpy(&length, entry + 28, 4);
    if ((offset & (AWF_HEADER - 1)) || offset < AWF_HEADER || !length || length > AWF_MAX_FILE) return EINVAL;
    uint8_t *buffer = IOMallocData(length);
    if (!buffer) return ENOMEM;
    error = native_ufs_object_read(AWF_OBJECT, offset, buffer, length, NULL);
    if (!error) {
      uint8_t digest[SHA256_DIGEST_LENGTH];
      SHA256_CTX context;
      SHA256_Init(&context); SHA256_Update(&context, buffer, length); SHA256_Final(digest, &context);
      if (memcmp(digest, entry + 32, 32)) { error = EIO; aston_wifi_object_stats[3] = aston_wifi_object_stats[3] + 1; }
    }
    if (error) { IOFreeData(buffer, length); aston_wifi_object_stats[4] = error; return error; }
    aston_wifi_object_stats[2] = aston_wifi_object_stats[2] + 1;
    *data = buffer; *size = length; return 0;
  }
  return ENOENT;
}

/* Called on the wireless worker after the Data addon becomes available.
 * Successful ownership is transferred to its boot-lifetime firmware cache. */
int sheng_wifi_firmware_read(const char *name, uint8_t **data, size_t *size) {
  *data = NULL; *size = 0;
  if (strcmp(name, "amss.bin") && strcmp(name, "amss2.bin") && strcmp(name, "m3.bin") && strcmp(name, "board-2.bin") &&
      strcmp(name, "board.bin") && strcmp(name, "regdb.bin")) return ENOENT;
  const int from_object = awf_read(name, data, size);
  if (from_object != ENOENT || aston_wifi_object_stats[0] == 1) return from_object;
  if (native_gpu_addon_stage < 1) return EAGAIN;
  char path[160]; snprintf(path, sizeof(path), "%s/%s", SHENG_WIFI_FIRMWARE_ROOT, name);
  vfs_context_t ctx = vfs_context_kernel(); vnode_t vp = NULL;
  int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
  uint8_t *buffer = NULL; size_t length = 0;
  if (!error) {
    struct vnode_attr va; VATTR_INIT(&va); VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(vp, &va, ctx);
    if (!error && (vnode_vtype(vp) != VREG || !va.va_data_size || va.va_data_size > 32u * 1024 * 1024)) error = EINVAL;
    if (!error) {
      length = (size_t)va.va_data_size; buffer = IOMallocData(length);
      if (!buffer) error = ENOMEM;
      else {
        int resid = 0;
        error = vn_rdwr(UIO_READ, vp, (caddr_t)buffer, (int)length, 0, UIO_SYSSPACE,
            IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error && resid) error = EIO;
      }
    }
  }
  if (vp) vnode_close(vp, FREAD, ctx);
  if (error) { if (buffer) IOFreeData(buffer, length); return error; }
  *data = buffer; *size = length; return 0;
}

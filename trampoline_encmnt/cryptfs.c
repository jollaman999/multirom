/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* TO DO:
 *   1.  Perhaps keep several copies of the encrypted key, in case something
 *       goes horribly wrong?
 *
 */

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <linux/dm-ioctl.h>
#include <libgen.h>
#include <stdlib.h>
#include <sys/param.h>
#include <string.h>
#include <sys/mount.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <errno.h>
#include <linux/kdev_t.h>
#include "cryptfs.h"
#include "cutils/properties.h"
#include "crypto_scrypt.h"

#include <hardware/keymaster0.h>
#include <hardware/keymaster1.h>

#ifndef min /* already defined by windows.h */
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif

#define UNUSED __attribute__((unused))

#define UNUSED __attribute__((unused))

#ifdef CONFIG_HW_DISK_ENCRYPTION
#include "cryptfs_hw.h"
#endif

#define DM_CRYPT_BUF_SIZE 4096

#define HASH_COUNT 2000
#define KEY_LEN_BYTES 16
#define IV_LEN_BYTES 16

#define KEY_IN_FOOTER  "footer"

#define DEFAULT_HEX_PASSWORD "64656661756c745f70617373776f7264"
#define DEFAULT_PASSWORD "default_password"

#define CRYPTO_BLOCK_DEVICE "userdata"

#define BREADCRUMB_FILE "/data/misc/vold/convert_fde"

#define TABLE_LOAD_RETRIES 10

#define RSA_KEY_SIZE 2048
#define RSA_KEY_SIZE_BYTES (RSA_KEY_SIZE / 8)
#define RSA_EXPONENT 0x10001
#define KEYMASTER_CRYPTFS_RATE_LIMIT 1  // Maximum one try per second

#define RETRY_MOUNT_ATTEMPTS 20
#define RETRY_MOUNT_DELAY_SECONDS 1

char *me = "cryptfs";

static unsigned char saved_master_key[KEY_LEN_BYTES];
static char *saved_mount_point;
static int  master_key_saved = 0;
static struct crypt_persist_data *persist_data = NULL;
static char key_fname[PROPERTY_VALUE_MAX] = "";
static char real_blkdev[PROPERTY_VALUE_MAX] = "";
static char file_system[PROPERTY_VALUE_MAX] = "";

static int previous_type;

#ifdef CONFIG_HW_DISK_ENCRYPTION
static int scrypt_keymaster(const char *passwd, const unsigned char *salt,
                            unsigned char *ikey, void *params);
static void convert_key_to_hex_ascii(const unsigned char *master_key,
                                     unsigned int keysize, char *master_key_ascii);
static int put_crypt_ftr_and_key(struct crypt_mnt_ftr *crypt_ftr);
static int test_mount_hw_encrypted_fs(struct crypt_mnt_ftr* crypt_ftr,
                                   char *passwd, char *mount_point, char *label);
int cryptfs_check_passwd_hw(char *passwd);

static void convert_key_to_hex_ascii_for_upgrade(const unsigned char *master_key,
                                     unsigned int keysize, char *master_key_ascii)
{
    unsigned int i, a;
    unsigned char nibble;

    for (i = 0, a = 0; i < keysize; i++, a += 2) {
        /* For each byte, write out two ascii hex digits */
        nibble = (master_key[i] >> 4) & 0xf;
        master_key_ascii[a] = nibble + (nibble > 9 ? 0x57 : 0x30);

        nibble = master_key[i] & 0xf;
        master_key_ascii[a + 1] = nibble + (nibble > 9 ? 0x57 : 0x30);
    }

    /* Add the null termination */
    master_key_ascii[a] = '\0';
}

static int get_keymaster_hw_fde_passwd(const char* passwd, unsigned char* newpw,
                                  unsigned char* salt,
                                  const struct crypt_mnt_ftr *ftr)
{
    /* if newpw updated, return 0
     * if newpw not updated return -1
     */
    int rc = -1;

    if (should_use_keymaster()) {
        if (scrypt_keymaster(passwd, salt, newpw, (void*)ftr)) {
            printf("scrypt failed\n");
        } else {
            rc = 0;
        }
    }

    return rc;
}

static int verify_hw_fde_passwd(char *passwd, struct crypt_mnt_ftr* crypt_ftr)
{
    unsigned char newpw[32] = {0};
    int key_index;
    if (get_keymaster_hw_fde_passwd(passwd, newpw, crypt_ftr->salt, crypt_ftr))
        key_index = set_hw_device_encryption_key(passwd,
                                           (char*) crypt_ftr->crypto_type_name);
    else
        key_index = set_hw_device_encryption_key((const char*)newpw,
                                           (char*) crypt_ftr->crypto_type_name);
    return key_index;
}

static int verify_and_update_hw_fde_passwd(char *passwd,
                                           struct crypt_mnt_ftr* crypt_ftr)
{
    char* new_passwd = NULL;
    unsigned char newpw[32] = {0};
    int key_index = -1;
    int passwd_updated = -1;
    int ascii_passwd_updated = (crypt_ftr->flags & CRYPT_ASCII_PASSWORD_UPDATED);

    key_index = verify_hw_fde_passwd(passwd, crypt_ftr);
    if (ascii_passwd_updated) {
        printf("Ascii password was updated\n");
        if (key_index < 0)
            ++crypt_ftr->failed_decrypt_count;
    } else if (!ascii_passwd_updated && (key_index >= 0)) {
        crypt_ftr->flags |= CRYPT_ASCII_PASSWORD_UPDATED;
    } else {
        /* Code in else part would execute only once:
         * When device is upgraded from L->M release.
         * Once upgraded, code flow should never come here.
         * L release passed actual password in hex, so try with hex
         * Each nible of passwd was encoded as a byte, so allocate memory
         * twice of password len plus one more byte for null termination
         */
        if (crypt_ftr->crypt_type == CRYPT_TYPE_DEFAULT) {
             new_passwd = (char*)malloc(strlen(DEFAULT_HEX_PASSWORD) + 1);
             if (new_passwd == NULL) {
                printf("System out of memory. Password verification  incomplete\n");
                goto out;
             }
             strlcpy(new_passwd, DEFAULT_HEX_PASSWORD, strlen(DEFAULT_HEX_PASSWORD) + 1);
        } else {
             new_passwd = (char*)malloc(strlen(passwd) * 2 + 1);
             if (new_passwd == NULL) {
                printf("System out of memory. Password verification  incomplete\n");
                goto out;
             }
             convert_key_to_hex_ascii_for_upgrade((const unsigned char*)passwd,
                                    strlen(passwd), new_passwd);
        }
        key_index = set_hw_device_encryption_key((const char*)new_passwd,
                                    (char*) crypt_ftr->crypto_type_name);
        if (key_index >=0) {
            crypt_ftr->failed_decrypt_count = 0;
            printf("Hex password verified...will try to update with Ascii value\n");
            /* Before updating password, tie that with keymaster to tie with ROT */

            if (get_keymaster_hw_fde_passwd(passwd, newpw,
                                           crypt_ftr->salt, crypt_ftr)) {
                passwd_updated = update_hw_device_encryption_key(new_passwd,
                                     passwd, (char*)crypt_ftr->crypto_type_name);
            } else {
                passwd_updated = update_hw_device_encryption_key(new_passwd,
                                    (const char*)newpw, (char*)crypt_ftr->crypto_type_name);
            }

            if (passwd_updated >= 0) {
                crypt_ftr->flags |= CRYPT_ASCII_PASSWORD_UPDATED;
                printf("Ascii password recorded and updated\n");
            } else {
                printf("Passwd verified, could not update...Will try next time\n");
            }
        } else {
                ++crypt_ftr->failed_decrypt_count;
        }
        free(new_passwd);
     }
out:
    // update footer before leaving
    put_crypt_ftr_and_key(crypt_ftr);
    return key_index;
}
#endif

void set_partition_data(const char* block_device, const char* key_location, const char* fs)
{
  strcpy(key_fname, key_location);
  strcpy(real_blkdev, block_device);
  strcpy(file_system, fs);
}

#ifndef MINIVOLD // no HALs in recovery...
static int keymaster_init(keymaster0_device_t **keymaster0_dev,
                          keymaster1_device_t **keymaster1_dev)
{
    int rc;

    const hw_module_t* mod;
    rc = hw_get_module_by_class(KEYSTORE_HARDWARE_MODULE_ID, NULL, &mod);
    if (rc) {
        printf("could not find any keystore module\n");
        goto err;
    }

    printf("keymaster module name is %s\n", mod->name);
    printf("keymaster version is %d\n", mod->module_api_version);

    *keymaster0_dev = NULL;
    *keymaster1_dev = NULL;
    if (mod->module_api_version == KEYMASTER_MODULE_API_VERSION_1_0) {
        printf("Found keymaster1 module, using keymaster1 API.\n");
        rc = keymaster1_open(mod, keymaster1_dev);
    } else {
        printf("Found keymaster0 module, using keymaster0 API.\n");
        rc = keymaster0_open(mod, keymaster0_dev);
    }

    if (rc) {
        printf("could not open keymaster device in %s (%s)\n",
              KEYSTORE_HARDWARE_MODULE_ID, strerror(-rc));
        goto err;
    }

    return 0;

err:
    *keymaster0_dev = NULL;
    *keymaster1_dev = NULL;
    return rc;
}
#endif

/* This signs the given object using the keymaster key. */
static int keymaster_sign_object(struct crypt_mnt_ftr *ftr,
                                 const unsigned char *object,
                                 const size_t object_size,
                                 unsigned char **signature,
                                 size_t *signature_size)
{
#ifdef MINIVOLD // no HALs in recovery...
    return -1;
#else
    int rc = 0;
    keymaster0_device_t *keymaster0_dev = 0;
    keymaster1_device_t *keymaster1_dev = 0;
    if (keymaster_init(&keymaster0_dev, &keymaster1_dev)) {
        printf("Failed to init keymaster\n");
        rc = -1;
        goto out;
    }

    unsigned char to_sign[RSA_KEY_SIZE_BYTES];
    size_t to_sign_size = sizeof(to_sign);
    memset(to_sign, 0, RSA_KEY_SIZE_BYTES);

    // To sign a message with RSA, the message must satisfy two
    // constraints:
    //
    // 1. The message, when interpreted as a big-endian numeric value, must
    //    be strictly less than the public modulus of the RSA key.  Note
    //    that because the most significant bit of the public modulus is
    //    guaranteed to be 1 (else it's an (n-1)-bit key, not an n-bit
    //    key), an n-bit message with most significant bit 0 always
    //    satisfies this requirement.
    //
    // 2. The message must have the same length in bits as the public
    //    modulus of the RSA key.  This requirement isn't mathematically
    //    necessary, but is necessary to ensure consistency in
    //    implementations.
    switch (ftr->kdf_type) {
        case KDF_SCRYPT_KEYMASTER:
            // This ensures the most significant byte of the signed message
            // is zero.  We could have zero-padded to the left instead, but
            // this approach is slightly more robust against changes in
            // object size.  However, it's still broken (but not unusably
            // so) because we really should be using a proper deterministic
            // RSA padding function, such as PKCS1.
            memcpy(to_sign + 1, object, min(RSA_KEY_SIZE_BYTES - 1, object_size));
            printf("Signing safely-padded object\n");
            break;
        default:
            printf("Unknown KDF type %d\n", ftr->kdf_type);
            rc = -1;
            goto out;
    }

    if (keymaster0_dev) {
        keymaster_rsa_sign_params_t params;
        params.digest_type = DIGEST_NONE;
        params.padding_type = PADDING_NONE;

        rc = keymaster0_dev->sign_data(keymaster0_dev,
                                      &params,
                                      ftr->keymaster_blob,
                                      ftr->keymaster_blob_size,
                                      to_sign,
                                      to_sign_size,
                                      signature,
                                      signature_size);
        goto out;
    } else if (keymaster1_dev) {
        keymaster_key_blob_t key = { ftr->keymaster_blob, ftr->keymaster_blob_size };
        keymaster_key_param_t params[] = {
            keymaster_param_enum(KM_TAG_PADDING, KM_PAD_NONE),
            keymaster_param_enum(KM_TAG_DIGEST, KM_DIGEST_NONE),
        };
        keymaster_key_param_set_t param_set = { params, sizeof(params)/sizeof(*params) };
        keymaster_operation_handle_t op_handle;
        keymaster_error_t error = keymaster1_dev->begin(keymaster1_dev, KM_PURPOSE_SIGN, &key,
                                                        &param_set, NULL /* out_params */,
                                                        &op_handle);
        if (error == KM_ERROR_KEY_RATE_LIMIT_EXCEEDED) {
            // Key usage has been rate-limited.  Wait a bit and try again.
            sleep(KEYMASTER_CRYPTFS_RATE_LIMIT);
            error = keymaster1_dev->begin(keymaster1_dev, KM_PURPOSE_SIGN, &key,
                                          &param_set, NULL /* out_params */,
                                          &op_handle);
        }
        if (error != KM_ERROR_OK) {
            printf("Error starting keymaster signature transaction: %d\n", error);
            rc = -1;
            goto out;
        }

        keymaster_blob_t input = { to_sign, to_sign_size };
        size_t input_consumed;
        error = keymaster1_dev->update(keymaster1_dev, op_handle, NULL /* in_params */,
                                       &input, &input_consumed, NULL /* out_params */,
                                       NULL /* output */);
        if (error != KM_ERROR_OK) {
            printf("Error sending data to keymaster signature transaction: %d\n", error);
            rc = -1;
            goto out;
        }
        if (input_consumed != to_sign_size) {
            // This should never happen.  If it does, it's a bug in the keymaster implementation.
            printf("Keymaster update() did not consume all data.\n");
            keymaster1_dev->abort(keymaster1_dev, op_handle);
            rc = -1;
            goto out;
        }

        keymaster_blob_t tmp_sig;
        error = keymaster1_dev->finish(keymaster1_dev, op_handle, NULL /* in_params */,
                                       NULL /* verify signature */, NULL /* out_params */,
                                       &tmp_sig);
        if (error != KM_ERROR_OK) {
            printf("Error finishing keymaster signature transaction: %d\n", error);
            rc = -1;
            goto out;
        }

        *signature = (uint8_t*)tmp_sig.data;
        *signature_size = tmp_sig.data_length;
    } else {
        printf("Cryptfs bug: keymaster_init succeded but didn't initialize a device.\n");
        rc = -1;
        goto out;
    }

    out:
        if (keymaster1_dev)
            keymaster1_close(keymaster1_dev);
        if (keymaster0_dev)
            keymaster0_close(keymaster0_dev);

        return rc;
#endif
}

static void ioctl_init(struct dm_ioctl *io, size_t dataSize, const char *name, unsigned flags)
{
    memset(io, 0, dataSize);
    io->data_size = dataSize;
    io->data_start = sizeof(struct dm_ioctl);
    io->version[0] = 4;
    io->version[1] = 0;
    io->version[2] = 0;
    io->flags = flags;
    if (name) {
        strlcpy(io->name, name, sizeof(io->name));
    }
}

static unsigned int get_blkdev_size(int fd)
{
  unsigned int nr_sec;

  if ( (ioctl(fd, BLKGETSIZE, &nr_sec)) == -1) {
    nr_sec = 0;
  }

  return nr_sec;
}

static int get_crypt_ftr_info(char **metadata_fname, off64_t *off)
{
  static int cached_data = 0;
  static off64_t cached_off = 0;
  static char cached_metadata_fname[PROPERTY_VALUE_MAX] = "";
  int fd;
  unsigned int nr_sec;
  int rc = -1;

  if (!cached_data) {
    printf("get_crypt_ftr_info crypto key location: '%s'\n", key_fname);
    if (!strcmp(key_fname, KEY_IN_FOOTER)) {
      if ( (fd = open(real_blkdev, O_RDWR|O_CLOEXEC)) < 0) {
        printf("Cannot open real block device %s\n", real_blkdev);
        return -1;
      }

      if ((nr_sec = get_blkdev_size(fd))) {
        /* If it's an encrypted Android partition, the last 16 Kbytes contain the
         * encryption info footer and key, and plenty of bytes to spare for future
         * growth.
         */
        strlcpy(cached_metadata_fname, real_blkdev, sizeof(cached_metadata_fname));
        cached_off = ((off64_t)nr_sec * 512) - CRYPT_FOOTER_OFFSET;
        cached_data = 1;
      } else {
        printf("Cannot get size of block device %s\n", real_blkdev);
      }
      close(fd);
    } else {
      strlcpy(cached_metadata_fname, key_fname, sizeof(cached_metadata_fname));
      cached_off = 0;
      cached_data = 1;
    }
  }

  if (cached_data) {
    if (metadata_fname) {
        *metadata_fname = cached_metadata_fname;
    }
    if (off) {
        *off = cached_off;
    }
    rc = 0;
  }

  return rc;
}

/* Set sha256 checksum in structure */
static void set_ftr_sha(struct crypt_mnt_ftr *crypt_ftr)
{
    SHA256_CTX c;
    SHA256_Init(&c);
    memset(crypt_ftr->sha256, 0, sizeof(crypt_ftr->sha256));
    SHA256_Update(&c, crypt_ftr, sizeof(*crypt_ftr));
    SHA256_Final(crypt_ftr->sha256, &c);
}

/* key or salt can be NULL, in which case just skip writing that value.  Useful to
 * update the failed mount count but not change the key.
 */
static int put_crypt_ftr_and_key(struct crypt_mnt_ftr *crypt_ftr)
{
  int fd;
  unsigned int cnt;
  /* starting_off is set to the SEEK_SET offset
   * where the crypto structure starts
   */
  off64_t starting_off;
  int rc = -1;
  char *fname = NULL;
  struct stat statbuf;

  set_ftr_sha(crypt_ftr);

  if (get_crypt_ftr_info(&fname, &starting_off)) {
    printf("Unable to get crypt_ftr_info\n");
    return -1;
  }
  if (fname[0] != '/') {
    printf("Unexpected value for crypto key location\n");
    return -1;
  }
  if ( (fd = open(fname, O_RDWR | O_CREAT|O_CLOEXEC, 0600)) < 0) {
    printf("Cannot open footer file %s for put\n", fname);
    return -1;
  }

  /* Seek to the start of the crypt footer */
  if (lseek64(fd, starting_off, SEEK_SET) == -1) {
    printf("Cannot seek to real block device footer\n");
    goto errout;
  }

  if ((cnt = write(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr))) != sizeof(struct crypt_mnt_ftr)) {
    printf("Cannot write real block device footer\n");
    goto errout;
  }

  fstat(fd, &statbuf);
  /* If the keys are kept on a raw block device, do not try to truncate it. */
  if (S_ISREG(statbuf.st_mode)) {
    if (ftruncate(fd, 0x4000)) {
      printf("Cannot set footer file size\n");
      goto errout;
    }
  }

  /* Success! */
  rc = 0;

errout:
  close(fd);
  return rc;

}

static int get_crypt_ftr_and_key(struct crypt_mnt_ftr *crypt_ftr)
{
  int fd;
  unsigned int cnt;
  off64_t starting_off;
  int rc = -1;
  char *fname = NULL;
  struct stat statbuf;

  if (get_crypt_ftr_info(&fname, &starting_off)) {
    printf("Unable to get crypt_ftr_info\n");
    return -1;
  }
  if (fname[0] != '/') {
    printf("Unexpected value for crypto key location\n");
    return -1;
  }
  if ( (fd = open(fname, O_RDWR|O_CLOEXEC)) < 0) {
    printf("Cannot open footer file %s for get\n", fname);
    return -1;
  }

  /* Make sure it's 16 Kbytes in length */
  fstat(fd, &statbuf);
  if (S_ISREG(statbuf.st_mode) && (statbuf.st_size != 0x4000)) {
    printf("footer file %s is not the expected size!\n", fname);
    goto errout;
  }

  /* Seek to the start of the crypt footer */
  if (lseek64(fd, starting_off, SEEK_SET) == -1) {
    printf("Cannot seek to real block device footer\n");
    goto errout;
  }

  if ( (cnt = read(fd, crypt_ftr, sizeof(struct crypt_mnt_ftr))) != sizeof(struct crypt_mnt_ftr)) {
    printf("Cannot read real block device footer\n");
    goto errout;
  }

  if (crypt_ftr->magic != CRYPT_MNT_MAGIC) {
    printf("Bad magic for real block device %s\n", fname);
    goto errout;
  }

  if (crypt_ftr->major_version != CURRENT_MAJOR_VERSION) {
    printf("Cannot understand major version %d real block device footer; expected %d\n",
          crypt_ftr->major_version, CURRENT_MAJOR_VERSION);
    goto errout;
  }

  if (crypt_ftr->minor_version > CURRENT_MINOR_VERSION) {
    printf("Warning: crypto footer minor version %d, expected <= %d, continuing...\n",
          crypt_ftr->minor_version, CURRENT_MINOR_VERSION);
  }

  /* If this is a verion 1.0 crypt_ftr, make it a 1.1 crypt footer, and update the
   * copy on disk before returning.
   */
  /*if (crypt_ftr->minor_version < CURRENT_MINOR_VERSION) {
    upgrade_crypt_ftr(fd, crypt_ftr, starting_off);
  }*/

  /* Success! */
  rc = 0;

errout:
  close(fd);
  return rc;
}

/* Convert a binary key of specified length into an ascii hex string equivalent,
 * without the leading 0x and with null termination
 */
static void convert_key_to_hex_ascii(const unsigned char *master_key,
                                     unsigned int keysize, char *master_key_ascii) {
    unsigned int i, a;
    unsigned char nibble;

    for (i=0, a=0; i<keysize; i++, a+=2) {
        /* For each byte, write out two ascii hex digits */
        nibble = (master_key[i] >> 4) & 0xf;
        master_key_ascii[a] = nibble + (nibble > 9 ? 0x37 : 0x30);

        nibble = master_key[i] & 0xf;
        master_key_ascii[a+1] = nibble + (nibble > 9 ? 0x37 : 0x30);
    }

    /* Add the null termination */
    master_key_ascii[a] = '\0';

}

static int load_crypto_mapping_table(struct crypt_mnt_ftr *crypt_ftr,
        const unsigned char *master_key, const char *real_blk_name,
        const char *name, int fd, const char *extra_params) {
  _Alignas(struct dm_ioctl) char buffer[DM_CRYPT_BUF_SIZE];
  struct dm_ioctl *io;
  struct dm_target_spec *tgt;
  char *crypt_params;
  char master_key_ascii[129]; /* Large enough to hold 512 bit key and null */
  int i;

  io = (struct dm_ioctl *) buffer;

  /* Load the mapping table for this device */
  tgt = (struct dm_target_spec *) &buffer[sizeof(struct dm_ioctl)];

  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  io->target_count = 1;
  tgt->status = 0;
  tgt->sector_start = 0;
  tgt->length = crypt_ftr->fs_size;
  crypt_params = buffer + sizeof(struct dm_ioctl) + sizeof(struct dm_target_spec);

#ifdef CONFIG_HW_DISK_ENCRYPTION
  if(is_hw_disk_encryption((char*)crypt_ftr->crypto_type_name)) {
    strlcpy(tgt->target_type, "req-crypt",DM_MAX_TYPE_NAME);
    if (is_ice_enabled())
      convert_key_to_hex_ascii(master_key, sizeof(int), master_key_ascii);
    else
      convert_key_to_hex_ascii(master_key, crypt_ftr->keysize, master_key_ascii);
  }
  else {
    convert_key_to_hex_ascii(master_key, crypt_ftr->keysize, master_key_ascii);
    strlcpy(tgt->target_type, "crypt", DM_MAX_TYPE_NAME);
  }
#else
  convert_key_to_hex_ascii(master_key, crypt_ftr->keysize, master_key_ascii);
  strlcpy(tgt->target_type, "crypt", DM_MAX_TYPE_NAME);
#endif

  snprintf(crypt_params,
           sizeof(buffer)-sizeof(struct dm_ioctl)-sizeof(struct dm_target_spec),
           "%s %s 0 %s 0 %s 0",
           crypt_ftr->crypto_type_name, master_key_ascii,
           real_blk_name, extra_params);

  printf("target_type = %s\n", tgt->target_type);
  printf("real_blk_name = %s, extra_params = %s\n", real_blk_name, extra_params);

  crypt_params += strlen(crypt_params) + 1;
  crypt_params = (char *) (((unsigned long)crypt_params + 7) & ~8); /* Align to an 8 byte boundary */
  tgt->next = crypt_params - buffer;

  for (i = 0; i < TABLE_LOAD_RETRIES; i++) {
    if (! ioctl(fd, DM_TABLE_LOAD, io)) {
      break;
    }
    usleep(500000);
  }

  if (i == TABLE_LOAD_RETRIES) {
    /* We failed to load the table, return an error */
    return -1;
  } else {
    return i + 1;
  }
}

static int get_dm_crypt_version(int fd, const char *name,  int *version)
{
    char buffer[DM_CRYPT_BUF_SIZE];
    struct dm_ioctl *io;
    struct dm_target_versions *v;

    io = (struct dm_ioctl *) buffer;

    ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);

    if (ioctl(fd, DM_LIST_VERSIONS, io)) {
        return -1;
    }

    /* Iterate over the returned versions, looking for name of "crypt".
     * When found, get and return the version.
     */
    v = (struct dm_target_versions *) &buffer[sizeof(struct dm_ioctl)];
    while (v->next) {
        if (! strcmp(v->name, "crypt")) {

            /* We found the crypt driver, return the version, and get out */
            version[0] = v->version[0];
            version[1] = v->version[1];
            version[2] = v->version[2];
            return 0;
        }
        v = (struct dm_target_versions *)(((char *)v) + v->next);
    }

    return -1;
}

static int create_crypto_blk_dev(struct crypt_mnt_ftr *crypt_ftr,
        const unsigned char *master_key, const char *real_blk_name,
        char *crypto_blk_name, const char *name) {
  char buffer[DM_CRYPT_BUF_SIZE];
  struct dm_ioctl *io;
  unsigned int minor;
  int fd=0;
  int err;
  int retval = -1;
  int version[3];
  char *extra_params;
  int load_count;
#ifdef CONFIG_HW_DISK_ENCRYPTION
  char encrypted_state[PROPERTY_VALUE_MAX] = {0};
  char progress[PROPERTY_VALUE_MAX] = {0};
#endif

  if ((fd = open("/dev/device-mapper", O_RDWR|O_CLOEXEC)) < 0 ) {
    printf("Cannot open device-mapper\n");
    goto errout;
  }

  io = (struct dm_ioctl *) buffer;

  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  err = ioctl(fd, DM_DEV_CREATE, io);
  if (err) {
    printf("Cannot create dm-crypt device %s: %s\n", name, strerror(errno));
    goto errout;
  }

  /* Get the device status, in particular, the name of it's device file */
  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  if (ioctl(fd, DM_DEV_STATUS, io)) {
    printf("Cannot retrieve dm-crypt device status\n");
    goto errout;
  }
  minor = (io->dev & 0xff) | ((io->dev >> 12) & 0xfff00);
  snprintf(crypto_blk_name, MAXPATHLEN, "/dev/block/dm-%u", minor);

#ifdef CONFIG_HW_DISK_ENCRYPTION
  if(is_hw_disk_encryption((char*)crypt_ftr->crypto_type_name)) {
    /* Set fde_enabled if either FDE completed or in-progress */
    property_get("ro.crypto.state", encrypted_state, ""); /* FDE completed */
    property_get("vold.encrypt_progress", progress, ""); /* FDE in progress */
    if (!strcmp(encrypted_state, "encrypted") || strcmp(progress, "")) {
      if (is_ice_enabled())
          extra_params = "fde_enabled ice";
      else
        extra_params = "fde_enabled";
    } else
      extra_params = "fde_disabled";
  } else {
    extra_params = "";
    if (! get_dm_crypt_version(fd, name, version)) {
      /* Support for allow_discards was added in version 1.11.0 */
      if ((version[0] >= 2) ||
          ((version[0] == 1) && (version[1] >= 11))) {
          extra_params = "1 allow_discards";
          printf("Enabling support for allow_discards in dmcrypt.\n");
      }
    }
  }
#else
  extra_params = "";
  if (! get_dm_crypt_version(fd, name, version)) {
      /* Support for allow_discards was added in version 1.11.0 */
      if ((version[0] >= 2) ||
          ((version[0] == 1) && (version[1] >= 11))) {
          extra_params = "1 allow_discards";
          printf("Enabling support for allow_discards in dmcrypt.\n");
      }
  }
#endif

  load_count = load_crypto_mapping_table(crypt_ftr, master_key, real_blk_name, name,
                                         fd, extra_params);
  if (load_count < 0) {
      printf("Cannot load dm-crypt mapping table.\n");
      goto errout;
  } else if (load_count > 1) {
      printf("Took %d tries to load dmcrypt table.\n", load_count);
  }

  /* Resume this device to activate it */
  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);

  if (ioctl(fd, DM_DEV_SUSPEND, io)) {
    printf("Cannot resume the dm-crypt device\n");
    goto errout;
  }

  /* We made it here with no errors.  Woot! */
  retval = 0;

errout:
  close(fd);   /* If fd is <0 from a failed open call, it's safe to just ignore the close error */

  return retval;
}

static int delete_crypto_blk_dev(char *name)
{
  int fd;
  char buffer[DM_CRYPT_BUF_SIZE];
  struct dm_ioctl *io;
  int retval = -1;

  if ((fd = open("/dev/device-mapper", O_RDWR|O_CLOEXEC)) < 0 ) {
    printf("Cannot open device-mapper\n");
    goto errout;
  }

  io = (struct dm_ioctl *) buffer;

  ioctl_init(io, DM_CRYPT_BUF_SIZE, name, 0);
  if (ioctl(fd, DM_DEV_REMOVE, io)) {
    printf("Cannot remove dm-crypt device\n");
    goto errout;
  }

  /* We made it here with no errors.  Woot! */
  retval = 0;

errout:
  close(fd);    /* If fd is <0 from a failed open call, it's safe to just ignore the close error */

  return retval;

}

static int pbkdf2(const char *passwd, const unsigned char *salt,
                  unsigned char *ikey, void *params UNUSED)
{
    printf("Using pbkdf2 for cryptfs KDF\n");

    /* Turn the password into a key and IV that can decrypt the master key */
    return PKCS5_PBKDF2_HMAC_SHA1(passwd, strlen(passwd), salt, SALT_LEN,
                                  HASH_COUNT, KEY_LEN_BYTES + IV_LEN_BYTES,
                                  ikey) != 1;
}

static int scrypt(const char *passwd, const unsigned char *salt,
                  unsigned char *ikey, void *params)
{
    printf("Using scrypt for cryptfs KDF\n");

    struct crypt_mnt_ftr *ftr = (struct crypt_mnt_ftr *) params;

    int N = 1 << ftr->N_factor;
    int r = 1 << ftr->r_factor;
    int p = 1 << ftr->p_factor;

    /* Turn the password into a key and IV that can decrypt the master key */
    unsigned int keysize;
    crypto_scrypt((const uint8_t*)passwd, strlen(passwd),
                  salt, SALT_LEN, N, r, p, ikey,
                  KEY_LEN_BYTES + IV_LEN_BYTES);

   return 0;
}

static int scrypt_keymaster(const char *passwd, const unsigned char *salt,
                            unsigned char *ikey, void *params)
{
    printf("Using scrypt with keymaster for cryptfs KDF\n");

    int rc;
    size_t signature_size;
    unsigned char* signature;
    struct crypt_mnt_ftr *ftr = (struct crypt_mnt_ftr *) params;

    int N = 1 << ftr->N_factor;
    int r = 1 << ftr->r_factor;
    int p = 1 << ftr->p_factor;

    rc = crypto_scrypt((const uint8_t*)passwd, strlen(passwd),
                       salt, SALT_LEN, N, r, p, ikey,
                       KEY_LEN_BYTES + IV_LEN_BYTES);

    if (rc) {
        printf("scrypt failed\n");
        return -1;
    }

    if (keymaster_sign_object(ftr, ikey, KEY_LEN_BYTES + IV_LEN_BYTES,
                              &signature, &signature_size)) {
        printf("Signing failed\n");
        return -1;
    }

    rc = crypto_scrypt(signature, signature_size, salt, SALT_LEN,
                       N, r, p, ikey, KEY_LEN_BYTES + IV_LEN_BYTES);
    free(signature);

    if (rc) {
        printf("scrypt failed\n");
        return -1;
    }

    return 0;
}

static int decrypt_master_key_aux(const char *passwd, unsigned char *salt,
                                  unsigned char *encrypted_master_key,
                                  unsigned char *decrypted_master_key,
                                  kdf_func kdf, void *kdf_params,
                                  unsigned char** intermediate_key,
                                  size_t* intermediate_key_size)
{
  unsigned char ikey[32+32] = { 0 }; /* Big enough to hold a 256 bit key and 256 bit IV */
  EVP_CIPHER_CTX d_ctx;
  int decrypted_len, final_len;

  /* Turn the password into an intermediate key and IV that can decrypt the
     master key */
  if (kdf(passwd, salt, ikey, kdf_params)) {
    printf("kdf failed\n");
    return -1;
  }

  /* Initialize the decryption engine */
  EVP_CIPHER_CTX_init(&d_ctx);
  if (! EVP_DecryptInit_ex(&d_ctx, EVP_aes_128_cbc(), NULL, ikey, ikey+KEY_LEN_BYTES)) {
    return -1;
  }
  EVP_CIPHER_CTX_set_padding(&d_ctx, 0); /* Turn off padding as our data is block aligned */
  /* Decrypt the master key */
  if (! EVP_DecryptUpdate(&d_ctx, decrypted_master_key, &decrypted_len,
                            encrypted_master_key, KEY_LEN_BYTES)) {
    return -1;
  }
  if (! EVP_DecryptFinal_ex(&d_ctx, decrypted_master_key + decrypted_len, &final_len)) {
    return -1;
  }

  if (decrypted_len + final_len != KEY_LEN_BYTES) {
    return -1;
  }

  /* Copy intermediate key if needed by params */
  if (intermediate_key && intermediate_key_size) {
    *intermediate_key = (unsigned char*) malloc(KEY_LEN_BYTES);
    if (intermediate_key) {
      memcpy(*intermediate_key, ikey, KEY_LEN_BYTES);
      *intermediate_key_size = KEY_LEN_BYTES;
    }
  }

  return 0;
}

static void get_kdf_func(struct crypt_mnt_ftr *ftr, kdf_func *kdf, void** kdf_params)
{
    if (ftr->kdf_type == KDF_SCRYPT_KEYMASTER) {
        *kdf = scrypt_keymaster;
        *kdf_params = ftr;
    } else if (ftr->kdf_type == KDF_SCRYPT) {
        *kdf = scrypt;
        *kdf_params = ftr;
    } else {
        *kdf = pbkdf2;
        *kdf_params = NULL;
    }
}

static int decrypt_master_key(const char *passwd, unsigned char *decrypted_master_key,
                              struct crypt_mnt_ftr *crypt_ftr,
                              unsigned char** intermediate_key,
                              size_t* intermediate_key_size)
{
    kdf_func kdf;
    void *kdf_params;
    int ret;

    get_kdf_func(crypt_ftr, &kdf, &kdf_params);
    ret = decrypt_master_key_aux(passwd, crypt_ftr->salt, crypt_ftr->master_key,
                                 decrypted_master_key, kdf, kdf_params,
                                 intermediate_key, intermediate_key_size);
    if (ret != 0) {
        printf("failure decrypting master key\n");
    }

    return ret;
}

#ifdef CONFIG_HW_DISK_ENCRYPTION
static int test_mount_hw_encrypted_fs(struct crypt_mnt_ftr* crypt_ftr,
                                   char *passwd, char *mount_point, char *label)
{
  /* Allocate enough space for a 256 bit key, but we may use less */
  unsigned char decrypted_master_key[32];
  char crypto_blkdev[MAXPATHLEN];
  unsigned int orig_failed_decrypt_count;
  int rc = 0;

  printf("crypt_ftr->fs_size = %lld\n", crypt_ftr->fs_size);
  orig_failed_decrypt_count = crypt_ftr->failed_decrypt_count;

  int key_index = 0;
  if(is_hw_disk_encryption((char*)crypt_ftr->crypto_type_name)) {
    key_index = verify_and_update_hw_fde_passwd(passwd, crypt_ftr);
    if (key_index < 0) {
      rc = crypt_ftr->failed_decrypt_count;
      goto errout;
    }
    else {
      if (is_ice_enabled()) {
        if (create_crypto_blk_dev(crypt_ftr, (unsigned char*)&key_index,
                            real_blkdev, crypto_blkdev, label)) {
          printf("Error creating decrypted block device\n");
          rc = -1;
          goto errout;
        }
      } else {
        if (create_crypto_blk_dev(crypt_ftr, decrypted_master_key,
                            real_blkdev, crypto_blkdev, label)) {
          printf("Error creating decrypted block device\n");
          rc = -1;
          goto errout;
        }
      }
    }
  }

  if (rc == 0) {
    crypt_ftr->failed_decrypt_count = 0;
    if (orig_failed_decrypt_count != 0) {
      put_crypt_ftr_and_key(crypt_ftr);
    }

    /* Save the name of the crypto block device
     * so we can mount it when restarting the framework. */
    property_set("ro.crypto.fs_crypto_blkdev", crypto_blkdev);
    master_key_saved = 1;
  }

 errout:
  return rc;
}
#endif


static int test_mount_encrypted_fs(struct crypt_mnt_ftr* crypt_ftr,
                                   char *passwd, char *mount_point, char *label)
{
  /* Allocate enough space for a 256 bit key, but we may use less */
  unsigned char decrypted_master_key[32];
  char crypto_blkdev[MAXPATHLEN];
  char tmp_mount_point[64];
  unsigned int orig_failed_decrypt_count;
  int rc;
  int use_keymaster = 0;
  int upgrade = 0;
  unsigned char* intermediate_key = 0;
  size_t intermediate_key_size = 0;

  printf("crypt_ftr->fs_size = %lld\n", crypt_ftr->fs_size);
  orig_failed_decrypt_count = crypt_ftr->failed_decrypt_count;

  if (! (crypt_ftr->flags & CRYPT_MNT_KEY_UNENCRYPTED) ) {
    if (decrypt_master_key(passwd, decrypted_master_key, crypt_ftr,
                           &intermediate_key, &intermediate_key_size)) {
      printf("Failed to decrypt master key\n");
      rc = -1;
      goto errout;
    }
  }

  // Create crypto block device - all (non fatal) code paths
  // need it
  if (create_crypto_blk_dev(crypt_ftr, decrypted_master_key,
                            real_blkdev, crypto_blkdev, label)) {
     printf("Error creating decrypted block device\n");
     rc = -1;
     goto errout;
  }

  /* Work out if the problem is the password or the data */
  unsigned char scrypted_intermediate_key[sizeof(crypt_ftr->
                                                 scrypted_intermediate_key)];
  int N = 1 << crypt_ftr->N_factor;
  int r = 1 << crypt_ftr->r_factor;
  int p = 1 << crypt_ftr->p_factor;

  rc = crypto_scrypt(intermediate_key, intermediate_key_size,
                     crypt_ftr->salt, sizeof(crypt_ftr->salt),
                     N, r, p, scrypted_intermediate_key,
                     sizeof(scrypted_intermediate_key));

  // Does the key match the crypto footer?
  if (rc == 0 && memcmp(scrypted_intermediate_key,
                        crypt_ftr->scrypted_intermediate_key,
                        sizeof(scrypted_intermediate_key)) == 0) {
    printf("Password matches\n");
    rc = 0;
  } else {
    /* Try mounting the file system anyway, just in case the problem's with
     * the footer, not the key. */
    sprintf(tmp_mount_point, "%s/tmp_mnt", mount_point);
    mkdir(tmp_mount_point, 0755);
    if (mount(crypto_blkdev, tmp_mount_point, file_system, 0, NULL) != 0) {
      printf("Error temp mounting decrypted block device '%s'\n", crypto_blkdev);
      delete_crypto_blk_dev(label);

      rc = ++crypt_ftr->failed_decrypt_count;
      put_crypt_ftr_and_key(crypt_ftr);
    } else {
      /* Success! */
      printf("Password did not match but decrypted drive mounted - continue\n");
      umount(tmp_mount_point);
      rc = 0;
    }
  }

  if (rc == 0) {
    /* Save the name of the crypto block device
     * so we can mount it when restarting the framework. */
    property_set("ro.crypto.fs_crypto_blkdev", crypto_blkdev);
  }

 errout:
  if (intermediate_key) {
    memset(intermediate_key, 0, intermediate_key_size);
    free(intermediate_key);
  }
  return rc;
}

/*
 * Called by vold when it's asked to mount an encrypted external
 * storage volume. The incoming partition has no crypto header/footer,
 * as any metadata is been stored in a separate, small partition.
 *
 * out_crypto_blkdev must be MAXPATHLEN.
 */
int cryptfs_setup_ext_volume(const char* label, const char* real_blkdev,
        const unsigned char* key, int keysize, char* out_crypto_blkdev) {
    int fd = open(real_blkdev, O_RDONLY|O_CLOEXEC);
    if (fd == -1) {
        printf("Failed to open %s: %s\n", real_blkdev, strerror(errno));
        return -1;
    }

    unsigned long nr_sec = 0;
    nr_sec = get_blkdev_size(fd);
    close(fd);

    if (nr_sec == 0) {
        printf("Failed to get size of %s: %s\n", real_blkdev, strerror(errno));
        return -1;
    }

    struct crypt_mnt_ftr ext_crypt_ftr;
    memset(&ext_crypt_ftr, 0, sizeof(ext_crypt_ftr));
    ext_crypt_ftr.fs_size = nr_sec;
    ext_crypt_ftr.keysize = keysize;
    strcpy((char*) ext_crypt_ftr.crypto_type_name, "aes-cbc-essiv:sha256");

    return create_crypto_blk_dev(&ext_crypt_ftr, key, real_blkdev,
            out_crypto_blkdev, label);
}

/*
 * Called by vold when it's asked to unmount an encrypted external
 * storage volume.
 */
int cryptfs_revert_ext_volume(const char* label) {
    return delete_crypto_blk_dev((char*) label);
}

int check_unmounted_and_get_ftr(struct crypt_mnt_ftr* crypt_ftr)
{
    char encrypted_state[PROPERTY_VALUE_MAX];
    property_get("ro.crypto.state", encrypted_state, "");
    if ( master_key_saved || strcmp(encrypted_state, "encrypted") ) {
        printf("encrypted fs already validated or not running with encryption,"
              " aborting\n");
        //return -1;
    }

    if (get_crypt_ftr_and_key(crypt_ftr)) {
        printf("Error getting crypt footer and key\n");
        return -1;
    }

    return 0;
}

#ifdef CONFIG_HW_DISK_ENCRYPTION
int cryptfs_check_passwd_hw(char* passwd)
{
    struct crypt_mnt_ftr crypt_ftr;
    int rc;

    /* get key */
    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        printf("Error getting crypt footer and key\n");
        return -1;
    }

    rc = test_mount_hw_encrypted_fs(&crypt_ftr, passwd,
            DATA_MNT_POINT, CRYPTO_BLOCK_DEVICE);

    return rc;
}
#endif

int cryptfs_check_passwd(char *passwd)
{
    struct crypt_mnt_ftr crypt_ftr;
    int rc;

    if (!passwd) {
        printf("cryptfs_check_passwd: passwd is NULL!\n");
        return -1;
    }

    rc = check_unmounted_and_get_ftr(&crypt_ftr);
    if (rc) {
        printf("Could not get footer\n");
        return rc;
    }

#ifdef CONFIG_HW_DISK_ENCRYPTION
    if (is_hw_disk_encryption((char*)crypt_ftr.crypto_type_name))
        return cryptfs_check_passwd_hw(passwd);
#endif

    rc = test_mount_encrypted_fs(&crypt_ftr, passwd,
                                 DATA_MNT_POINT, CRYPTO_BLOCK_DEVICE);

    // try falling back to Lollipop hex passwords
    if (rc) {
        int hex_pass_len = strlen(passwd) * 2 + 1;
        char *hex_passwd = (char *)malloc(hex_pass_len);
        if (hex_passwd) {
            convert_key_to_hex_ascii((unsigned char *)passwd,
                                   strlen(passwd), hex_passwd);
            rc = test_mount_encrypted_fs(&crypt_ftr, hex_passwd,
                                DATA_MNT_POINT, "userdata");
            memset(hex_passwd, 0, hex_pass_len);
            free(hex_passwd);
        }
    }

    if (rc) {
        printf("Password did not match\n");
        return rc;
    }

    return rc;
}

int cryptfs_verify_passwd(char *passwd)
{
    struct crypt_mnt_ftr crypt_ftr;
    /* Allocate enough space for a 256 bit key, but we may use less */
    unsigned char decrypted_master_key[32];
    char encrypted_state[PROPERTY_VALUE_MAX];
    int rc;

    property_get("ro.crypto.state", encrypted_state, "");
    if (strcmp(encrypted_state, "encrypted") ) {
        printf("device not encrypted, aborting\n");
        return -2;
    }

    if (!master_key_saved) {
        printf("encrypted fs not yet mounted, aborting\n");
        return -1;
    }

    if (!saved_mount_point) {
        printf("encrypted fs failed to save mount point, aborting\n");
        return -1;
    }

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        printf("Error getting crypt footer and key\n");
        return -1;
    }

    if (crypt_ftr.flags & CRYPT_MNT_KEY_UNENCRYPTED) {
        /* If the device has no password, then just say the password is valid */
        rc = 0;
    } else {
#ifdef CONFIG_HW_DISK_ENCRYPTION
        if(is_hw_disk_encryption((char*)crypt_ftr.crypto_type_name)) {
            if (verify_hw_fde_passwd(passwd, &crypt_ftr) >= 0)
              rc = 0;
            else
              rc = -1;
        } else {
            decrypt_master_key(passwd, decrypted_master_key, &crypt_ftr, 0, 0);
            if (!memcmp(decrypted_master_key, saved_master_key, crypt_ftr.keysize)) {
                /* They match, the password is correct */
                rc = 0;
            } else {
              /* If incorrect, sleep for a bit to prevent dictionary attacks */
                sleep(1);
                rc = 1;
            }
        }
#else
        decrypt_master_key(passwd, decrypted_master_key, &crypt_ftr, 0, 0);
        if (!memcmp(decrypted_master_key, saved_master_key, crypt_ftr.keysize)) {
            /* They match, the password is correct */
            rc = 0;
        } else {
            /* If incorrect, sleep for a bit to prevent dictionary attacks */
            sleep(1);
            rc = 1;
        }
#endif
    }

    return rc;
}

/* Returns type of the password, default, pattern, pin or password.
 */
int cryptfs_get_password_type(void)
{
    struct crypt_mnt_ftr crypt_ftr;

    if (get_crypt_ftr_and_key(&crypt_ftr)) {
        printf("Error getting crypt footer and key\n");
        return -1;
    }

    if (crypt_ftr.flags & CRYPT_INCONSISTENT_STATE) {
        return -1;
    }

    return crypt_ftr.crypt_type;
}

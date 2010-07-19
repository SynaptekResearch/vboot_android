/* Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Common functions between firmware and kernel verified boot.
 * (Firmware portion)
 */


#include "vboot_common.h"
#include "utility.h"


char* kVbootErrors[VBOOT_ERROR_MAX] = {
  "Success.",
  "Key block invalid.",
  "Key block signature failed.",
  "Key block hash failed.",
  "Public key invalid.",
  "Preamble invalid.",
  "Preamble signature check failed.",
};


uint64_t OffsetOf(const void *base, const void *ptr) {
  return (uint64_t)(size_t)ptr - (uint64_t)(size_t)base;
}


/* Helper functions to get data pointed to by a public key or signature. */
uint8_t* GetPublicKeyData(VbPublicKey* key) {
  return (uint8_t*)key + key->key_offset;
}

const uint8_t* GetPublicKeyDataC(const VbPublicKey* key) {
  return (const uint8_t*)key + key->key_offset;
}

uint8_t* GetSignatureData(VbSignature* sig) {
  return (uint8_t*)sig + sig->sig_offset;
}

const uint8_t* GetSignatureDataC(const VbSignature* sig) {
  return (const uint8_t*)sig + sig->sig_offset;
}


/* Helper functions to verify the data pointed to by a subfield is inside
 * the parent data.  Returns 0 if inside, 1 if error. */
int VerifyMemberInside(const void* parent, uint64_t parent_size,
                       const void* member, uint64_t member_size,
                       uint64_t member_data_offset,
                       uint64_t member_data_size) {
  uint64_t end = OffsetOf(parent, member);

  if (end > parent_size)
    return 1;

  if (end + member_size > parent_size)
    return 1;

  end += member_data_offset;
  if (end > parent_size)
    return 1;
  if (end + member_data_size > parent_size)
    return 1;

  return 0;
}


int VerifyPublicKeyInside(const void* parent, uint64_t parent_size,
                          const VbPublicKey* key) {
  return VerifyMemberInside(parent, parent_size,
                            key, sizeof(VbPublicKey),
                            key->key_offset, key->key_size);
}


int VerifySignatureInside(const void* parent, uint64_t parent_size,
                          const VbSignature* sig) {
  return VerifyMemberInside(parent, parent_size,
                            sig, sizeof(VbSignature),
                            sig->sig_offset, sig->sig_size);
}


void PublicKeyInit(VbPublicKey* key, uint8_t* key_data, uint64_t key_size) {
  key->key_offset = OffsetOf(key, key_data);
  key->key_size = key_size;
  key->algorithm = kNumAlgorithms; /* Key not present yet */
  key->key_version = 0;
}


int PublicKeyCopy(VbPublicKey* dest, const VbPublicKey* src) {
  if (dest->key_size < src->key_size)
    return 1;

  dest->key_size = src->key_size;
  dest->algorithm = src->algorithm;
  dest->key_version = src->key_version;
  Memcpy(GetPublicKeyData(dest), GetPublicKeyDataC(src), src->key_size);
  return 0;
}


RSAPublicKey* PublicKeyToRSA(const VbPublicKey* key) {
  RSAPublicKey *rsa;

  if (kNumAlgorithms <= key->algorithm) {
    VBDEBUG(("Invalid algorithm.\n"));
    return NULL;
  }
  if (RSAProcessedKeySize((int)key->algorithm) != (int)key->key_size) {
    VBDEBUG(("Wrong key size for algorithm\n"));
    return NULL;
  }

  rsa = RSAPublicKeyFromBuf(GetPublicKeyDataC(key), (int)key->key_size);
  if (!rsa)
    return NULL;

  rsa->algorithm = (int)key->algorithm;
  return rsa;
}


int VerifyData(const uint8_t* data, uint64_t size, const VbSignature *sig,
               const RSAPublicKey* key) {

  if (sig->sig_size != siglen_map[key->algorithm]) {
    VBDEBUG(("Wrong signature size for algorithm.\n"));
    return 1;
  }
  if (sig->data_size > size) {
    VBDEBUG(("Data buffer smaller than length of signed data.\n"));
    return 1;
  }

  if (!RSAVerifyBinary_f(NULL, key, data, sig->data_size,
                         GetSignatureDataC(sig), key->algorithm))
    return 1;

  return 0;
}


int VerifyDigest(const uint8_t* digest, const VbSignature *sig,
                 const RSAPublicKey* key) {

  if (sig->sig_size != siglen_map[key->algorithm]) {
    VBDEBUG(("Wrong signature size for algorithm.\n"));
    return 1;
  }

  if (!RSAVerifyBinaryWithDigest_f(NULL, key, digest,
                         GetSignatureDataC(sig), key->algorithm))
    return 1;

  return 0;
}


int KeyBlockVerify(const VbKeyBlockHeader* block, uint64_t size,
                   const VbPublicKey *key) {

  const VbSignature* sig;

  /* Sanity checks before attempting signature of data */
  if (SafeMemcmp(block->magic, KEY_BLOCK_MAGIC, KEY_BLOCK_MAGIC_SIZE)) {
    VBDEBUG(("Not a valid verified boot key block.\n"));
    return VBOOT_KEY_BLOCK_INVALID;
  }
  if (block->header_version_major != KEY_BLOCK_HEADER_VERSION_MAJOR) {
    VBDEBUG(("Incompatible key block header version.\n"));
    return VBOOT_KEY_BLOCK_INVALID;
  }
  if (size < block->key_block_size) {
    VBDEBUG(("Not enough data for key block.\n"));
    return VBOOT_KEY_BLOCK_INVALID;
  }

  /* Check signature or hash, depending on whether we have a key. */
  if (key) {
    /* Check signature */
    RSAPublicKey* rsa;
    int rv;

    sig = &block->key_block_signature;

    if (VerifySignatureInside(block, block->key_block_size, sig)) {
      VBDEBUG(("Key block signature off end of block\n"));
      return VBOOT_KEY_BLOCK_INVALID;
    }

    rsa = PublicKeyToRSA(key);
    if (!rsa) {
      VBDEBUG(("Invalid public key\n"));
      return VBOOT_PUBLIC_KEY_INVALID;
    }

    /* Make sure advertised signature data sizes are sane. */
    if (block->key_block_size < sig->data_size) {
      VBDEBUG(("Signature calculated past end of the block\n"));
      return VBOOT_KEY_BLOCK_INVALID;
    }
    rv = VerifyData((const uint8_t*)block, size, sig, rsa);
    RSAPublicKeyFree(rsa);
    if (rv)
      return VBOOT_KEY_BLOCK_SIGNATURE;
  } else {
    /* Check hash */
    uint8_t* header_checksum = NULL;
    int rv;

    sig = &block->key_block_checksum;

    if (VerifySignatureInside(block, block->key_block_size, sig)) {
      VBDEBUG(("Key block hash off end of block\n"));
      return VBOOT_KEY_BLOCK_INVALID;
    }
    if (sig->sig_size != SHA512_DIGEST_SIZE) {
      VBDEBUG(("Wrong hash size for key block.\n"));
      return VBOOT_KEY_BLOCK_INVALID;
    }

    header_checksum = DigestBuf((const uint8_t*)block, sig->data_size,
                                SHA512_DIGEST_ALGORITHM);
    rv = SafeMemcmp(header_checksum, GetSignatureDataC(sig),
                    SHA512_DIGEST_SIZE);
    Free(header_checksum);
    if (rv) {
      VBDEBUG(("Invalid key block hash.\n"));
      return VBOOT_KEY_BLOCK_HASH;
    }
  }

  /* Verify we signed enough data */
  if (sig->data_size < sizeof(VbKeyBlockHeader)) {
    VBDEBUG(("Didn't sign enough data\n"));
    return VBOOT_KEY_BLOCK_INVALID;
  }

  /* Verify data key is inside the block and inside signed data */
  if (VerifyPublicKeyInside(block, block->key_block_size, &block->data_key)) {
    VBDEBUG(("Data key off end of key block\n"));
    return VBOOT_KEY_BLOCK_INVALID;
  }
  if (VerifyPublicKeyInside(block, sig->data_size, &block->data_key)) {
    VBDEBUG(("Data key off end of signed data\n"));
    return VBOOT_KEY_BLOCK_INVALID;
  }

  /* Success */
  return VBOOT_SUCCESS;
}


int VerifyFirmwarePreamble(const VbFirmwarePreambleHeader* preamble,
                           uint64_t size, const RSAPublicKey* key) {

  const VbSignature* sig = &preamble->preamble_signature;

  /* Sanity checks before attempting signature of data */
  if (preamble->header_version_major !=
      FIRMWARE_PREAMBLE_HEADER_VERSION_MAJOR) {
    VBDEBUG(("Incompatible firmware preamble header version.\n"));
    return VBOOT_PREAMBLE_INVALID;
  }
  if (size < preamble->preamble_size) {
    VBDEBUG(("Not enough data for preamble.\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Check signature */
  if (VerifySignatureInside(preamble, preamble->preamble_size, sig)) {
    VBDEBUG(("Preamble signature off end of preamble\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Make sure advertised signature data sizes are sane. */
  if (preamble->preamble_size < sig->data_size) {
    VBDEBUG(("Signature calculated past end of the block\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  if (VerifyData((const uint8_t*)preamble, size, sig, key)) {
    VBDEBUG(("Preamble signature validation failed\n"));
    return VBOOT_PREAMBLE_SIGNATURE;
  }

  /* Verify we signed enough data */
  if (sig->data_size < sizeof(VbFirmwarePreambleHeader)) {
    VBDEBUG(("Didn't sign enough data\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Verify body signature is inside the block */
  if (VerifySignatureInside(preamble, preamble->preamble_size,
                            &preamble->body_signature)) {
    VBDEBUG(("Firmware body signature off end of preamble\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Verify kernel subkey is inside the block */
  if (VerifyPublicKeyInside(preamble, preamble->preamble_size,
                            &preamble->kernel_subkey)) {
    VBDEBUG(("Kernel subkey off end of preamble\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Success */
  return VBOOT_SUCCESS;
}


int VerifyKernelPreamble(const VbKernelPreambleHeader* preamble,
                         uint64_t size, const RSAPublicKey* key) {

  const VbSignature* sig = &preamble->preamble_signature;

  /* Sanity checks before attempting signature of data */
  if (preamble->header_version_major != KERNEL_PREAMBLE_HEADER_VERSION_MAJOR) {
    VBDEBUG(("Incompatible kernel preamble header version.\n"));
    return VBOOT_PREAMBLE_INVALID;
  }
  if (size < preamble->preamble_size) {
    VBDEBUG(("Not enough data for preamble.\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Check signature */
  if (VerifySignatureInside(preamble, preamble->preamble_size, sig)) {
    VBDEBUG(("Preamble signature off end of preamble\n"));
    return VBOOT_PREAMBLE_INVALID;
  }
  if (VerifyData((const uint8_t*)preamble, size, sig, key)) {
    VBDEBUG(("Preamble signature validation failed\n"));
    return VBOOT_PREAMBLE_SIGNATURE;
  }

  /* Verify we signed enough data */
  if (sig->data_size < sizeof(VbKernelPreambleHeader)) {
    VBDEBUG(("Didn't sign enough data\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Verify body signature is inside the block */
  if (VerifySignatureInside(preamble, preamble->preamble_size,
                            &preamble->body_signature)) {
    VBDEBUG(("Kernel body signature off end of preamble\n"));
    return VBOOT_PREAMBLE_INVALID;
  }

  /* Success */
  return VBOOT_SUCCESS;
}

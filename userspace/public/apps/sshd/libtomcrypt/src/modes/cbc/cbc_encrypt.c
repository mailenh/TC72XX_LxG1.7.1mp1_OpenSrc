/* LibTomCrypt, modular cryptographic library -- Tom St Denis
 *
 * LibTomCrypt is a library that provides various cryptographic
 * algorithms in a highly modular and flexible manner.
 *
 * The library is free for all purposes without any express
 * guarantee it works.
 *
 * Tom St Denis, tomstdenis@gmail.com, http://libtomcrypt.org
 */
#include "tomcrypt.h"

/**
   @file cbc_encrypt.c
   CBC implementation, encrypt block, Tom St Denis
*/


#ifdef CBC

/**
  CBC encrypt
  @param pt     Plaintext
  @param ct     [out] Ciphertext
  @param len    The number of bytes to process (must be multiple of block length)
  @param cbc    CBC state
  @return CRYPT_OK if successful
*/
int cbc_encrypt(const unsigned char *pt, unsigned char *ct, unsigned long len, symmetric_CBC *cbc)
{
   int x, err;

   LTC_ARGCHK(pt != NULL);
   LTC_ARGCHK(ct != NULL);
   LTC_ARGCHK(cbc != NULL);

   if ((err = cipher_is_valid(cbc->cipher)) != CRYPT_OK) {
       return err;
   }
   
   /* is blocklen valid? */
   if (cbc->blocklen < 0 || cbc->blocklen > (int)sizeof(cbc->IV)) {
      return CRYPT_INVALID_ARG;
   }    

   if (len % cbc->blocklen) {
      return CRYPT_INVALID_ARG;
   }
#ifdef LTC_FAST
   if (len % sizeof(LTC_FAST_TYPE)) {   
      return CRYPT_INVALID_ARG;
   }
#endif

   if (cipher_descriptor[cbc->cipher].accel_cbc_encrypt != NULL) {
      cipher_descriptor[cbc->cipher].accel_cbc_encrypt(pt, ct, len / cbc->blocklen, cbc->IV, &cbc->key);
   } else {
      while (len) {
         /* xor IV against plaintext */
         #if defined(LTC_FAST)
	     for (x = 0; x < cbc->blocklen; x += sizeof(LTC_FAST_TYPE)) {
	         *((LTC_FAST_TYPE*)((unsigned char *)cbc->IV + x)) ^= *((LTC_FAST_TYPE*)((unsigned char *)pt + x));
	     }
	 #else 
            for (x = 0; x < cbc->blocklen; x++) {
               cbc->IV[x] ^= pt[x];
            }
	 #endif

         /* encrypt */
         cipher_descriptor[cbc->cipher].ecb_encrypt(cbc->IV, ct, &cbc->key);

        /* store IV [ciphertext] for a future block */
         #if defined(LTC_FAST)
	     for (x = 0; x < cbc->blocklen; x += sizeof(LTC_FAST_TYPE)) {
	         *((LTC_FAST_TYPE*)((unsigned char *)cbc->IV + x)) = *((LTC_FAST_TYPE*)((unsigned char *)ct + x));
	     }
	 #else 
             for (x = 0; x < cbc->blocklen; x++) {
                cbc->IV[x] = ct[x];
             }
	 #endif
        
        ct  += cbc->blocklen;
        pt  += cbc->blocklen;
        len -= cbc->blocklen;
     }
   }
   return CRYPT_OK;
}

#endif

/* $Source: /cvs/cable/Lion/userspace/public/apps/sshd/libtomcrypt/src/modes/cbc/cbc_encrypt.c,v $ */
/* $Revision: 1.2 $ */
/* $Date: 2014/11/19 09:14:08 $ */

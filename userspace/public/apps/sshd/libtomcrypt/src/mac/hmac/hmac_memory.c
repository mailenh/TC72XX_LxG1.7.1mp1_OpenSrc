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
  @file hmac_memory.c
  HMAC support, process a block of memory, Tom St Denis/Dobes Vandermeer
*/

#ifdef HMAC

/**
   HMAC a block of memory to produce the authentication tag
   @param hash      The index of the hash to use 
   @param key       The secret key 
   @param keylen    The length of the secret key (octets)
   @param in        The data to HMAC
   @param inlen     The length of the data to HMAC (octets)
   @param out       [out] Destination of the authentication tag
   @param outlen    [in/out] Max size and resulting size of authentication tag
   @return CRYPT_OK if successful
*/
int hmac_memory(int hash, 
                const unsigned char *key,  unsigned long keylen,
                const unsigned char *in,   unsigned long inlen, 
                      unsigned char *out,  unsigned long *outlen)
{
    hmac_state *hmac;
    int err;

    LTC_ARGCHK(key    != NULL);
    LTC_ARGCHK(in   != NULL);
    LTC_ARGCHK(out    != NULL); 
    LTC_ARGCHK(outlen != NULL);

    /* allocate ram for hmac state */
    hmac = XMALLOC(sizeof(hmac_state));
    if (hmac == NULL) {
       return CRYPT_MEM;
    }

    if ((err = hmac_init(hmac, hash, key, keylen)) != CRYPT_OK) {
       goto LBL_ERR;
    }

    if ((err = hmac_process(hmac, in, inlen)) != CRYPT_OK) {
       goto LBL_ERR;
    }

    if ((err = hmac_done(hmac, out, outlen)) != CRYPT_OK) {
       goto LBL_ERR;
    }

   err = CRYPT_OK;
LBL_ERR:
#ifdef LTC_CLEAN_STACK
   zeromem(hmac, sizeof(hmac_state));
#endif

   XFREE(hmac);
   return err;   
}

#endif


/* $Source: /cvs/cable/Lion/userspace/public/apps/sshd/libtomcrypt/src/mac/hmac/hmac_memory.c,v $ */
/* $Revision: 1.2 $ */
/* $Date: 2014/11/19 09:14:05 $ */

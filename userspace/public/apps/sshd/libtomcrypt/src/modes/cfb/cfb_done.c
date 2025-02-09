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
   @file cfb_done.c
   CFB implementation, finish chain, Tom St Denis
*/

#ifdef CFB

/** Terminate the chain
  @param cfb    The CFB chain to terminate
  @return CRYPT_OK on success
*/
int cfb_done(symmetric_CFB *cfb)
{
   int err;
   LTC_ARGCHK(cfb != NULL);

   if ((err = cipher_is_valid(cfb->cipher)) != CRYPT_OK) {
      return err;
   }
   cipher_descriptor[cfb->cipher].done(&cfb->key);
   return CRYPT_OK;
}

   

#endif

/* $Source: /cvs/cable/Lion/userspace/public/apps/sshd/libtomcrypt/src/modes/cfb/cfb_done.c,v $ */
/* $Revision: 1.2 $ */
/* $Date: 2014/11/19 09:14:09 $ */

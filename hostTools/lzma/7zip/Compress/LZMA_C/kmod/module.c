
/*
 * Copyright (C) 2006 Junjiro Okajima
 * Copyright (C) 2006 Tomas Matejicek, slax.org
 *
 * LICENSE follows the described one in lzma.txt.
 */

/* $Id: module.c,v 1.3 2015/06/09 09:02:16 wtchen Exp $ */

#include <linux/init.h>
#include <linux/module.h>

#include "../LzmaDecode.c"

EXPORT_SYMBOL(LzmaDecodeProperties);
EXPORT_SYMBOL(LzmaDecode);

#if 0
static int __init unlzma_init(void)
{
	return 0;
}

static void __exit unlzma_exit(void)
{
}

module_init(unlzma_init);
module_exit(unlzma_exit);
#endif

MODULE_LICENSE("GPL");
MODULE_VERSION("$Id: module.c,v 1.3 2015/06/09 09:02:16 wtchen Exp $");
MODULE_DESCRIPTION("LZMA uncompress. "
		   "A tiny wrapper for LzmaDecode.c in LZMA SDK from www.7-zip.org.");

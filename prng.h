/*******************************************************************************
 *
 * Copyright (c) 2011, 2012, 2013, 2014, 2015 Olaf Bergmann (TZI) and others.
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the Eclipse Public License v1.0
 * and Eclipse Distribution License v. 1.0 which accompanies this distribution.
 *
 * The Eclipse Public License is available at http://www.eclipse.org/legal/epl-v10.html
 * and the Eclipse Distribution License is available at 
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * Contributors:
 *    Olaf Bergmann  - initial API and implementation
 *    Hauke Mehrtens - memory optimization, ECC integration
 *
 *******************************************************************************/

/** 
 * @file prng.h
 * @brief Pseudo Random Numbers
 */

#ifndef _DTLS_PRNG_H_
#define _DTLS_PRNG_H_

#include "tinydtls.h"
#include "alert.h"

/** 
 * @defgroup prng Pseudo Random Numbers
 * @{
 */

#ifndef WITH_CONTIKI
#include <stdlib.h>
#include <stdio.h>

#ifdef __RTOS__

/*
 * brief:   Fills buf with len random bytes
 * param:   buf:  [IN/OUT]  buffer to store random data
 *          len:  [IN]      length of buf in bytes
 * return:  1:    success
 *         <0:    failure
 */
static int dtls_prng(unsigned char *buf, size_t len)
{
  if( rng_gen_random(buf, len) != 0 )
  {
    dtls_debug( "RNG failed" );
    return( dtls_alert_fatal_create(DTLS_ALERT_INTERNAL_ERROR) );
  }

  return( 1 );
}

/*
 * brief:   Initializes RNG
 * param:   seed: [IN]  unused param
 */
static void dtls_prng_init(unsigned short seed)
{
  (void)seed; /* unused param */

  rng_init();
}

#else /* __RTOS__ */

/**
 * Fills \p buf with \p len random bytes. This is the default
 * implementation for prng().  You might want to change prng() to use
 * a better PRNG on your specific platform.
 */
static inline int
dtls_prng(unsigned char *buf, size_t len) {
  FILE *urandom = fopen("/dev/urandom", "r");
  if (urandom == NULL) {
    return -1;
  }
  int status = (fread(buf, 1, len, urandom) == len)? 1: -1;
  fclose(urandom);
  return status;
}

static inline void
dtls_prng_init(unsigned short seed) {
  /* urandom initialized at system level */
}
#endif /* __RTOS__ */

#else /* WITH_CONTIKI */
#include <string.h>
#include "random.h"

#ifdef HAVE_PRNG
static inline int
dtls_prng(unsigned char *buf, size_t len)
{
	return contiki_prng_impl(buf, len);
}
#else
/**
 * Fills \p buf with \p len random bytes. This is the default
 * implementation for prng().  You might want to change prng() to use
 * a better PRNG on your specific platform.
 */
static inline int
dtls_prng(unsigned char *buf, size_t len) {
  unsigned short v = random_rand();
  while (len > sizeof(v)) {
    memcpy(buf, &v, sizeof(v));
    len -= sizeof(v);
    buf += sizeof(v);
    v = random_rand();
  }

  memcpy(buf, &v, len);
  return 1;
}
#endif /* HAVE_PRNG */

static inline void
dtls_prng_init(unsigned short seed) {
  /* random_init() messes with the radio interface of the CC2538 and
   * therefore must not be called after the radio has been
   * initialized. */
#ifndef CONTIKI_TARGET_CC2538DK
	random_init(seed);
#endif
}
#endif /* WITH_CONTIKI */

/** @} */

#endif /* _DTLS_PRNG_H_ */

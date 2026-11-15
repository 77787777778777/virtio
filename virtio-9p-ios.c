/* The stream adapter: the boundary 9pfs actually sees.

   9pfs opens the node once and dups the fd into two buffered FILE * streams,
   then drives them with short fread/fwrite calls, so this must behave like a
   byte pipe, not a message queue: writes accumulate until a 4-byte
   little-endian size prefix says the PDU is complete, and reads return as
   soon as any reply byte is available (short reads are mandatory, never
   wait for a full buffer).

   One request is in flight at a time; the accumulator lock serialises whole
   submit-to-reply cycles, while the reply FIFO has its own lock, so a reader
   thread never waits on the ring lock and a second writer simply blocks on
   the accumulator until the first cycle completes.  */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "virtio-9p.h"

static uint8_t *acc;
static size_t acc_len, acc_off;
static uint8_t *reply_buf;
static uint8_t *fifo;
static size_t fifo_len, fifo_off, fifo_cap;
static int ios_error;

static pthread_mutex_t acc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fifo_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fifo_data_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t fifo_space_cv = PTHREAD_COND_INITIALIZER;

error_t
v9p_ios_init (void)
{
  acc = malloc (V9P_TX_BUF_SIZE);
  reply_buf = malloc (V9P_RX_BUF_SIZE);
  fifo = malloc (V9P_RX_BUF_SIZE);
  if (!acc || !reply_buf || !fifo)
    return ENOMEM;
  fifo_cap = V9P_RX_BUF_SIZE;
  return 0;
}

static void
ios_fail (error_t err)
{
  ios_error = err;
  pthread_mutex_lock (&fifo_lock);
  pthread_cond_broadcast (&fifo_data_cv);
  pthread_cond_broadcast (&fifo_space_cv);
  pthread_mutex_unlock (&fifo_lock);
}

ssize_t
v9p_ios_write (const void *buf, size_t len)
{
  error_t err;

  pthread_mutex_lock (&acc_lock);
  if (acc_len - acc_off + len > V9P_TX_BUF_SIZE)
    {
      pthread_mutex_unlock (&acc_lock);
      errno = EMSGSIZE;
      return -1;
    }
  memcpy (acc + acc_len, buf, len);
  acc_len += len;

  for (;;)
    {
      uint32_t size, rlen;

      if (acc_len - acc_off < 4)
        break;
      size = acc[acc_off] | (acc[acc_off + 1] << 8)
             | (acc[acc_off + 2] << 16) | ((uint32_t) acc[acc_off + 3] << 24);
      if (size < 7 || size > V9P_TX_BUF_SIZE)
        {
          pthread_mutex_unlock (&acc_lock);
          ios_fail (EMSGSIZE);
          errno = EMSGSIZE;
          return -1;
        }
      if (acc_len - acc_off < size)
        break;

      err = v9p_exchange (acc + acc_off, size, reply_buf,
                          V9P_RX_BUF_SIZE, &rlen);
      if (err)
        {
          pthread_mutex_unlock (&acc_lock);
          ios_fail (err);
          errno = err;
          return -1;
        }

      pthread_mutex_lock (&fifo_lock);
      while (fifo_off + fifo_len + rlen > fifo_cap && ios_error == 0)
        pthread_cond_wait (&fifo_space_cv, &fifo_lock);
      memcpy (fifo + fifo_off + fifo_len, reply_buf, rlen);
      fifo_len += rlen;
      pthread_cond_broadcast (&fifo_data_cv);
      pthread_mutex_unlock (&fifo_lock);

      acc_off += size;
    }

  if (acc_off == acc_len)
    acc_off = acc_len = 0;
  else if (acc_off > 0)
    {
      memmove (acc, acc + acc_off, acc_len - acc_off);
      acc_len -= acc_off;
      acc_off = 0;
    }
  pthread_mutex_unlock (&acc_lock);
  return len;
}

ssize_t
v9p_ios_read (void *buf, size_t len)
{
  size_t n;

  pthread_mutex_lock (&fifo_lock);
  for (;;)
    {
      if (fifo_len > 0)
        break;
      if (ios_error)
        {
          pthread_mutex_unlock (&fifo_lock);
          errno = ios_error;
          return -1;
        }
      pthread_cond_wait (&fifo_data_cv, &fifo_lock);
    }
  n = fifo_len < len ? fifo_len : len;
  memcpy (buf, fifo + fifo_off, n);
  fifo_off += n;
  fifo_len -= n;
  if (fifo_len == 0)
    fifo_off = 0;
  pthread_cond_broadcast (&fifo_space_cv);
  pthread_mutex_unlock (&fifo_lock);
  return n;
}

int
v9p_ios_can_read (void)
{
  int r;
  pthread_mutex_lock (&fifo_lock);
  r = fifo_len > 0;
  pthread_mutex_unlock (&fifo_lock);
  return r;
}

int
v9p_ios_can_write (void)
{
  int r;
  pthread_mutex_lock (&acc_lock);
  r = acc_len - acc_off < V9P_TX_BUF_SIZE;
  pthread_mutex_unlock (&acc_lock);
  return r;
}

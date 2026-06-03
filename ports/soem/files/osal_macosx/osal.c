/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/* Darwin (macOS) OSAL for SOEM 2.0.
 *
 * Derived from osal/linux/osal.c.  macOS lacks clock_nanosleep(), so the
 * sleep primitives are implemented on top of nanosleep() instead, and the
 * mutex does not request PTHREAD_PRIO_INHERIT (pthread_mutexattr_setprotocol
 * returns ENOTSUP on macOS).  Everything else is identical to the Linux OSAL:
 * CLOCK_MONOTONIC / CLOCK_REALTIME are available since macOS 10.12.
 */
#include <osal.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Returns time from some unspecified moment in past,
 * strictly increasing, used for time intervals measurement. */
void osal_get_monotonic_time(ec_timet *ts)
{
   clock_gettime(CLOCK_MONOTONIC, ts);
}

ec_timet osal_current_time(void)
{
   struct timespec ts;

   clock_gettime(CLOCK_REALTIME, &ts);
   return ts;
}

void osal_time_diff(ec_timet *start, ec_timet *end, ec_timet *diff)
{
   osal_timespecsub(end, start, diff);
}

void osal_timer_start(osal_timert *self, uint32 timeout_usec)
{
   struct timespec start_time;
   struct timespec timeout;

   osal_get_monotonic_time(&start_time);
   osal_timespec_from_usec(timeout_usec, &timeout);
   osal_timespecadd(&start_time, &timeout, &self->stop_time);
}

boolean osal_timer_is_expired(osal_timert *self)
{
   struct timespec current_time;
   int is_not_yet_expired;

   osal_get_monotonic_time(&current_time);
   is_not_yet_expired = osal_timespeccmp(&current_time, &self->stop_time, <);

   return is_not_yet_expired == FALSE;
}

int osal_usleep(uint32 usec)
{
   struct timespec ts;
   struct timespec remain;

   osal_timespec_from_usec(usec, &ts);
   /* macOS has no clock_nanosleep; nanosleep is relative to CLOCK_MONOTONIC.
    * Resume across signal interrupts using the reported remaining time. */
   while (nanosleep(&ts, &remain) == -1 && errno == EINTR)
   {
      ts = remain;
   }
   return 0;
}

int osal_monotonic_sleep(ec_timet *ts)
{
   struct timespec now;
   struct timespec diff;
   struct timespec remain;

   /* Emulate clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME): sleep for the
    * interval between now and the absolute target.  Recomputing the delta from
    * the monotonic clock keeps this drift-free relative to the target. */
   clock_gettime(CLOCK_MONOTONIC, &now);
   if (osal_timespeccmp(ts, &now, <=))
   {
      return 0;
   }
   osal_timespecsub(ts, &now, &diff);

   while (nanosleep(&diff, &remain) == -1 && errno == EINTR)
   {
      diff = remain;
   }
   return 0;
}

void *osal_malloc(size_t size)
{
   return malloc(size);
}

void osal_free(void *ptr)
{
   free(ptr);
}

int osal_thread_create(void *thandle, int stacksize, void *func, void *param)
{
   int ret;
   pthread_attr_t attr;
   pthread_t *threadp;

   threadp = thandle;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, stacksize);
   ret = pthread_create(threadp, &attr, func, param);
   pthread_attr_destroy(&attr);
   if (ret < 0)
   {
      return 0;
   }
   return 1;
}

int osal_thread_create_rt(void *thandle, int stacksize, void *func, void *param)
{
   int ret;
   pthread_attr_t attr;
   struct sched_param schparam;
   pthread_t *threadp;

   threadp = thandle;
   pthread_attr_init(&attr);
   pthread_attr_setstacksize(&attr, stacksize);
   ret = pthread_create(threadp, &attr, func, param);
   pthread_attr_destroy(&attr);
   if (ret < 0)
   {
      return 0;
   }
   /* Best-effort real-time scheduling.  macOS honours SCHED_FIFO only with
    * elevated privileges; ignore failure so the thread still runs. */
   memset(&schparam, 0, sizeof(schparam));
   schparam.sched_priority = sched_get_priority_max(SCHED_FIFO);
   pthread_setschedparam(*threadp, SCHED_FIFO, &schparam);

   return 1;
}

void *osal_mutex_create(void)
{
   pthread_mutexattr_t mutexattr;
   osal_mutext *mutex;
   mutex = (osal_mutext *)osal_malloc(sizeof(osal_mutext));
   if (mutex)
   {
      /* PTHREAD_PRIO_INHERIT is unsupported on macOS (ENOTSUP); use defaults. */
      pthread_mutexattr_init(&mutexattr);
      pthread_mutex_init(mutex, &mutexattr);
      pthread_mutexattr_destroy(&mutexattr);
   }
   return (void *)mutex;
}

void osal_mutex_destroy(void *mutex)
{
   pthread_mutex_destroy((osal_mutext *)mutex);
   osal_free(mutex);
}

void osal_mutex_lock(void *mutex)
{
   pthread_mutex_lock((osal_mutext *)mutex);
}

void osal_mutex_unlock(void *mutex)
{
   pthread_mutex_unlock((osal_mutext *)mutex);
}

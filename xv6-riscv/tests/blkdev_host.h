#include <stdint.h>
#include <string.h>
#include <pthread.h>
typedef unsigned int uint;
typedef unsigned char uchar;
typedef uint64_t uint64;
#define ROOTDEV 1
struct sleeplock { pthread_mutex_t mutex; };
static inline void initsleeplock(struct sleeplock *s, char *name)
{ (void)name; pthread_mutex_init(&s->mutex, 0); }
static inline void acquiresleep(struct sleeplock *s)
{ pthread_mutex_lock(&s->mutex); }
static inline void releasesleep(struct sleeplock *s)
{ pthread_mutex_unlock(&s->mutex); }

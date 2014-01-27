#ifndef TRACE_H
#define TRACE_H
#include "newDefines.h"
#ifdef __KERNEL__
extern int tux3_trace;
#define assert(expr)		BUG_ON(!(expr))
#define logline(fmt, ...)	printk(fmt, ##__VA_ARGS__)
#else
#define tux3_trace	1
#define logline(fmt, ...)	printf(fmt, ##__VA_ARGS__)
#endif

#define trace_off(...) do {} while (0)
#define trace_on(fmt, ...) do {						\
	if (tux3_trace && ALLOW_BUILTIN_LOG==1)				\
		logline("%s: " fmt "\n" , __func__, ##__VA_ARGS__);	\
} while (0)

#endif /* !TRACE_H */

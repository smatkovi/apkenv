#include <stdarg.h>
#include <stdio.h>

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    return 0;
}

int __android_log_write(int prio, const char *tag, const char *msg) {
    return 0;
}

void __android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
}

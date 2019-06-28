#include "serializable.h"
#include <stdio.h>

namespace bgpfsm {

ssize_t Serializable::print(uint8_t *to, size_t buf_sz) const {
    return print(0, to, buf_sz);
}

ssize_t Serializable::_print(size_t indent, uint8_t **to, size_t *buf_left, const char* format, ...) {
    if (*buf_left <= indent) return 0;
    for (size_t i = 0; i < indent; i++) {
        sprintf((char *) *to, "\t");
        *to += 1;
        *buf_left -= 1;
    }
    va_list args;
    va_start(args, format);
    ssize_t sz = vsnprintf((char *) *to, *buf_left, format, args);
    va_end(args);

    if (sz < 0) return sz;
    if ((size_t) sz > *buf_left) return *buf_left;

    *buf_left -= sz;
    *to += sz;

    return sz;
}

}
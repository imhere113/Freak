#ifndef __UTILS_H__
#define __UTILS_H__

#define ARRAY_SIZE(ary) (sizeof((ary))/sizeof(*(ary)))

#define container_of(ptr, type, member) \
    (type *)((char *)(ptr) - (char *) &((type *)0)->member)

#define min(x, y) ({                \
    typeof(x) _min1 = (x);          \
    typeof(y) _min2 = (y);          \
    (void) (&_min1 == &_min2);      \
    _min1 < _min2 ? _min1 : _min2; })

#endif /* __UTILS_H__ */
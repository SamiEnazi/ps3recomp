/*
 * ps3recomp - a log cap that cannot wrap
 *
 * PS3_LOG_FIRST(ctr, limit) is true for the first `limit` evaluations and
 * false from then on. `if (__atomic_fetch_add(&n, 1, ...) < limit)` and
 * `if (n++ < limit)` keep counting on every call, so on a hot path a signed
 * int counter passes INT_MAX, wraps negative, and the line prints on every
 * call from then on (millions of lines in a long session). This one stops
 * counting at the limit. Callers racing on the same counter can carry it past
 * the limit by one each, never around. As sp00nznet/ps3recomp#364.
 *
 * `ctr` is a plain integer lvalue (usually a function-local static); `limit`
 * is evaluated up to twice.
 */
#ifndef PS3EMU_LOG_FIRST_H
#define PS3EMU_LOG_FIRST_H

#define PS3_LOG_FIRST(ctr, limit)                                   \
    (__atomic_load_n(&(ctr), __ATOMIC_RELAXED) < (limit) &&         \
     __atomic_fetch_add(&(ctr), 1, __ATOMIC_RELAXED) < (limit))

#endif /* PS3EMU_LOG_FIRST_H */

#pragma once
#include <cstddef>
#include <cstring>
#include <deque>
#include <vector>
struct queue_t { size_t size=0, capacity=0; std::deque<std::vector<unsigned char>> items; };
inline void queue_init(queue_t *q, size_t size, size_t capacity) { q->size=size;q->capacity=capacity;q->items.clear(); }
inline bool queue_is_empty(queue_t *q) { return q->items.empty(); }
inline bool queue_is_full(queue_t *q) { return q->items.size()>=q->capacity; }
inline bool queue_try_add(queue_t *q, const void *p) {
    if (queue_is_full(q)) return false;
    const auto *b=static_cast<const unsigned char *>(p);q->items.emplace_back(b,b+q->size);return true;
}
inline bool queue_try_remove(queue_t *q, void *p) {
    if (queue_is_empty(q)) return false;
    if(p) std::memcpy(p,q->items.front().data(),q->size);
    q->items.pop_front();return true;
}

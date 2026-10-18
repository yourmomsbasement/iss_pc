// SPDX-License-Identifier: GPL-2.0-or-later
// LeastRecentlyUsedCache::ForEachItemBelow (common/lru_cache.h): a callback that returns true
// stops the walk. The texture cache's collector relies on it to end a pass after its deletion
// budget (and after 256 images); without it every pass walked the whole list of old images.
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <vector>
#include "common/lru_cache.h"

int main() {
    Common::LeastRecentlyUsedCache<int, std::uint64_t> lru;
    for (int i = 0; i < 100; ++i) {
        lru.Insert(i, static_cast<std::uint64_t>(i));
    }

    // Returning true stops after that item.
    std::vector<int> seen;
    lru.ForEachItemBelow(1000, [&](int obj) {
        seen.push_back(obj);
        return seen.size() == 10;
    });
    assert(seen.size() == 10);
    for (int i = 0; i < 10; ++i) {
        assert(seen[i] == i);
    }

    // Returning false visits every item at or below the tick, oldest first.
    seen.clear();
    lru.ForEachItemBelow(49, [&](int obj) {
        seen.push_back(obj);
        return false;
    });
    assert(seen.size() == 50);

    // A void callback visits them too.
    int count = 0;
    lru.ForEachItemBelow(1000, [&](int) { ++count; });
    assert(count == 100);

    // Freeing the current item from the callback is allowed (the collector does it).
    std::vector<std::size_t> ids;
    Common::LeastRecentlyUsedCache<std::size_t, std::uint64_t> owned;
    for (std::size_t i = 0; i < 8; ++i) {
        ids.push_back(owned.Insert(i, i));
    }
    count = 0;
    owned.ForEachItemBelow(100, [&](std::size_t obj) {
        owned.Free(ids[obj]);
        return ++count == 3;
    });
    assert(count == 3);
    count = 0;
    owned.ForEachItemBelow(100, [&](std::size_t) { ++count; });
    assert(count == 5);
    std::puts("lru-cache-test: ok");
    return 0;
}

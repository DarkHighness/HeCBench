//
// Copyright 2004-present Facebook. All Rights Reserved.
//

#include <stdlib.h>
#include <assert.h>
#include <iterator>
#include <vector>
#include "common.h"

using namespace std;

#define WARP_SIZE 32

/// A simple pair type for CUDA device usage
template <typename K, typename V>
struct Pair {
  inline Pair() {}

  inline Pair(K key, V value) : k(key), v(value) {}

  inline bool operator==(const Pair<K, V> &rhs) const {
    return (k == rhs.k) && (v == rhs.v);
  }

  inline bool operator!=(const Pair<K, V> &rhs) const {
    return !operator==(rhs);
  }

  inline bool operator<(const Pair<K, V> &rhs) const {
    return (k < rhs.k) || ((k == rhs.k) && (v < rhs.v));
  }

  inline bool operator>(const Pair<K, V> &rhs) const {
    return (k > rhs.k) || ((k == rhs.k) && (v > rhs.v));
  }

  K k;
  V v;
};

/**
   Extract a single bit at `pos` from `val`
*/

inline int getBit(int val, int pos) {
  return (val >> pos) & 0x1;
}

/**
   Return the current thread's lane in the warp
*/
inline int getLaneId(sycl::nd_item<1> &item) {
  return item.get_local_id(0) % WARP_SIZE;
}

template <typename T>
struct GreaterThan {
  static inline bool compare(const T lhs, const T rhs) {
    return (lhs > rhs);
  }
};

template <typename T>
struct LessThan {
  static inline bool compare(const T lhs, const T rhs) {
    return (lhs < rhs);
  }
};

template <typename T>
inline T shfl_xor(const T val, int laneMask, sycl::nd_item<1> &item,
                  int width = WARP_SIZE) {
  return sycl::permute_group_by_xor(item.get_sub_group(), val, laneMask);
}

template <typename K, typename V>
inline Pair<K, V> shfl_xor(const Pair<K, V> &p, int laneMask,
                           sycl::nd_item<1> &item,
                           int width = WARP_SIZE) {
  return Pair<K, V>(
      sycl::permute_group_by_xor(item.get_sub_group(), p.k, laneMask),
      sycl::permute_group_by_xor(item.get_sub_group(), p.v, laneMask));
}

template <typename T, typename Comparator>
inline T shflSwap(const T x, int mask, int dir, sycl::nd_item<1> &item) {
  T y = shfl_xor(x, mask, item);
  return Comparator::compare(x, y) == dir ? y : x;
}

/// Defines a bitonic sort network to exchange 'V' according to
/// `SWAP()`'s compare and exchange mechanism across the warp, ordered
/// according to the comparator `comp`. In other words, if `comp` is
/// `GreaterThan<T>`, then lane 0 will contain the highest `val`
/// presented across the warp
///
/// See also 
/// http://on-demand.gputechconf.com/gtc/2013/presentations/S3174-Kepler-Shuffle-Tips-Tricks.pdf
template <typename T, typename Comparator>
T warpBitonicSort(T val, sycl::nd_item<1> &item) {
  const int laneId = getLaneId(item);
  // 2
  val = shflSwap<T, Comparator>(val, 0x01, getBit(laneId, 1) ^ getBit(laneId, 0), item);

  // 4
  val = shflSwap<T, Comparator>(val, 0x02, getBit(laneId, 2) ^ getBit(laneId, 1), item);
  val = shflSwap<T, Comparator>(val, 0x01, getBit(laneId, 2) ^ getBit(laneId, 0), item);

  // 8
  val = shflSwap<T, Comparator>(val, 0x04, getBit(laneId, 3) ^ getBit(laneId, 2), item);
  val = shflSwap<T, Comparator>(val, 0x02, getBit(laneId, 3) ^ getBit(laneId, 1), item);
  val = shflSwap<T, Comparator>(val, 0x01, getBit(laneId, 3) ^ getBit(laneId, 0), item);

  // 16
  val = shflSwap<T, Comparator>(val, 0x08, getBit(laneId, 4) ^ getBit(laneId, 3), item);
  val = shflSwap<T, Comparator>(val, 0x04, getBit(laneId, 4) ^ getBit(laneId, 2), item);
  val = shflSwap<T, Comparator>(val, 0x02, getBit(laneId, 4) ^ getBit(laneId, 1), item);
  val = shflSwap<T, Comparator>(val, 0x01, getBit(laneId, 4) ^ getBit(laneId, 0), item);

  // 32
  val = shflSwap<T, Comparator>(val, 0x10, getBit(laneId, 4), item);
  val = shflSwap<T, Comparator>(val, 0x08, getBit(laneId, 3), item);
  val = shflSwap<T, Comparator>(val, 0x04, getBit(laneId, 2), item);
  val = shflSwap<T, Comparator>(val, 0x02, getBit(laneId, 1), item);
  val = shflSwap<T, Comparator>(val, 0x01, getBit(laneId, 0), item);

  return val;
}

/// Determine if two warp threads have the same value (a collision).
template <typename T>
inline bool warpHasCollision(T val, sycl::nd_item<1> &item) {
  // -sort all values
  // -compare our lower neighbor's value against ourselves (excepting
  //  the first lane)
  // -if any lane as a difference of 0, there is a duplicate
  //  (excepting the first lane)
  val = warpBitonicSort<T, LessThan<T>>(val, item);
  const T lower = sycl::shift_group_right(item.get_sub_group(), val, 1);

  // Shuffle for lane 0 will present its same value, so only
  // subsequent lanes will detect duplicates
  const bool dup = (lower == val) && (getLaneId(item) != 0);
  return (sycl::any_of_group(item.get_sub_group(), dup) != 0);
}

/// Determine if two warp threads have the same value (a collision),
/// and returns a bitmask of the lanes that are known to collide with
/// other lanes. Not all lanes that are mutually colliding return a
/// bit; all lanes with a `1` bit are guaranteed to collide with a
/// lane with a `0` bit, so the mask can be used to serialize
/// execution for lanes that collide with others.
/// (mask | (mask >> 1)) will yield all mutually colliding lanes.
template <typename T>
inline unsigned int warpCollisionMask(T val, sycl::nd_item<1> &item) {
  // -sort all (lane, value) pairs on value
  // -compare our lower neighbor's value against ourselves (excepting
  //  the first lane)
  // -if any lane as a difference of 0, there is a duplicate
  //  (excepting the first lane)
  // -shuffle sort (originating lane, dup) pairs back to the original
  //  lane and report
  Pair<T, int> pVal(val, getLaneId(item));

  pVal = warpBitonicSort<Pair<T, int>, LessThan<Pair<T, int>>>(pVal, item);

  // If our neighbor is the same as us, we know our thread's value is
  // duplicated. All except for lane 0, since shfl will present its
  // own value (and if lane 0's value is duplicated, lane 1 will pick
  // that up)
  const T lower = sycl::shift_group_right(item.get_sub_group(), pVal.k, 1);
  Pair<int, bool> dup(pVal.v, (lower == pVal.k) && (getLaneId(item) != 0));

  // Sort back based on lane ID so each thread originally knows
  // whether or not it duplicated
  dup = warpBitonicSort<Pair<int, bool>, LessThan<Pair<int, bool>>>(dup, item);

  auto sg = item.get_sub_group();
  return sycl::reduce_over_group(sg, dup.v ? (0x1 << sg.get_local_linear_id()) : 0,
                                 sycl::plus<>());
}

void checkDuplicates(sycl::nd_item<1> &item,
                     int num, const int* __restrict v,
                     int * __restrict hasDuplicate) {
  int lid = item.get_local_id(0);
  hasDuplicate[lid] = (int)warpHasCollision(v[lid], item);
}

void checkDuplicateMask(sycl::nd_item<1> &item,
                        int num, const int *__restrict v, 
                        unsigned int *__restrict duplicateMask) {
  int lid = item.get_local_id(0);
  unsigned int mask = warpCollisionMask(v[lid], item);
  if (lid == 0) {
    *duplicateMask = mask;
  }
}

vector<int> checkDuplicates(sycl::queue &q, const vector<int> &v) {
  unsigned int v_size = v.size();
  sycl::buffer<int, 1> d_set (v.data(), v_size);
  sycl::buffer<int, 1> d_hasDuplicate (32);

  sycl::range<1> gws (32);
  sycl::range<1> lws (32);
  q.submit([&](sycl::handler &cgh) {
    auto s = d_set.get_access<sycl_read>(cgh);
    auto d = d_hasDuplicate.get_access<sycl_write>(cgh);
    cgh.parallel_for(sycl::nd_range<1>(gws, lws),
        [=](sycl::nd_item<1> item) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
      checkDuplicates(item, v_size, s.get_pointer(), d.get_pointer());
    });
  });

  vector<int> hasDuplicates(32, false);
  q.submit([&](sycl::handler &cgh) {
    auto acc = d_hasDuplicate.get_access<sycl_read>(cgh);
    cgh.copy(acc, hasDuplicates.data());
  }).wait();

  return hasDuplicates;
}

unsigned int checkDuplicateMask(sycl::queue &q, const vector<int> &v) {
  unsigned int v_size = v.size();
  sycl::buffer<int, 1> d_set (v.data(), v_size);
  sycl::buffer<unsigned int, 1> d_duplicateMask (1);

  sycl::range<1> gws (32);
  sycl::range<1> lws (32);
  q.submit([&](sycl::handler &cgh) {
    auto s = d_set.get_access<sycl_read>(cgh);
    auto d = d_duplicateMask.get_access<sycl_write>(cgh);
    cgh.parallel_for(sycl::nd_range<1>(gws, lws),
        [=](sycl::nd_item<1> item) [[sycl::reqd_sub_group_size(WARP_SIZE)]] {
      checkDuplicateMask(item, v_size, s.get_pointer(), d.get_pointer());
    });
  });

  unsigned int mask;
  q.submit([&](sycl::handler &cgh) {
    auto acc = d_duplicateMask.get_access<sycl_read>(cgh);
    cgh.copy(acc, &mask);
  }).wait();

  return mask;
}

void test_collision(sycl::queue &q, const int ND) {
  for (int numDups = 0; numDups < ND; ++numDups) {

    vector<int> v;
    for (int i = 0; i < ND - numDups; ++i) {
      int r = 0;

      while (true) {
        r = rand();

        bool found = false;
        for (unsigned int j = 0; j < v.size(); ++j) {
          if (v[j] == r) {
            found = true;
            break;
          }
        }

        if (!found) {
          break;
        }
      }

      v.push_back(r);
    }

    for (int i = 0; i < numDups; ++i) {
      v.push_back(v[0]);
    }

    assert(ND == v.size());
    auto dupCheck = checkDuplicates(q, v);

    for (auto dup : dupCheck) {
      assert((numDups > 0) == dup);
    }
  }
}

void test_collisionMask(sycl::queue &q, const int ND) {
  for (int numDups = 0; numDups < ND; ++numDups) {
    vector<int> v;
    for (int i = 0; i < ND - numDups; ++i) {
      int r = 0;

      while (true) {
        r = rand();

        bool found = false;
        for (unsigned int j = 0; j < v.size(); ++j) {
          if (v[j] == r) {
            found = true;
            break;
          }
        }

        if (!found) {
          break;
        }
      }

      v.push_back(r);
    }

    for (int i = 0; i < numDups; ++i) {
      v.push_back(v[0]);
    }

    assert (ND == v.size());

    auto mask = checkDuplicateMask(q, v);
    auto expected = numDups > 0 ? 0xffffffffU << (ND - numDups) : 0;
    if (expected != mask) {
      printf("Error: numDups=%d expected=%x mask=%x\n", numDups, expected, mask);
      break;
    }
    if (numDups == 1) break;
  }
}

int main(int argc, char* argv[]) {
  srand(123);
  const int num_dup = 32;

#ifdef USE_GPU
  sycl::gpu_selector dev_sel;
#else
  sycl::cpu_selector dev_sel;
#endif
  sycl::queue q(dev_sel);
  
  test_collision(q, num_dup);
  test_collisionMask(q, num_dup);
  return 0;
}


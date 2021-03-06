// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include "kudu/tablet/rowset_info.h"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <string>
#include <utility>

#include <glog/logging.h>
#include <inttypes.h>

#include "kudu/gutil/casts.h"
#include "kudu/gutil/endian.h"
#include "kudu/gutil/map-util.h"
#include "kudu/gutil/strings/util.h"
#include "kudu/tablet/rowset.h"
#include "kudu/tablet/rowset_tree.h"
#include "kudu/util/logging.h"
#include "kudu/util/slice.h"

using std::shared_ptr;
using std::unordered_map;
using std::vector;

// Enforce a minimum size of 1MB, since otherwise the knapsack algorithm
// will always pick up small rowsets no matter what.
static const int kMinSizeMb = 1;

namespace kudu {
namespace tablet {

namespace {

// Less-than comparison by minimum key (both by actual encoded key and cdf)
bool LessCDFAndRSMin(const RowSetInfo& a, const RowSetInfo& b) {
  return a.cdf_min_key() < b.cdf_min_key() && a.min_key().compare(b.min_key()) < 0;
}

// Less-than comparison by maximum key (both by actual key slice and cdf)
bool LessCDFAndRSMax(const RowSetInfo& a, const RowSetInfo& b) {
  return a.cdf_max_key() < b.cdf_max_key() && a.max_key().compare(b.max_key()) < 0;
}

// Debug-checks that min <= imin <= imax <= max
void DCheckInside(const Slice& min, const Slice& max,
                 const Slice& imin, const Slice& imax) {
  DCHECK_LE(min.compare(max), 0);
  DCHECK_LE(imin.compare(imax), 0);
  DCHECK_LE(min.compare(imin), 0);
  DCHECK_LE(imax.compare(max), 0);
}

// Return the number of bytes of common prefix shared by 'min' and 'max'
int CommonPrefix(const Slice& min, const Slice& max) {
  int min_len = std::min(min.size(), max.size());
  int common_prefix = 0;
  while (common_prefix < min_len &&
         min[common_prefix] == max[common_prefix]) {
    ++common_prefix;
  }
  return common_prefix;
}

void DCheckCommonPrefix(const Slice& min, const Slice& imin,
                        const Slice& imax, int common_prefix) {
  DCHECK_EQ(memcmp(min.data(), imin.data(), common_prefix), 0)
    << "slices should share common prefix:\n"
    << "\t" << KUDU_REDACT(min.ToDebugString()) << "\n"
    << "\t" << KUDU_REDACT(imin.ToDebugString());
  DCHECK_EQ(memcmp(min.data(), imax.data(), common_prefix), 0)
    << "slices should share common prefix:\n"
    << "\t" << KUDU_REDACT(min.ToDebugString()) << "\n"
    << "\t" << KUDU_REDACT(imin.ToDebugString());
}

// 字串中->int，从start处算起
uint64_t SliceTailToInt(const Slice& slice, int start) {
  uint64_t ret = 0;
  DCHECK_GE(start, 0);
  DCHECK_LE(start, slice.size());
  memcpy(&ret, &slice.data()[start], std::min(slice.size() - start, sizeof(ret)));
  ret = BigEndian::ToHost64(ret);
  return ret;
}

// Finds fraction (imin, imax) takes up of rs->GetBounds().
// Requires that (imin, imax) is contained in rs->GetBounds().
// 计算区间[imin, imax]在rowset区间中占的比例
double StringFractionInRange(const RowSetInfo* rsi,
                             const Slice& imin,
                             const Slice& imax) {
  Slice min(rsi->min_key());
  Slice max(rsi->max_key());
  if (!rsi->has_bounds()) {
    VLOG(2) << "Ignoring " << rsi->rowset()->ToString() << " in CDF calculation";
    return 0;
  }
  DCheckInside(min, max, imin, imax);

  int common_prefix = CommonPrefix(min, max);
  DCheckCommonPrefix(min, imin, imax, common_prefix);

  // Convert the remaining portion of each string to an integer.
  uint64_t min_int = SliceTailToInt(min, common_prefix);
  uint64_t max_int = SliceTailToInt(max, common_prefix);
  uint64_t imin_int = SliceTailToInt(imin, common_prefix);
  uint64_t imax_int = SliceTailToInt(imax, common_prefix);

  // Compute how far between min and max the query point falls.
  if (min_int == max_int) return 0;
  return static_cast<double>(imax_int - imin_int) / (max_int - min_int);
}

// Computes the "width" of an interval [prev, next] according to the amount
// of data estimated to be inside the interval, where this is calculated by
// multiplying the fraction that the interval takes up in the keyspace of
// each rowset by the rowset's size (assumes distribution of rows is somewhat
// uniform).
// Requires: [prev, next] contained in each rowset in "active"
// 基于datasize计算区间[prev, next]的width
// 算法：计算区间占每个包含它的rowset的比例(根据key space)，然后这个比例乘以当前rowset的大小，算出一个width
//       最后把多个width累加
double WidthByDataSize(const Slice& prev, const Slice& next,
                       const unordered_map<RowSet*, RowSetInfo*>& active) {
  double weight = 0;

  for (const auto& rs_rsi : active) {
    double fraction = StringFractionInRange(rs_rsi.second, prev, next);
    weight += rs_rsi.second->size_bytes() * fraction;
  }

  return weight;
}


void CheckCollectOrderedCorrectness(const vector<RowSetInfo>& min_key,
                                    const vector<RowSetInfo>& max_key,
                                    double total_width) {
  CHECK_GE(total_width, 0);
  CHECK_EQ(min_key.size(), max_key.size());
  if (!min_key.empty()) {
    CHECK_EQ(min_key.front().cdf_min_key(), 0.0f);
    CHECK_EQ(max_key.back().cdf_max_key(), total_width);
  }
  DCHECK(std::is_sorted(min_key.begin(), min_key.end(), LessCDFAndRSMin));
  DCHECK(std::is_sorted(max_key.begin(), max_key.end(), LessCDFAndRSMax));
}

} // anonymous namespace

// RowSetInfo class ---------------------------------------------------

void RowSetInfo::Collect(const RowSetTree& tree, vector<RowSetInfo>* rsvec) {
  rsvec->reserve(tree.all_rowsets().size());
  for (const shared_ptr<RowSet>& ptr : tree.all_rowsets()) {
    rsvec->push_back(RowSetInfo(ptr.get(), 0));
  }
}

// 返回结果
// min_key: 维护了按起始端点排序的各个rowset的RowsetInfo
// max_key: 维护了按结束端点排序的各个rowset的RowsetInfo
// 对于同一个rowset在min_key里的RowsetInfo和在max_key里的RowsetInfo是相同的
void RowSetInfo::CollectOrdered(const RowSetTree& tree,
                                vector<RowSetInfo>* min_key,
                                vector<RowSetInfo>* max_key) {
  // Resize
  size_t len = tree.all_rowsets().size();
  min_key->reserve(min_key->size() + len);
  max_key->reserve(max_key->size() + len);

  // The collection process works as follows:
  // For each sorted endpoint, first we identify whether it is a
  // start or stop endpoint.
  //
  // At a start point, the associated rowset is added to the
  // "active" rowset mapping, allowing us to keep track of the index
  // of the rowset's RowSetInfo in the min_key vector.
  //
  // At a stop point, the rowset is removed from the "active" map.
  // Note that the "active" map allows access to the incomplete
  // RowSetInfo that the RowSet maps to.
  //
  // The algorithm keeps track of its state - a "sliding window"
  // across the keyspace - by maintaining the previous key and current
  // value of the total width traversed over the intervals.
  // 滑动窗口[prev, next]，是整个key空间不可再分割的最小空间（中间没有其他endpoint）
  Slice prev;
  unordered_map<RowSet*, RowSetInfo*> active; // active维护了包含滑动窗口的rowset
  double total_width = 0.0f;

  // We need to filter out the rowsets that aren't available before we process the endpoints,
  // else there's a race since we see endpoints twice and a delta compaction might finish in
  // between.
  RowSetVector available_rowsets;
  for (const shared_ptr<RowSet> rs : tree.all_rowsets()) {
    if (rs->IsAvailableForCompaction()) {
      available_rowsets.push_back(rs);
    }
  }

  RowSetTree available_rs_tree;
  available_rs_tree.Reset(available_rowsets);
  // 遍历所有endpoint（已有序），生成一个个滑动窗口
  for (const RowSetTree::RSEndpoint& rse :
                available_rs_tree.key_endpoints()) {
    RowSet* rs = rse.rowset_;
    const Slice& next = rse.slice_;
    double interval_width = WidthByDataSize(prev, next, active); // 计算滑动窗口的区间宽度(width)

    // Increment active rowsets in min_key by the interval_width.
    // 更新active中rowset的cdf_max_key_(整个区间的最新宽度）
    for (const auto& rs_rsi : active) {
      RowSetInfo& cdf_rs = *rs_rsi.second; // 引用，所以min_key里的对应rowset的RowsetInfo也会被更新
      cdf_rs.cdf_max_key_ += interval_width;
    }

    // Move sliding window
    total_width += interval_width;
    prev = next;

    // Add/remove current RowSetInfo
    // 如果端点是START，把端点对应的rowset加入active
    if (rse.endpoint_ == RowSetTree::START) {
      min_key->push_back(RowSetInfo(rs, total_width)); //
      // Store reference from vector. This is safe b/c of reserve() above.
      active.insert(std::make_pair(rs, &min_key->back()));
    } else if (rse.endpoint_ == RowSetTree::STOP) { // 端点是STOP，从端点对应的从active中删除
      // If not in active set, then STOP before START in endpoint tree
      RowSetInfo* cdf_rs = CHECK_NOTNULL(active[rs]);
      CHECK_EQ(cdf_rs->rowset(), rs) << "Inconsistent key interval tree.";
      CHECK_EQ(active.erase(rs), 1);
      max_key->push_back(*cdf_rs);
    } else {
      LOG(FATAL) << "Undefined RowSet endpoint type.\n"
                 << "\tExpected either RowSetTree::START=" << RowSetTree::START
                 << " or RowSetTree::STOP=" << RowSetTree::STOP << ".\n"
                 << "\tRecieved:\n"
                 << "\t\tRowSet=" << rs->ToString() << "\n"
                 << "\t\tKey=" << KUDU_REDACT(next.ToDebugString()) << "\n"
                 << "\t\tEndpointType=" << rse.endpoint_;
    }
  }

  CheckCollectOrderedCorrectness(*min_key, *max_key, total_width);

  FinalizeCDFVector(min_key, total_width);
  FinalizeCDFVector(max_key, total_width);
}

RowSetInfo::RowSetInfo(RowSet* rs, double init_cdf)
  : rowset_(rs),
    size_bytes_(rs->EstimateOnDiskSize()),
    size_mb_(std::max(implicit_cast<int>(size_bytes_ / 1024 / 1024), kMinSizeMb)),
    cdf_min_key_(init_cdf),
    cdf_max_key_(init_cdf) {
  has_bounds_ = rs->GetBounds(&min_key_, &max_key_).ok();
}

// 最终的cdf_key还要除以整个keyspace的width
void RowSetInfo::FinalizeCDFVector(vector<RowSetInfo>* vec,
                                 double quot) {
  if (quot == 0) return;
  for (RowSetInfo& cdf_rs : *vec) {
    CHECK_GT(cdf_rs.size_mb_, 0) << "Expected file size to be at least 1MB "
                                 << "for RowSet " << cdf_rs.rowset_->ToString()
                                 << ", was " << cdf_rs.size_bytes()
                                 << " bytes.";
    cdf_rs.cdf_min_key_ /= quot;
    cdf_rs.cdf_max_key_ /= quot;
    cdf_rs.density_ = (cdf_rs.cdf_max_key() - cdf_rs.cdf_min_key())
      / cdf_rs.size_mb_;
  }
}

string RowSetInfo::ToString() const {
  string ret;
  ret.append(rowset_->ToString());
  StringAppendF(&ret, "(% 3dM) [%.04f, %.04f]", size_mb_,
                cdf_min_key_, cdf_max_key_);
  if (has_bounds_) {
    ret.append(" [").append(KUDU_REDACT(Slice(min_key_).ToDebugString()));
    ret.append(",").append(KUDU_REDACT(Slice(max_key_).ToDebugString()));
    ret.append("]");
  }
  return ret;
}

bool RowSetInfo::Intersects(const RowSetInfo &other) const {
  if (other.cdf_min_key() > cdf_max_key()) return false;
  if (other.cdf_max_key() < cdf_min_key()) return false;
  return true;
}

} // namespace tablet
} // namespace kudu

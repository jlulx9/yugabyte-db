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
#ifndef YB_CLIENT_SCAN_PREDICATE_INTERNAL_H
#define YB_CLIENT_SCAN_PREDICATE_INTERNAL_H

#include "yb/client/value.h"
#include "yb/client/value-internal.h"
#include "yb/common/scan_spec.h"
#include "yb/gutil/macros.h"
#include "yb/util/status.h"

namespace yb {
namespace client {

class KuduPredicate::Data {
 public:
  Data();
  virtual ~Data();
  virtual Status AddToScanSpec(ScanSpec* spec) = 0;
  virtual Data* Clone() const = 0;
};

// A predicate implementation which represents an error constructing
// some other predicate.
//
// This allows us to provide a simple API -- if a predicate fails to
// construct, we return an instance of this class instead of the requested
// predicate implementation. Then, when the caller adds it to a scanner,
// the error is returned.
class ErrorPredicateData : public KuduPredicate::Data {
 public:
  explicit ErrorPredicateData(const Status& s)
  : status_(s) {
  }

  virtual ~ErrorPredicateData() {
  }

  virtual Status AddToScanSpec(ScanSpec* spec) OVERRIDE {
    return status_;
  }

  virtual ErrorPredicateData* Clone() const OVERRIDE {
    return new ErrorPredicateData(status_);
  }

 private:
  Status status_;
};


// A simple binary comparison predicate between a column and
// a constant.
class ComparisonPredicateData : public KuduPredicate::Data {
 public:
  ComparisonPredicateData(ColumnSchema col,
                          KuduPredicate::ComparisonOp op,
                          KuduValue* value);
  virtual ~ComparisonPredicateData();

  virtual Status AddToScanSpec(ScanSpec* spec) OVERRIDE;

  virtual ComparisonPredicateData* Clone() const OVERRIDE {
      return new ComparisonPredicateData(col_, op_, val_->Clone());
  }

 private:
  friend class KuduScanner;

  ColumnSchema col_;
  KuduPredicate::ComparisonOp op_;
  gscoped_ptr<KuduValue> val_;

  // Owned.
  ColumnRangePredicate* pred_;
};

} // namespace client
} // namespace yb
#endif /* YB_CLIENT_SCAN_PREDICATE_INTERNAL_H */

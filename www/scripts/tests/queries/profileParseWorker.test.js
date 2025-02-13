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

import {describe, test, expect} from '@jest/globals';
import {readFileSync} from 'node:fs';

// JEST does not support workers, so "profileParseWorker.js" cannot be tested directly
describe("Test Compression Library", () => {
  // Test whether the compression library imported by the worker script
  // properly utilizes the pako library's compression methods
  test("Basic Test", () => {
    var exampleJSONProfileText = readFileSync("../../../testdata/impala-profiles/"
        + "impala_profile_log_tpcds_compute_stats_extended.expected.pretty.json",
        {encoding : "utf-8"});
    import("../../../pako.min.js").then((pako) => {
      pako = pako.default;
      expect(pako.inflate(pako.deflate(exampleJSONProfileText, {level : 3}), {to : "string"}))
          .toBe(exampleJSONProfileText);
    });
  });
});
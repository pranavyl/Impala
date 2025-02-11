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

#ifndef IMPALA_JWT_UTIL_H
#define IMPALA_JWT_UTIL_H

#include <string>

#include "common/logging.h"
#include "common/status.h"

namespace impala {

class JWKSSnapshot;
class JWKSMgr;

/// JSON Web Token (JWT) is an Internet proposed standard for creating data with optional
/// signature and/or optional encryption whose payload holds JSON that asserts some
/// number of claims. The tokens are signed either using a private secret or a public/
/// private key.
/// This class works as wrapper for jwt-cpp. It provides APIs to decode/verify JWT token,
/// and extracts custom claim from the payload of JWT token.
/// The class is thread safe.
class JWTHelper {
 public:
  /// Opaque types for storing the JWT decoded token. This allows us to avoid including
  /// header file jwt-cpp/jwt.h.
  struct JWTDecodedToken;

  // Custom deleter: intended for use with std::unique_ptr<JWTDecodedToken>.
  class TokenDeleter {
   public:
    /// Called by unique_ptr to free JWTDecodedToken
    void operator()(JWTHelper::JWTDecodedToken* token) const;
  };
  /// UniqueJWTDecodedToken -- a wrapper around opaque decoded token structure to
  /// facilitate automatic reference counting.
  typedef std::unique_ptr<JWTDecodedToken, TokenDeleter> UniqueJWTDecodedToken;

  /// Load JWKS from a given local JSON file. Returns an error if problems were
  /// encountered.
  Status Init(const std::string& jwks_file_path);

  /// Load JWKS from a given local JSON file or URL. Returns an error if problems were
  /// encountered.
  Status Init(const std::string& jwks_uri, bool jwks_verify_server_certificate,
      const std::string& jwks_ca_certificate, bool is_local_file);

  /// Decode the given JWT token. The decoding result is stored in decoded_token_.
  /// Return Status::OK if the decoding is successful.
  static Status Decode(
      const std::string& token, UniqueJWTDecodedToken& decoded_token_out);

  /// Verify the token's signature with the JWKS. The token should be already decoded by
  /// calling Decode().
  /// Return Status::OK if the verification is successful.
  Status Verify(const JWTDecodedToken* decoded_token) const;

  /// Extract custom claim "Username" from from the payload of the decoded JWT token.
  /// Return Status::OK if the extraction is successful.
  static Status GetCustomClaimUsername(const JWTDecodedToken* decoded_token,
      const std::string& custom_claim_username, std::string& username);

  /// Return snapshot of JWKS.
  std::shared_ptr<const JWKSSnapshot> GetJWKS() const;

 private:
  /// Set it as TRUE when Init() is called.
  bool initialized_ = false;

  /// JWKS Manager for Json Web Token (JWT) verification.
  /// Only one instance per daemon.
  std::unique_ptr<JWKSMgr> jwks_mgr_;
};

} // namespace impala

#endif

/*
 * Copyright 2020 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

pub(crate) mod net;

use base64::Engine;

pub(crate) fn base64_encode<A: AsRef<[u8]>>(bytes: A) -> String {
    base64::engine::general_purpose::STANDARD.encode(bytes.as_ref())
}

pub(crate) fn base64_decode<A: AsRef<str>>(input: A) -> Result<Vec<u8>, base64::DecodeError> {
    base64::engine::general_purpose::STANDARD.decode(input.as_ref())
}

/// A type which can be logged, usually error types.
pub(crate) trait Loggable {
    /// Output a log.
    fn log(&self);
}

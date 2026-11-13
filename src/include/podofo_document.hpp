////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2026 SereneDB GmbH, Berlin, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is SereneDB GmbH, Berlin, Germany
////////////////////////////////////////////////////////////////////////////////

#pragma once

#include <duckdb/common/open_file_info.hpp>
#include <duckdb/main/client_context.hpp>
#include <duckdb/main/extension/extension_loader.hpp>
#include <podofo/main/PdfMemDocument.h>

#include <memory>
#include <string>
#include <string_view>

namespace duckdb::pdf {

std::unique_ptr<PoDoFo::PdfMemDocument> OpenDocument(ClientContext &context, const OpenFileInfo &path,
                                                     std::string_view password, std::string &bytes,
                                                     std::string_view function);

void LoadInspectionFunctions(ExtensionLoader &loader);

} // namespace duckdb::pdf
